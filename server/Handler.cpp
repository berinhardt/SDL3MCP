#include "Handler.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Png.h"
#include "SocketStream.h"
#include "StdioStream.h"
#include "ToolDefinitions.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kNoAppError =
    "no application session is attached; call wait_session first";
constexpr std::int64_t kMaxTimeoutMs = 60000;
/*
  A call's own timeout_ms is a budget for the application's answer, so the
  Handler gives it a little slack on top: the application is the authority on
  when it has nothing to send, and the Handler only bounds the wait.
*/
constexpr std::int64_t kForwardGraceMs = 500;

constexpr std::string_view kSpawnSchema = R"({
  "type": "object",
  "properties": {
    "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
  },
  "additionalProperties": false
})";

constexpr std::string_view kSpawnAnnotations = R"({
  "readOnlyHint": false,
  "idempotentHint": false,
  "openWorldHint": true
})";

std::string spawnDescription(std::string_view alias) {
   return "Start the application configured for the alias " + std::string(alias) +
          " and wait for it to connect and complete the MCP handshake. The "
          "application is started with --mcp=<the Handler's listener endpoint> "
          "appended to its configured argv. One application at a time: a call "
          "while one is attached or starting fails. Arguments: optional timeout_ms "
          "(1-60000, default the Handler's --timeout-ms). Returns "
          "`spawned <alias> (pid N)`; a timeout or exec failure names the reason "
          "and the pid, and the process is not stopped (spawn-and-forget). "
          "Example: {\"timeout_ms\":5000}";
}

/*
  A spawned app is not the Handler's to reap: SA_NOCLDWAIT makes the kernel
  release the child as soon as it exits, and the Handler tracks the session
  rather than the process.
*/
void installChildReaping() {
   struct sigaction action{};
   action.sa_handler = SIG_DFL;
   sigemptyset(&action.sa_mask);
   action.sa_flags = SA_NOCLDWAIT;
   (void)::sigaction(SIGCHLD, &action, nullptr);
}

/*
  Keeps the child from inheriting the Handler's client stream, upstream socket,
  or listener. close_range is one syscall, so it is safe between fork and exec;
  the rlimit loop is the fallback for a kernel that does not have it.
*/
void closeDescriptorsAbove(int lowest) {
   if (::close_range(static_cast<unsigned int>(lowest), ~0U, 0) == 0) {
      return;
   }

   struct rlimit limit{};
   if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      return;
   }
   for (rlim_t fd = static_cast<rlim_t>(lowest); fd < limit.rlim_cur; ++fd) {
      ::close(static_cast<int>(fd));
   }
}

constexpr std::string_view kWaitDescription =
    "Wait for an application to connect to the listener. Arguments: optional "
    "timeout_ms (1-60000, default the Handler's --timeout-ms). Returns as soon as "
    "a session is attached; an expired timeout is a normal tool error. The "
    "application's tools are advertised alongside this one whether or not it is "
    "attached. Example: {\"timeout_ms\":5000}";

constexpr std::string_view kWaitSchema = R"({
  "type": "object",
  "properties": {
    "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
  },
  "additionalProperties": false
})";

constexpr std::string_view kWaitAnnotations = R"({
  "readOnlyHint": true,
  "idempotentHint": false,
  "openWorldHint": true
})";

constexpr std::string_view kHandlerInputDescription =
    "Queue a batch of synthetic input actions; the call returns the frame that "
    "follows the batch: text naming both the frame the actions were applied at "
    "and the frame attached, plus that frame's pixels, presented as an image/png "
    "part. The batch is applied atomically at the next frame and the call returns "
    "after the following frame, once the application has consumed the generated "
    "events. Arguments: actions (required; 1-256 action objects), optional region, "
    "and optional timeout_ms (1-60000, default the Handler's --timeout-ms) "
    "bounding the whole call. Each action object is one of six types: key {key "
    "name or chord, action=down|up|press (default press), "
    "modifiers=[ctrl|shift|alt|gui]}; text {text, 1-1024 UTF-8 bytes}; mouse_move "
    "{x,y in window points or dx,dy relative}; mouse_button "
    "{button=left|middle|right|x1|x2, action=down|up|click (default click), "
    "clicks=1-3, optional x,y}; scroll {y in notches, x default 0}; release_all. "
    "The optional region crops the returned frame in pixels of the full frame "
    "with a top-left origin. Example: "
    "{\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"}],"
    "\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}";

constexpr std::string_view kHandlerCaptureDescription =
    "Ask the application for a fresh frame and return it as a PNG image block at "
    "the app's native pixel size. Arguments: optional timeout_ms (1-60000, "
    "default the Handler's --timeout-ms) and optional region. region crops pixels "
    "from the captured frame with a top-left origin and must fit inside "
    "capture.lastWidth x capture.lastHeight from sdl_info. sdl_input coordinates "
    "are window points, while region is in pixels of the full frame; sdl_info "
    "reports pixelWidth, pixelHeight and pixelDensity so a caller can convert "
    "between them. Captures are never resized, so region is the only way to keep "
    "the payload small. The text part names the frame attached, so a caller can "
    "tell a fresh frame from a stale one. Example: "
    "{\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360},"
    "\"timeout_ms\":2000}";

std::string endpointText(std::string_view host, std::uint16_t port) {
   std::string text;
   if (host.find(':') != std::string_view::npos) {
      text = "[" + std::string(host) + "]";
   } else {
      text = std::string(host);
   }
   return text + ":" + std::to_string(port);
}

nlohmann::json parseDefinition(std::string_view text) {
   return nlohmann::json::parse(text.begin(), text.end());
}

/*
  Prints one argv the way a shell would read it. A word made only of characters
  a shell leaves alone is printed bare; anything else is single-quoted, with an
  embedded quote written as '\'' so the line stays unambiguous.
*/
std::string shellQuote(std::string_view argument) {
   const auto isSafe = [](char character) {
      return (character >= 'a' && character <= 'z') ||
             (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9') || character == '_' ||
             character == '-' || character == '.' || character == '/' ||
             character == ':' || character == '=';
   };
   if (!argument.empty() &&
       std::all_of(argument.begin(), argument.end(), isSafe)) {
      return std::string(argument);
   }

   std::string quoted = "'";
   for (const char character : argument) {
      if (character == '\'') {
         quoted += "'\\''";
      } else {
         quoted += character;
      }
   }
   quoted += "'";
   return quoted;
}

/* Reads a JSON integer, including unsigned values that still fit in int64_t. */
bool readInteger(const nlohmann::json& object,
                 const char* name,
                 std::int64_t& value) {
   const auto entry = object.find(name);
   if (entry == object.end() || !entry->is_number_integer()) {
      return false;
   }
   if (entry->is_number_unsigned()) {
      const std::uint64_t raw = entry->get<std::uint64_t>();
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
         return false;
      }
      value = static_cast<std::int64_t>(raw);
      return true;
   }
   value = entry->get<std::int64_t>();
   return true;
}

nlohmann::json objectField(const nlohmann::json& entry,
                           const char* name,
                           nlohmann::json fallback) {
   const auto value = entry.find(name);
   if (value == entry.end() || !value->is_object()) {
      return fallback;
   }
   return *value;
}

std::uint64_t frameIndexOf(const nlohmann::json& header) {
   const auto frame = header.find("frame");
   if (frame == header.end() || !frame->is_number_integer()) {
      return 0;
   }
   if (frame->is_number_unsigned()) {
      return frame->get<std::uint64_t>();
   }
   const std::int64_t signedValue = frame->get<std::int64_t>();
   return signedValue > 0 ? static_cast<std::uint64_t>(signedValue) : 0;
}

/*
  The image summary names the frame the app sent, its region when one was given,
  and the PNG size, so a caller can tell a fresh attachment from a stale one.
*/
std::string captureSummary(const nlohmann::json& arguments,
                           int width,
                           int height,
                           std::size_t pngBytes,
                           std::uint64_t frame) {
   std::string text = std::to_string(width) + "x" + std::to_string(height) + " PNG";

   const auto region = arguments.find("region");
   std::int64_t x = 0;
   std::int64_t y = 0;
   if (region != arguments.end() && region->is_object() &&
       readInteger(*region, "x", x) && readInteger(*region, "y", y)) {
      text += " at " + std::to_string(x) + "," + std::to_string(y);
   }

   text += ", " + std::to_string(pngBytes) + " bytes";
   if (frame > 0) {
      text += "; frame " + std::to_string(frame) + " attached";
   }
   return text;
}

}  // namespace

Handler::Handler(HandlerOptions options) : options_(std::move(options)) {
}

Handler::~Handler() {
   shutdown();
}

int Handler::run() {
   /*
     Writes to a closed pipe or socket raise SIGPIPE; the stream reports the
     failure as a value instead, so the default termination is never useful.
   */
   ::signal(SIGPIPE, SIG_IGN);
   installChildReaping();

   std::string error;
   if (!loadTargets(error)) {
      std::fprintf(stderr, "sdl3mcp: %s\n", error.c_str());
      std::fflush(stderr);
      return 2;
   }
   if (options_.hasDryRun) {
      return printDryRun();
   }

   buildBaseTools();
   rebuildCatalog();

   if (!startClient(error)) {
      std::fprintf(stderr, "sdl3mcp: %s\n", error.c_str());
      std::fflush(stderr);
      return 1;
   }
   if (!startListener(error)) {
      std::fprintf(stderr, "sdl3mcp: %s\n", error.c_str());
      std::fflush(stderr);
      shutdown();
      return 1;
   }
   std::fprintf(stderr, "sdl3mcp: listening on %s\n",
                endpointText(options_.listenHost, listener_.boundPort()).c_str());
   std::fflush(stderr);

   startSignalFd();
   if (signalFd_ < 0) {
      std::fprintf(stderr, "sdl3mcp: cannot install the signal handler: %s\n",
                   std::strerror(errno));
      std::fflush(stderr);
      shutdown();
      return 1;
   }

   while (running_) {
      std::array<pollfd, 5> descriptors{};
      descriptors[0].fd = clientFd_;
      descriptors[0].events = POLLIN;
      if (clientSession_ && clientSession_->stream().wantsWrite()) {
         descriptors[0].events |= POLLOUT;
      }
      descriptors[1].fd = listener_.fd();
      descriptors[1].events = POLLIN;
      descriptors[2].fd = appFd_;
      descriptors[2].events = POLLIN;
      if (appPeer_ && appPeer_->wantsWrite()) {
         descriptors[2].events |= POLLOUT;
      }
      descriptors[3].fd = signalFd_;
      descriptors[3].events = POLLIN;
      descriptors[4].fd =
          pendingSpawn_.has_value() ? pendingSpawn_->errorFd : -1;
      descriptors[4].events = POLLIN;

      const auto beforePoll = Clock::now();
      const int ready = ::poll(descriptors.data(), descriptors.size(),
                               nextPollTimeoutMs(beforePoll));
      if (ready < 0) {
         if (errno == EINTR) {
            continue;
         }
         std::fprintf(stderr, "sdl3mcp: poll failed: %s\n", std::strerror(errno));
         std::fflush(stderr);
         break;
      }

      const auto now = Clock::now();

      if ((descriptors[3].revents & POLLIN) != 0) {
         signalfd_siginfo info{};
         while (::read(signalFd_, &info, sizeof info) > 0) {
         }
         running_ = false;
      }

      if (running_ && (descriptors[1].revents & POLLIN) != 0) {
         acceptApplication();
      }

      if (running_ && clientSession_ != nullptr &&
          (descriptors[0].revents &
           (POLLIN | POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0) {
         clientSession_->pump();
         if (!clientSession_->isOpen()) {
            running_ = false;
         }
      }

      if (appPeer_ != nullptr &&
          (descriptors[2].revents &
           (POLLIN | POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0) {
         appPeer_->pump();
      }

      if (pendingSpawn_.has_value() &&
          (descriptors[4].revents &
           (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
         serviceSpawnExec();
      }

      if (running_) {
         expireWaiters(now);
         expireForwards(now);
         expireSpawn(now);
         if (appPeer_ != nullptr && !appPeer_->isReady() &&
             !appPeer_->isClosed() && now >= appPeer_->handshakeDeadline()) {
            detachApp("the application did not complete the MCP handshake in time");
         }
      }

      if (appPeer_ != nullptr && appPeer_->isClosed()) {
         appPeer_.reset();
         appFd_ = -1;
         appReady_ = false;
      }

      if (clientSession_ != nullptr && !clientSession_->isOpen()) {
         /* The client went away; its completions are already cancelled. */
         waiters_.clear();
         forwards_.clear();
         cancelSpawn();
      }
   }

   shutdown();
   return 0;
}

bool Handler::loadTargets(std::string& error) {
   targets_.clear();
   error.clear();
   if (options_.targetsPaths.empty()) {
      return true;
   }
   return loadSpawnTargets(options_.targetsPaths, targets_, error);
}

int Handler::printDryRun() {
   const SpawnTarget* target = nullptr;
   for (const SpawnTarget& candidate : targets_) {
      if (candidate.alias == options_.dryRunAlias) {
         target = &candidate;
         break;
      }
   }
   if (target == nullptr) {
      std::fprintf(stderr, "sdl3mcp: --dry-run: unknown alias \"%s\"\n",
                   options_.dryRunAlias.c_str());
      std::fflush(stderr);
      return 2;
   }

   /*
     A dry run binds nothing, so a port still at zero is the endpoint the spawn
     would have been told about: print the placeholder rather than a guess.
   */
   const std::string endpoint =
       options_.listenPort == 0
           ? std::string("HOST:PORT")
           : endpointText(options_.listenHost, options_.listenPort);

   std::string line = "command:";
   for (const std::string& argument : targetCommand(*target, endpoint)) {
      line += " " + shellQuote(argument);
   }
   std::printf("%s\n", line.c_str());
   std::fflush(stdout);
   return 0;
}

void Handler::buildBaseTools() {
   baseTools_.clear();

   baseTools_.push_back(Tool{
       "wait_session", std::string(kWaitDescription),
       parseDefinition(kWaitSchema), parseDefinition(kWaitAnnotations),
       [this](ToolOrder order, ToolCompletionDelegate& completion) {
          handleWaitSession(order, completion);
       }});

   for (const ToolDefinition& definition : applicationToolDefinitions()) {
      ToolDefinition handlerDefinition = definition;
      if (definition.name == "sdl_input" || definition.name == "sdl_capture") {
         handlerDefinition.description =
             definition.name == "sdl_input" ? std::string(kHandlerInputDescription)
                                            : std::string(kHandlerCaptureDescription);
         handlerDefinition.inputSchema["properties"]["timeout_ms"] =
             nlohmann::json{{"type", "integer"},
                            {"minimum", 1},
                            {"maximum", kMaxTimeoutMs}};
      }

      baseTools_.push_back(Tool{
          handlerDefinition.name, handlerDefinition.description,
          handlerDefinition.inputSchema, handlerDefinition.annotations,
          [this](ToolOrder order, ToolCompletionDelegate& completion) {
             handleForward(order, completion);
          }});
   }

   /* Each app source contributes one spawn tool, whether or not an app is up. */
   for (const SpawnTarget& target : targets_) {
      const SpawnTarget spawnTarget = target;
      baseTools_.push_back(Tool{
          "app_spawn_" + target.alias, spawnDescription(target.alias),
          parseDefinition(kSpawnSchema), parseDefinition(kSpawnAnnotations),
          [this, spawnTarget](ToolOrder order, ToolCompletionDelegate& completion) {
             handleSpawn(order, completion, spawnTarget);
          }});
   }
}

void Handler::rebuildCatalog() {
   catalog_.clear();

   for (const Tool& tool : baseTools_) {
      /*
        sdl_capture and sdl_input carry the Handler's forward budget, so their
        definitions stay the Handler's. The other base entries take the peer's
        metadata but keep the Handler's forward invoke.
      */
      if (tool.name == "sdl_capture" || tool.name == "sdl_input") {
         catalog_.add(tool);
         continue;
      }

      const nlohmann::json* peer = nullptr;
      for (const nlohmann::json& entry : peerTools_) {
         if (entry.is_object() &&
             entry.value("name", std::string()) == tool.name) {
            peer = &entry;
            break;
         }
      }
      if (peer == nullptr) {
         catalog_.add(tool);
         continue;
      }

      Tool merged = tool;
      if (peer->contains("description") && (*peer)["description"].is_string()) {
         merged.description = (*peer)["description"].get<std::string>();
      }
      merged.inputSchema = objectField(*peer, "inputSchema", tool.inputSchema);
      merged.annotations = objectField(*peer, "annotations", tool.annotations);
      catalog_.add(std::move(merged));
   }

   /* A name only the peer advertises is appended and forwarded like the rest. */
   for (const nlohmann::json& entry : peerTools_) {
      if (!entry.is_object() || !entry.contains("name") ||
          !entry["name"].is_string()) {
         continue;
      }
      const std::string name = entry["name"].get<std::string>();
      if (name.empty() || catalog_.find(name) != nullptr) {
         continue;
      }
      catalog_.add(Tool{
          name, entry.value("description", std::string()),
          objectField(entry, "inputSchema", nlohmann::json::object()),
          objectField(entry, "annotations", nlohmann::json::object()),
          [this](ToolOrder order, ToolCompletionDelegate& completion) {
             handleForward(order, completion);
          }});
   }
}

std::vector<nlohmann::json> Handler::mergedTools() const {
   std::vector<nlohmann::json> tools;
   for (const auto& entry : catalog_.tools()) {
      const Tool& tool = entry.second;
      tools.push_back(nlohmann::json{
          {"name", tool.name},
          {"description", tool.description},
          {"inputSchema", tool.inputSchema},
          {"annotations", tool.annotations},
      });
   }
   return tools;
}

void Handler::publishToolsIfChanged(bool notify) {
   nlohmann::json current = nlohmann::json::array();
   for (nlohmann::json& tool : mergedTools()) {
      current.push_back(std::move(tool));
   }

   const bool changed = current != publishedTools_;
   publishedTools_ = std::move(current);
   if (!notify || !changed || clientSession_ == nullptr) {
      return;
   }
   clientSession_->stream().sendText(
       nlohmann::json{{"jsonrpc", "2.0"},
                      {"method", "notifications/tools/list_changed"}}
           .dump());
}

bool Handler::startClient(std::string& error) {
   error.clear();

   if (options_.relay()) {
      auto socket = std::make_unique<SocketStream>();
      SocketStream* raw = socket.get();
      auto session = std::make_unique<Session>(catalog_, std::move(socket));

      std::string connectError;
      if (!raw->connect(options_.connectHost, options_.connectPort,
                        static_cast<std::uint32_t>(options_.timeoutMs),
                        connectError)) {
         error = "--connect: " + connectError;
         return false;
      }

      clientSession_ = std::move(session);
      clientFd_ = raw->fd();
      return true;
   }

   auto stdio = std::make_unique<StdioStream>();
   StdioStream* raw = stdio.get();
   auto session = std::make_unique<Session>(catalog_, std::move(stdio));
   if (!raw->start(error)) {
      return false;
   }

   clientSession_ = std::move(session);
   clientFd_ = raw->inputFd();
   return true;
}

bool Handler::startListener(std::string& error) {
   return listener_.listen(options_.listenHost, options_.listenPort, error);
}

void Handler::startSignalFd() {
   sigset_t mask;
   sigemptyset(&mask);
   sigaddset(&mask, SIGINT);
   sigaddset(&mask, SIGTERM);
   if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
      signalFd_ = -1;
      return;
   }
   signalFd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

void Handler::acceptApplication() {
   std::string error;
   const int fd = listener_.accept(error);
   if (fd < 0) {
      if (!error.empty()) {
         std::fprintf(stderr, "sdl3mcp: %s\n", error.c_str());
         std::fflush(stderr);
      }
      return;
   }

   /*
     Exactly one application session. The listener keeps accepting only so a
     second connection is closed at once instead of waiting in the backlog.
   */
   if (appPeer_ != nullptr) {
      ::close(fd);
      return;
   }

   auto socket = std::make_unique<SocketStream>();
   std::string adoptError;
   if (!socket->adopt(fd,
                      endpointText(options_.listenHost, listener_.boundPort()),
                      adoptError)) {
      std::fprintf(stderr, "sdl3mcp: %s\n", adoptError.c_str());
      std::fflush(stderr);
      return;
   }

   auto peer = std::make_unique<AppPeer>(
       std::move(socket), *this, static_cast<std::uint32_t>(options_.timeoutMs));
   std::string startError;
   if (!peer->start(startError)) {
      std::fprintf(stderr, "sdl3mcp: %s\n", startError.c_str());
      std::fflush(stderr);
      return;
   }

   appPeer_ = std::move(peer);
   appFd_ = fd;
}

void Handler::handleWaitSession(const ToolOrder& order,
                                ToolCompletionDelegate& completion) {
   std::int64_t timeoutMs = options_.timeoutMs;
   if (order.arguments.contains("timeout_ms")) {
      std::int64_t requested = 0;
      if (!readInteger(order.arguments, "timeout_ms", requested) || requested < 1 ||
          requested > kMaxTimeoutMs) {
         completion.complete(
             ToolResult::text("wait_session timeout_ms must be in 1..60000", true));
         return;
      }
      timeoutMs = requested;
   }

   if (appReady_) {
      completion.complete(
          ToolResult::text("an application session is already attached"));
      return;
   }

   waiters_.push_back(PendingWait{
       &completion,
       Clock::now() + std::chrono::milliseconds(timeoutMs),
       timeoutMs,
   });
}

void Handler::handleSpawn(const ToolOrder& order,
                          ToolCompletionDelegate& completion,
                          const SpawnTarget& target) {
   if (appPeer_ != nullptr || appReady_ || pendingSpawn_.has_value()) {
      completion.complete(ToolResult::text(
          "an application is already attached; one spawn at a time", true));
      return;
   }

   std::int64_t timeoutMs = options_.timeoutMs;
   if (order.arguments.contains("timeout_ms")) {
      std::int64_t requested = 0;
      if (!readInteger(order.arguments, "timeout_ms", requested) || requested < 1 ||
          requested > kMaxTimeoutMs) {
         completion.complete(ToolResult::text(
             "app_spawn_" + target.alias + " timeout_ms must be in 1..60000", true));
         return;
      }
      timeoutMs = requested;
   }

   /*
     The listener is bound before the loop accepts tool calls, so its endpoint is
     the one the app is told to dial. A bound port is never zero here.
   */
   const std::string endpoint =
       endpointText(options_.listenHost, listener_.boundPort());

   std::string error;
   if (!startTarget(target, endpoint, timeoutMs, completion, error)) {
      completion.complete(
          ToolResult::text("app_spawn_" + target.alias + ": " + error, true));
   }
}

bool Handler::startTarget(const SpawnTarget& target,
                          std::string_view endpoint,
                          std::int64_t timeoutMs,
                          ToolCompletionDelegate& completion,
                          std::string& error) {
   error.clear();

   std::vector<std::string> command = targetCommand(target, endpoint);
   std::vector<char*> argv;
   argv.reserve(command.size() + 1);
   for (std::string& argument : command) {
      argv.push_back(const_cast<char*>(argument.c_str()));
   }
   argv.push_back(nullptr);

   int execPipe[2] = {-1, -1};
   if (::pipe(execPipe) != 0) {
      error = std::string("cannot create the exec channel: ") + std::strerror(errno);
      return false;
   }
   for (const int fd : execPipe) {
      const int flags = ::fcntl(fd, F_GETFD);
      if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
         error =
             std::string("cannot make the exec channel close-on-exec: ") +
             std::strerror(errno);
         ::close(execPipe[0]);
         ::close(execPipe[1]);
         return false;
      }
   }
   const int readFlags = ::fcntl(execPipe[0], F_GETFL);
   if (readFlags >= 0) {
      (void)::fcntl(execPipe[0], F_SETFL, readFlags | O_NONBLOCK);
   }

   const std::string execFailure = "cannot execute " + command.front();

   const pid_t pid = ::fork();
   if (pid < 0) {
      error = std::string("cannot fork the application: ") + std::strerror(errno);
      ::close(execPipe[0]);
      ::close(execPipe[1]);
      return false;
   }

   if (pid == 0) {
      /* Child: only async-signal-safe calls from here to execvp. */
      sigset_t empty;
      sigemptyset(&empty);
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);

      /* SA_NOCLDWAIT is the Handler's policy for its own children, not the app's. */
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      ::sigaction(SIGCHLD, &action, nullptr);
      /* The Handler ignores SIGPIPE for its own streams; an app gets the default. */
      ::sigaction(SIGPIPE, &action, nullptr);

      /* The app's stdout must not reach the Handler's MCP stream. */
      const int devNull = ::open("/dev/null", O_RDONLY);
      if (devNull >= 0) {
         ::dup2(devNull, STDIN_FILENO);
         if (devNull != STDIN_FILENO) {
            ::close(devNull);
         }
      }
      ::dup2(STDERR_FILENO, STDOUT_FILENO);

      /* Keep the exec channel on fd 3; everything else above 2 is not the app's. */
      ::dup2(execPipe[1], 3);
      (void)::fcntl(3, F_SETFD, 0);
      closeDescriptorsAbove(4);

      ::execvp(argv[0], argv.data());

      const ssize_t ignored =
          ::write(3, execFailure.data(), execFailure.size());
      (void)ignored;
      ::_exit(127);
   }

   ::close(execPipe[1]);

   PendingSpawn pending;
   pending.alias = target.alias;
   pending.pid = pid;
   pending.errorFd = execPipe[0];
   pending.completion = &completion;
   pending.deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
   pending.timeoutMs = timeoutMs;
   pendingSpawn_ = std::move(pending);
   return true;
}

/*
  The exec channel is close-on-exec: EOF means execvp succeeded and the process
  is running, while any bytes are the reason it could not exec. A failed exec is
  the only spawn outcome visible without waiting for the child, which
  spawn-and-forget forbids.
*/
void Handler::serviceSpawnExec() {
   if (!pendingSpawn_.has_value() || pendingSpawn_->errorFd < 0) {
      return;
   }

   char buffer[512];
   for (;;) {
      const ssize_t count = ::read(pendingSpawn_->errorFd, buffer, sizeof buffer);
      if (count > 0) {
         pendingSpawn_->execError.append(buffer, static_cast<std::size_t>(count));
         continue;
      }
      if (count == 0) {
         /* EOF with no message: exec succeeded, so wait for the handshake. */
         ::close(pendingSpawn_->errorFd);
         pendingSpawn_->errorFd = -1;
         if (!pendingSpawn_->execError.empty()) {
            completeSpawn("app_spawn_" + pendingSpawn_->alias +
                              " failed: " + pendingSpawn_->execError +
                              " (pid " + std::to_string(pendingSpawn_->pid) + ")",
                          true);
         }
         return;
      }
      if (errno == EINTR) {
         continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
         return;
      }

      /* A read failure is not an exec failure; let the handshake deadline decide. */
      ::close(pendingSpawn_->errorFd);
      pendingSpawn_->errorFd = -1;
      return;
   }
}

void Handler::expireSpawn(Clock::time_point now) {
   if (!pendingSpawn_.has_value() || now < pendingSpawn_->deadline) {
      return;
   }

   const std::string text =
       "app_spawn_" + pendingSpawn_->alias + " timed out after " +
       std::to_string(pendingSpawn_->timeoutMs) +
       " ms waiting for the application to connect; process pid " +
       std::to_string(pendingSpawn_->pid) + " may still be running";
   completeSpawn(text, true);
}

void Handler::completeSpawn(std::string text, bool isError) {
   if (!pendingSpawn_.has_value()) {
      return;
   }

   PendingSpawn pending = std::move(*pendingSpawn_);
   pendingSpawn_.reset();
   if (pending.errorFd >= 0) {
      ::close(pending.errorFd);
   }
   pending.completion->complete(ToolResult::text(std::move(text), isError));
}

void Handler::cancelSpawn() {
   if (!pendingSpawn_.has_value()) {
      return;
   }
   if (pendingSpawn_->errorFd >= 0) {
      ::close(pendingSpawn_->errorFd);
   }
   pendingSpawn_.reset();
}

void Handler::handleForward(const ToolOrder& order,
                            ToolCompletionDelegate& completion) {
   if (!appReady_ || appPeer_ == nullptr || appPeer_->isClosed()) {
      completion.complete(ToolResult::text(std::string(kNoAppError), true));
      return;
   }
   if (!peerAdvertises(order.name)) {
      completion.complete(
          ToolResult::text("the application does not provide " + order.name, true));
      return;
   }

   std::int64_t timeoutMs = options_.timeoutMs;
   nlohmann::json arguments = order.arguments;
   if (arguments.contains("timeout_ms")) {
      std::int64_t requested = 0;
      if (!readInteger(arguments, "timeout_ms", requested) || requested < 1 ||
          requested > kMaxTimeoutMs) {
         completion.complete(ToolResult::text(
             order.name + " timeout_ms must be in 1..60000", true));
         return;
      }
      timeoutMs = requested + kForwardGraceMs;
      arguments.erase("timeout_ms");
   }

   const std::uint64_t id = appPeer_->nextCallId();
   if (!appPeer_->sendCall(id, order.name, arguments)) {
      completion.complete(ToolResult::text(
          "cannot send " + order.name + " to the application", true));
      return;
   }

   forwards_.emplace(
       id, PendingForward{order.name, std::move(arguments), &completion,
                          Clock::now() + std::chrono::milliseconds(timeoutMs)});
}

void Handler::completeWaiters(const std::string& text) {
   for (PendingWait& waiter : waiters_) {
      waiter.completion->complete(ToolResult::text(text));
   }
   waiters_.clear();
}

void Handler::expireWaiters(Clock::time_point now) {
   for (auto waiter = waiters_.begin(); waiter != waiters_.end();) {
      if (now < waiter->deadline) {
         ++waiter;
         continue;
      }
      waiter->completion->complete(ToolResult::text(
          "no application session connected within " +
              std::to_string(waiter->timeoutMs) + " ms",
          true));
      waiter = waiters_.erase(waiter);
   }
}

void Handler::expireForwards(Clock::time_point now) {
   std::vector<std::uint64_t> expired;
   for (const auto& entry : forwards_) {
      if (now >= entry.second.deadline) {
         expired.push_back(entry.first);
      }
   }
   if (expired.empty()) {
      return;
   }

   for (const std::uint64_t id : expired) {
      const auto entry = forwards_.find(id);
      if (entry == forwards_.end()) {
         continue;
      }
      entry->second.completion->complete(ToolResult::text(
          "the application did not answer " + entry->second.name +
              " within the proxy timeout",
          true));
      forwards_.erase(entry);
   }

   detachApp("the application stopped answering");
}

void Handler::failPendingForwards(const std::string& prefix,
                                  const std::string& suffix) {
   for (auto& entry : forwards_) {
      entry.second.completion->complete(
          ToolResult::text(prefix + entry.second.name + suffix, true));
   }
   forwards_.clear();
}

void Handler::detachApp(std::string reason) {
   if (appPeer_ == nullptr) {
      return;
   }
   (void)reason;
   std::unique_ptr<AppPeer> peer = std::move(appPeer_);
   appFd_ = -1;
   appReady_ = false;
   /* close() reports onAppDetached once, which fails the remaining forwards. */
   peer->close();
}

bool Handler::peerAdvertises(std::string_view name) const {
   for (const nlohmann::json& entry : peerTools_) {
      if (entry.is_object() && entry.value("name", std::string()) == name) {
         return true;
      }
   }
   return false;
}

ToolResult Handler::presentFrame(const std::string& name,
                                 const nlohmann::json& arguments,
                                 const nlohmann::json& header,
                                 std::vector<std::uint8_t> payload) const {
   (void)name;
   const int width = header.value("width", 0);
   const int height = header.value("height", 0);

   std::vector<std::uint8_t> png;
   if (!encodePngToVector(payload.data(), width, height, png)) {
      return ToolResult::text("PNG encoding failed for " + std::to_string(width) +
                                  "x" + std::to_string(height),
                              true);
   }

   std::string text = header.value("text", std::string());
   if (text.empty()) {
      text = captureSummary(arguments, width, height, png.size(),
                            frameIndexOf(header));
   }

   ToolResult result;
   result.content = nlohmann::json::array({
       nlohmann::json{{"type", "text"}, {"text", std::move(text)}},
       nlohmann::json{{"type", "image"},
                      {"mimeType", "image/png"},
                      {"data", base64Encode(png)}},
   });
   return result;
}

int Handler::nextPollTimeoutMs(Clock::time_point now) const {
   std::optional<Clock::time_point> next;
   const auto consider = [&next](Clock::time_point deadline) {
      if (!next.has_value() || deadline < *next) {
         next = deadline;
      }
   };

   for (const PendingWait& waiter : waiters_) {
      consider(waiter.deadline);
   }
   for (const auto& entry : forwards_) {
      consider(entry.second.deadline);
   }
   if (pendingSpawn_.has_value()) {
      consider(pendingSpawn_->deadline);
   }
   if (appPeer_ != nullptr && !appPeer_->isReady() && !appPeer_->isClosed()) {
      consider(appPeer_->handshakeDeadline());
   }

   if (!next.has_value()) {
      return -1;
   }
   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                              *next - now)
                              .count();
   if (remaining <= 0) {
      return 0;
   }
   return static_cast<int>(std::min<long long>(remaining, 600000));
}

void Handler::shutdown() {
   waiters_.clear();
   forwards_.clear();
   /* Spawn-and-forget: drop the bookkeeping, never the process. */
   cancelSpawn();

   if (appPeer_ != nullptr) {
      std::unique_ptr<AppPeer> peer = std::move(appPeer_);
      appFd_ = -1;
      appReady_ = false;
      peer->close();
   }

   if (clientSession_ != nullptr) {
      clientSession_->close();
      clientSession_.reset();
   }
   clientFd_ = -1;

   listener_.close();
   if (signalFd_ >= 0) {
      ::close(signalFd_);
      signalFd_ = -1;
   }
}

void Handler::onAppAttached(std::vector<nlohmann::json> tools) {
   peerTools_ = std::move(tools);
   appReady_ = true;
   rebuildCatalog();
   /* Attach is silent: a bare app is an identity, and a spawn changes no name. */
   publishToolsIfChanged(false);
   completeWaiters("an application session attached");

   if (pendingSpawn_.has_value()) {
      completeSpawn("spawned " + pendingSpawn_->alias + " (pid " +
                        std::to_string(pendingSpawn_->pid) + ")",
                    false);
   }
}

void Handler::onAppToolsChanged(std::vector<nlohmann::json> tools) {
   peerTools_ = std::move(tools);
   rebuildCatalog();
   publishToolsIfChanged(true);
}

void Handler::onAppDetached(std::string reason) {
   (void)reason;
   appReady_ = false;
   peerTools_.clear();
   rebuildCatalog();
   publishToolsIfChanged(false);
   failPendingForwards("the application closed its connection during ", "");
}

void Handler::onAppContent(std::uint64_t requestId, nlohmann::json result) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }

   ToolResult decoded;
   std::string error;
   if (!ToolResult::decode(result, {}, decoded, error)) {
      entry->second.completion->complete(ToolResult::text(
          "the application returned an invalid result: " + error, true));
   } else {
      entry->second.completion->complete(std::move(decoded));
   }
   forwards_.erase(entry);
}

void Handler::onAppFrame(std::uint64_t requestId,
                         nlohmann::json header,
                         std::vector<std::uint8_t> payload) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }

   ToolResult decoded;
   std::string error;
   if (!ToolResult::decode(header, std::move(payload), decoded, error)) {
      entry->second.completion->complete(ToolResult::text(
          "the application sent an invalid frame result: " + error, true));
   } else {
      entry->second.completion->complete(
          presentFrame(entry->second.name, entry->second.arguments, decoded.content,
                       std::move(decoded.payload)));
   }
   forwards_.erase(entry);
}

void Handler::onAppForwardFailure(std::uint64_t requestId, std::string error) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }
   entry->second.completion->complete(ToolResult::text(std::move(error), true));
   forwards_.erase(entry);
}

void Handler::onAppError(std::uint64_t requestId, nlohmann::json error) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }
   entry->second.completion->complete(ToolResult::error(std::move(error)));
   forwards_.erase(entry);
}

}  // namespace sdl3mcp
