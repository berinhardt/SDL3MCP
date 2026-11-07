#include "supervisor.hpp"

#include "png.hpp"

#include <SDL3MCP/SDL3MCP_version.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kProtocolVersion = "2025-11-25";
constexpr std::string_view kServerName = "sdl3mcp";
constexpr int kWindowPollMs = 50;
/* The relay wakes the stdio read this often to notice a session that went away. */
constexpr int kPeerPollMs = 50;
/* SIGTERM to SIGKILL grace when the application ignores the quit request. */
constexpr int kTerminateGraceMs = 2000;
constexpr std::int64_t kMaxWaitSessionMs = 60000;

/*
  The largest raw frame the supervisor accepts from the child. It bounds the
  allocation a hostile or broken child can request before one payload byte is
  read; a larger byteLength is rejected without allocating it.
*/
constexpr uint64_t kMaxCaptureBytes = 64ull * 1024 * 1024;

nlohmann::json textContent(std::string text)
{
    return nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", std::move(text)}},
    });
}

SupervisorToolResult toolError(std::string text)
{
    return SupervisorToolResult{textContent(std::move(text)), true};
}

SupervisorToolResult toolOk(std::string text)
{
    return SupervisorToolResult{textContent(std::move(text)), false};
}

nlohmann::json toolResultJson(const nlohmann::json &id, const SupervisorToolResult &result)
{
    return makeResult(id, nlohmann::json{
                              {"content", result.content},
                              {"isError", result.isError},
                          });
}

std::string stateName(SupervisorState state)
{
    switch (state) {
    case SupervisorState::Idle:
        return "idle";
    case SupervisorState::Starting:
        return "starting";
    case SupervisorState::Running:
        return "running";
    case SupervisorState::WaitingSession:
        return "waiting for a session";
    case SupervisorState::PeerAttached:
        return "attached";
    }
    return "idle";
}

/* Prints IPv6 literals in brackets, like the library's connect error text. */
std::string endpointText(std::string_view host, std::uint16_t port)
{
    std::string text;
    if (host.find(':') != std::string_view::npos) {
        text = "[" + std::string(host) + "]";
    } else {
        text = std::string(host);
    }
    return text + ":" + std::to_string(port);
}

std::string describeRpcError(const nlohmann::json &error)
{
    if (error.is_object()) {
        const auto message = error.find("message");
        if (message != error.end() && message->is_string()) {
            return message->get<std::string>();
        }
    }
    return error.dump();
}

/*
  Reads the window out of an sdl_info result. The text block is JSON, matching
  the library's own sdl_info, so the supervisor needs no SDL to understand it.
*/
bool windowFromInfo(const nlohmann::json &result, nlohmann::json &window)
{
    const auto content = result.find("content");
    if (content == result.end() || !content->is_array() || content->empty()) {
        return false;
    }

    const nlohmann::json &block = (*content)[0];
    const auto type = block.find("type");
    const auto text = block.find("text");
    if (type == block.end() || *type != "text" || text == block.end() ||
        !text->is_string()) {
        return false;
    }

    const nlohmann::json info =
        nlohmann::json::parse(text->get<std::string>(), nullptr, false);
    if (info.is_discarded() || !info.is_object()) {
        return false;
    }

    const auto entry = info.find("window");
    if (entry == info.end() || entry->is_null() || !entry->is_object()) {
        return false;
    }

    window = *entry;
    return true;
}

std::string describeWindow(const nlohmann::json &window)
{
    std::string title = "(untitled)";
    const auto titleEntry = window.find("title");
    if (titleEntry != window.end() && titleEntry->is_string()) {
        title = titleEntry->get<std::string>();
    }

    const auto dimension = [&window](const char *name) {
        const auto entry = window.find(name);
        return entry != window.end() && entry->is_number_integer()
                   ? entry->get<int>()
                   : 0;
    };

    const int width = dimension("width");
    const int height = dimension("height");

    std::string text = "spawned \"" + title + "\" (" + std::to_string(width) + "x" +
                       std::to_string(height) + " points";

    const int pixelWidth = dimension("pixelWidth");
    const int pixelHeight = dimension("pixelHeight");
    if (pixelWidth > 0 && pixelHeight > 0) {
        text += ", " + std::to_string(pixelWidth) + "x" + std::to_string(pixelHeight) +
                " pixels";
    }

    text += ")";
    return text;
}

/* Reads a JSON integer, including unsigned values that still fit in int64_t. */
bool readInteger(const nlohmann::json &object, const char *name, int64_t &value)
{
    const auto entry = object.find(name);
    if (entry == object.end() || !entry->is_number_integer()) {
        return false;
    }
    if (entry->is_number_unsigned()) {
        const uint64_t raw = entry->get<uint64_t>();
        if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return false;
        }
        value = static_cast<int64_t>(raw);
        return true;
    }
    value = entry->get<int64_t>();
    return true;
}

/*
  Validates the child's raw-frame header before any payload byte is read, so a
  mismatched or oversized byteLength is rejected instead of allocated. The
  expected size uses 64-bit math: width * height * 4 can overflow an int.
*/
bool parseCaptureHeader(const nlohmann::json &result,
                        int &width,
                        int &height,
                        size_t &byteLength,
                        std::string &error)
{
    const auto format = result.find("format");
    if (format == result.end() || !format->is_string() ||
        format->get<std::string>() != "rgba8888") {
        error = "the application sent a capture result without format rgba8888";
        return false;
    }

    int64_t widthValue = 0;
    int64_t heightValue = 0;
    if (!readInteger(result, "width", widthValue) || widthValue <= 0 ||
        !readInteger(result, "height", heightValue) || heightValue <= 0) {
        error = "the application sent a capture result with invalid dimensions";
        return false;
    }

    int64_t lengthValue = 0;
    if (!readInteger(result, "byteLength", lengthValue) || lengthValue < 0) {
        error = "the application sent a capture result without a valid byteLength";
        return false;
    }

    const uint64_t expected = static_cast<uint64_t>(widthValue) *
                              static_cast<uint64_t>(heightValue) * 4u;
    if (static_cast<uint64_t>(lengthValue) != expected) {
        error = "the application sent byteLength " + std::to_string(lengthValue) +
                " for " + std::to_string(widthValue) + "x" +
                std::to_string(heightValue) + " (expected " +
                std::to_string(expected) + ")";
        return false;
    }
    if (static_cast<uint64_t>(lengthValue) > kMaxCaptureBytes) {
        error = "the application sent " + std::to_string(lengthValue) +
                " frame bytes, above the " + std::to_string(kMaxCaptureBytes) +
                " byte supervisor cap";
        return false;
    }

    width = static_cast<int>(widthValue);
    height = static_cast<int>(heightValue);
    byteLength = static_cast<size_t>(lengthValue);
    return true;
}

/*
  The image summary names the frame the app sent, its region when one was given,
  and the PNG size.
*/
std::string captureSummary(const nlohmann::json &arguments,
                           const Frame &frame,
                           size_t pngBytes)
{
    std::string text =
        std::to_string(frame.width) + "x" + std::to_string(frame.height) + " PNG";

    const auto region = arguments.find("region");
    int64_t x = 0;
    int64_t y = 0;
    if (region != arguments.end() && region->is_object() &&
        readInteger(*region, "x", x) && readInteger(*region, "y", y)) {
        text += " at " + std::to_string(x) + "," + std::to_string(y);
    }

    text += ", " + std::to_string(pngBytes) + " bytes";
    return text;
}

} // namespace

nlohmann::json spawnToolDefinition()
{
    return nlohmann::json{
        {"name", "spawn"},
        {"description",
         "Start the application the supervisor was launched with and wait until it "
         "has a window. While it runs its own tools replace this one; call spawn "
         "again only after the application has exited."},
        {"inputSchema",
         nlohmann::json{
             {"type", "object"},
             {"properties", nlohmann::json::object()},
             {"additionalProperties", false},
         }},
        {"annotations",
         nlohmann::json{
             {"readOnlyHint", false},
             {"destructiveHint", false},
             {"idempotentHint", false},
             {"openWorldHint", false},
         }},
    };
}

nlohmann::json waitSessionToolDefinition()
{
    return nlohmann::json{
        {"name", "wait_session"},
        {"description",
         "Wait for an application to connect to the relay's listening socket. "
         "Returns as soon as a session is attached; without timeout_ms it waits "
         "indefinitely and returns a normal tool error when timeout_ms expires. "
         "While a session is attached its tools replace this one."},
        {"inputSchema",
         nlohmann::json{
             {"type", "object"},
             {"properties", nlohmann::json{
                  {"timeout_ms", nlohmann::json{
                                     {"type", "integer"},
                                     {"minimum", 1},
                                     {"maximum", kMaxWaitSessionMs},
                                 }},
              }},
             {"additionalProperties", false},
         }},
        {"annotations",
         nlohmann::json{
             {"readOnlyHint", true},
             {"idempotentHint", false},
             {"openWorldHint", true},
         }},
    };
}

nlohmann::json supervisorCaptureToolDefinition()
{
    return nlohmann::json{
        {"name", "sdl_capture"},
        {"description",
         "Ask the application for a fresh frame and return it as a PNG image block "
         "at the app's native pixel size. The optional region crops pixels from the "
         "captured frame, using capture.lastWidth x capture.lastHeight from sdl_info, "
         "with a top-left origin; it must fit inside the frame and is the only way "
         "to keep the payload small, because captures are never resized."},
        {"inputSchema",
         nlohmann::json{
             {"type", "object"},
             {"properties", nlohmann::json{
                  {"timeout_ms", nlohmann::json{
                                     {"type", "integer"},
                                     {"minimum", 1},
                                     {"maximum", 60000},
                                 }},
                  {"region", nlohmann::json{
                                 {"type", "object"},
                                 {"description",
                                  "Crop rectangle in pixels of the full frame, "
                                  "top-left origin."},
                                 {"properties", nlohmann::json{
                                      {"x", nlohmann::json{{"type", "integer"},
                                                           {"minimum", 0}}},
                                      {"y", nlohmann::json{{"type", "integer"},
                                                           {"minimum", 0}}},
                                      {"width", nlohmann::json{{"type", "integer"},
                                                              {"minimum", 1}}},
                                      {"height", nlohmann::json{{"type", "integer"},
                                                                {"minimum", 1}}},
                                  }},
                                 {"required", nlohmann::json::array(
                                                  {"x", "y", "width", "height"})},
                                 {"additionalProperties", false},
                             }},
              }},
             {"additionalProperties", false},
         }},
        {"annotations",
         nlohmann::json{
             {"readOnlyHint", true},
             {"idempotentHint", false},
             {"openWorldHint", false},
         }},
    };
}

/*
  The tool view while an application is attached, whether it is a spawned child
  or a relayed session. sdl_capture is replaced because PNG and base64 are the
  supervisor's presentation policy; every other app tool keeps its entry.
*/
nlohmann::json attachedToolList(const nlohmann::json &appTools)
{
    nlohmann::json tools = nlohmann::json::array();
    if (appTools.is_array()) {
        for (const nlohmann::json &tool : appTools) {
            if (tool.is_object() && tool.value("name", std::string()) == "sdl_capture") {
                tools.push_back(supervisorCaptureToolDefinition());
            } else {
                tools.push_back(tool);
            }
        }
    }
    return nlohmann::json{{"tools", std::move(tools)}};
}

nlohmann::json supervisorToolList(SupervisorState state, const nlohmann::json &childTools)
{
    switch (state) {
    case SupervisorState::Idle:
        return nlohmann::json{
            {"tools", nlohmann::json::array({spawnToolDefinition()})},
        };
    case SupervisorState::Starting:
        return nlohmann::json{{"tools", nlohmann::json::array()}};
    case SupervisorState::Running:
        return attachedToolList(childTools);
    case SupervisorState::WaitingSession:
        return nlohmann::json{
            {"tools", nlohmann::json::array({waitSessionToolDefinition()})},
        };
    case SupervisorState::PeerAttached:
        return attachedToolList(childTools);
    }
    return nlohmann::json{{"tools", nlohmann::json::array()}};
}

Supervisor::Supervisor(int readFd, int writeFd, SupervisorOptions options)
    : transport_(readFd, writeFd), options_(std::move(options))
{
    if (options_.relay()) {
        state_ = SupervisorState::WaitingSession;
    }
}

Supervisor::~Supervisor()
{
    shutdown();
}

int Supervisor::run()
{
    blockSigpipe();

    if (options_.relay()) {
        std::string error;
        if (!listen(error)) {
            std::fprintf(stderr, "sdl3mcp: %s\n", error.c_str());
            std::fflush(stderr);
            return 1;
        }
        try {
            acceptor_ = std::thread(&Supervisor::acceptLoop, this);
        } catch (const std::exception &) {
            failed_.store(true);
            shutdown();
            return 1;
        }
    } else {
        try {
            monitor_ = std::thread(&Supervisor::monitorLoop, this);
        } catch (const std::exception &) {
            failed_.store(true);
            shutdown();
            return 1;
        }
    }

    const int tickMs = options_.relay() ? kPeerPollMs : -1;
    for (;;) {
        std::string line;
        std::string error;
        const ReadStatus status = transport_.readLine(line, error, tickMs);
        if (status == ReadStatus::Eof || status == ReadStatus::Stopped) {
            break;
        }
        if (status == ReadStatus::Error) {
            failed_.store(true);
            break;
        }
        if (status == ReadStatus::Timeout) {
            /* A relayed session may close while no call is in flight. */
            pollRelayPeer();
            continue;
        }
        try {
            if (!handleLine(line)) {
                break;
            }
        } catch (const std::exception &) {
            /* An exception from a hostile or broken peer stops the supervisor,
               but only after shutdown() has stopped the application. */
            failed_.store(true);
            break;
        }
    }

    shutdown();
    return failed_.load() ? 1 : 0;
}

bool Supervisor::listen(std::string &error)
{
    int fd = -1;
    if (!listenSocket(options_.listenHost.c_str(), options_.listenPort, fd, boundPort_,
                      error)) {
        return false;
    }

    listenerFd_ = fd;
    /* The bound endpoint goes to stderr: stdout is the MCP channel. */
    std::fprintf(stderr, "sdl3mcp: listening on %s\n",
                 endpointText(options_.listenHost, boundPort_).c_str());
    std::fflush(stderr);
    return true;
}

void Supervisor::stop()
{
    transport_.stop();
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        stopRequested_ = true;
    }
    monitorCondition_.notify_all();
}

SupervisorState Supervisor::state() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

bool Supervisor::failed() const
{
    return failed_.load();
}

void Supervisor::blockSigpipe()
{
    /* Writes happen on the request and monitor threads, so both block it. */
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
}

int Supervisor::remainingMs(Deadline deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
        return 0;
    }
    /* Clamp so a huge --startup-timeout-ms cannot overflow the int we pass on. */
    return static_cast<int>(std::min<long long>(remaining, 600000));
}

void Supervisor::monitorLoop()
{
    blockSigpipe();

    for (;;) {
        ChildPeer *child = nullptr;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            monitorCondition_.wait(lock, [this] {
                return stopping_ ||
                       (state_ == SupervisorState::Running && child_ != nullptr);
            });
            if (stopping_) {
                return;
            }
            child = child_.get();
        }

        /* Blocks until this child exits; stop() makes it exit by terminating it. */
        child->waitForExit();

        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            if (stopping_) {
                return;
            }
            if (child_.get() != child || state_ != SupervisorState::Running) {
                /* A startup failure on the request thread already handled it. */
                continue;
            }
            state_ = SupervisorState::Idle;
            peerTools_ = nlohmann::json::array();
        }
        /* Wake a terminate call waiting for the exit to be published. */
        monitorCondition_.notify_all();

        sendToolsListChanged();
    }
}

void Supervisor::acceptLoop()
{
    blockSigpipe();

    for (;;) {
        {
            const std::lock_guard<std::mutex> lock(stateMutex_);
            if (stopping_) {
                return;
            }
        }

        pollfd descriptor{};
        descriptor.fd = listenerFd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, kPeerPollMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }

        const int client = ::accept(listenerFd_, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return;
        }

        /*
          Exactly one session. The listener keeps accepting only so a second
          connection is closed here at once instead of rotting in the backlog;
          it is never handed to the proxy.
        */
        bool refuse = false;
        {
            const std::lock_guard<std::mutex> lock(stateMutex_);
            refuse = stopping_ || state_ == SupervisorState::PeerAttached;
        }
        if (refuse) {
            ::close(client);
            continue;
        }

        attachPeer(client);
    }
}

void Supervisor::attachPeer(int fd)
{
    auto peer = std::make_unique<SocketPeer>(fd);
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (stopping_) {
            return;
        }
        pendingPeer_ = peer.get();
    }

    const Deadline deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options_.startupTimeoutMs);

    nlohmann::json tools;
    std::string failure;
    if (!initializePeer(peer.get(), deadline, tools, failure)) {
        /* The peer owns the fd, so the failed session closes with it. */
        {
            const std::lock_guard<std::mutex> lock(stateMutex_);
            pendingPeer_ = nullptr;
        }
        std::fprintf(stderr, "sdl3mcp: %s\n", failure.c_str());
        std::fflush(stderr);
        return;
    }

    bool attached = false;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        pendingPeer_ = nullptr;
        if (!stopping_ && state_ != SupervisorState::PeerAttached) {
            relayPeer_ = std::move(peer);
            peer_ = relayPeer_.get();
            peerTools_ = std::move(tools);
            state_ = SupervisorState::PeerAttached;
            attached = true;
        }
    }
    if (!attached) {
        return;
    }

    /* Wake wait_session, then announce the new tool view once. */
    monitorCondition_.notify_all();
    sendToolsListChanged();
}

bool Supervisor::initializePeer(Peer *peer,
                                Deadline deadline,
                                nlohmann::json &tools,
                                std::string &failure)
{
    const nlohmann::json initializeParams{
        {"protocolVersion", std::string(kProtocolVersion)},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", nlohmann::json{
             {"name", std::string(kServerName)},
             {"version", std::string(SDL3MCP_VERSION_STRING)},
         }},
    };

    nlohmann::json ignored;
    std::string error;
    if (!requestPeer(peer, "initialize", initializeParams, ignored, deadline, error)) {
        failure = "cannot initialize the application session: " + error;
        return false;
    }

    /* The app is now a server; a real MCP client acknowledges initialize. */
    if (!peer->writeLine(nlohmann::json{
                                    {"jsonrpc", "2.0"},
                                    {"method", "notifications/initialized"},
                                }
                            .dump())) {
        failure = "cannot acknowledge initialize to the application session";
        return false;
    }

    nlohmann::json listResult;
    if (!requestPeer(peer, "tools/list", nlohmann::json::object(), listResult, deadline,
                     error)) {
        failure = "cannot list the application session's tools: " + error;
        return false;
    }

    const auto entries = listResult.find("tools");
    if (entries == listResult.end() || !entries->is_array()) {
        failure = "the application answered tools/list without a tools array";
        return false;
    }

    tools = *entries;
    return true;
}

void Supervisor::detachPeer(Peer *peer)
{
    std::unique_ptr<Peer> released;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (!options_.relay() || state_ != SupervisorState::PeerAttached || peer_ != peer) {
            return;
        }
        released = std::move(relayPeer_);
        peer_ = nullptr;
        peerTools_ = nlohmann::json::array();
        state_ = SupervisorState::WaitingSession;
    }

    /* `released` closes the socket; the waiter and the client hear about it once. */
    monitorCondition_.notify_all();
    sendToolsListChanged();
}

void Supervisor::pollRelayPeer()
{
    if (!options_.relay()) {
        return;
    }

    Peer *peer = nullptr;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ == SupervisorState::PeerAttached) {
            peer = peer_;
        }
    }
    if (peer != nullptr && peer->closed()) {
        detachPeer(peer);
    }
}

Peer *Supervisor::dataPath()
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return peer_;
}

SupervisorToolResult Supervisor::waitSession(const nlohmann::json &arguments)
{
    std::int64_t timeoutMs = -1;
    if (arguments.contains("timeout_ms")) {
        if (!readInteger(arguments, "timeout_ms", timeoutMs) || timeoutMs < 1 ||
            timeoutMs > kMaxWaitSessionMs) {
            return toolError("wait_session timeout_ms must be in 1.." +
                             std::to_string(kMaxWaitSessionMs));
        }
    }

    const auto attached = [this] { return state_ == SupervisorState::PeerAttached; };

    std::unique_lock<std::mutex> lock(stateMutex_);
    if (attached()) {
        return toolOk("an application session is already attached");
    }

    if (timeoutMs < 0) {
        monitorCondition_.wait(lock, [this, &attached] {
            return attached() || stopping_ || stopRequested_;
        });
    } else {
        const Deadline deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeoutMs);
        while (!attached() && !stopping_ && !stopRequested_) {
            if (monitorCondition_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                break;
            }
        }
    }

    if (attached()) {
        return toolOk("an application session attached");
    }
    if (stopping_ || stopRequested_) {
        return toolError("the supervisor is stopping before an application session "
                         "attached");
    }
    return toolError("no application session connected within " +
                     std::to_string(timeoutMs) + " ms");
}

void Supervisor::shutdown()
{
    ChildPeer *process = nullptr;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
        process = child_.get();
        /* A handshake in progress must not delay shutdown for the whole timeout. */
        if (pendingPeer_ != nullptr) {
            pendingPeer_->stop();
        }
    }
    monitorCondition_.notify_all();
    transport_.stop();

    if (process != nullptr) {
        process->terminate(kTerminateGraceMs);
    }
    if (monitor_.joinable()) {
        monitor_.join();
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    if (listenerFd_ >= 0) {
        ::close(listenerFd_);
        listenerFd_ = -1;
    }

    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        peer_ = nullptr;
        pendingPeer_ = nullptr;
        relayPeer_.reset();
        child_.reset();
        state_ = options_.relay() ? SupervisorState::WaitingSession
                                  : SupervisorState::Idle;
        peerTools_ = nlohmann::json::array();
    }
}

bool Supervisor::handleLine(const std::string &line)
{
    RpcMessage message;
    nlohmann::json parseError;
    if (!parseMessage(line, message, parseError)) {
        return writeJson(parseError);
    }

    if (message.kind == RpcMessage::Kind::Notification) {
        return true;
    }

    handleRequest(message.request);
    return !failed_.load();
}

void Supervisor::handleRequest(const RpcRequest &request)
{
    if (request.method == "initialize") {
        writeJson(initializeResult(request.id, request.params));
        return;
    }
    if (request.method == "ping") {
        writeJson(makeResult(request.id, nlohmann::json::object()));
        return;
    }
    if (request.method == "tools/list") {
        handleToolsList(request.id);
        return;
    }
    if (request.method == "tools/call") {
        handleToolsCall(request.id, request.params);
        return;
    }

    writeJson(makeError(request.id, RpcErrorCode::MethodNotFound,
                        "Method not found: " + request.method));
}

nlohmann::json Supervisor::initializeResult(const nlohmann::json &id,
                                            const nlohmann::json &params)
{
    if (initialized_) {
        return makeError(id, RpcErrorCode::InvalidRequest,
                         "initialize was already called");
    }

    const auto version = params.find("protocolVersion");
    if (version == params.end() || !version->is_string()) {
        return makeError(id, RpcErrorCode::InvalidParams,
                         "initialize requires a string protocolVersion");
    }

    initialized_ = true;

    return makeResult(id, nlohmann::json{
                              {"protocolVersion", std::string(kProtocolVersion)},
                              {"capabilities", nlohmann::json{
                                   {"tools", nlohmann::json{{"listChanged", true}}},
                               }},
                              {"serverInfo", nlohmann::json{
                                   {"name", std::string(kServerName)},
                                   {"version", std::string(SDL3MCP_VERSION_STRING)},
                               }},
                          });
}

void Supervisor::handleToolsList(const nlohmann::json &id)
{
    if (!initialized_) {
        writeJson(makeError(id, RpcErrorCode::InvalidRequest,
                            "tools are unavailable before initialize"));
        return;
    }

    SupervisorState current = SupervisorState::Idle;
    nlohmann::json tools;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        current = state_;
        tools = peerTools_;
    }

    writeJson(makeResult(id, supervisorToolList(current, tools)));
}

void Supervisor::handleToolsCall(const nlohmann::json &id, const nlohmann::json &params)
{
    if (!initialized_) {
        writeJson(makeError(id, RpcErrorCode::InvalidRequest,
                            "tools are unavailable before initialize"));
        return;
    }

    const auto name = params.find("name");
    if (name == params.end() || !name->is_string()) {
        writeJson(makeError(id, RpcErrorCode::InvalidParams,
                            "tools/call requires a string tool name"));
        return;
    }
    const std::string toolName = name->get<std::string>();

    nlohmann::json arguments = nlohmann::json::object();
    const auto supplied = params.find("arguments");
    if (supplied != params.end()) {
        if (!supplied->is_object()) {
            writeJson(makeError(id, RpcErrorCode::InvalidParams,
                                "tools/call arguments must be a JSON object"));
            return;
        }
        arguments = *supplied;
    }

    /*
      Relay mode: the supervisor is the client-facing endpoint and every call
      goes to the attached session. wait_session belongs to the supervisor and
      is legal whether or not one is attached.
    */
    if (options_.relay()) {
        if (toolName == "wait_session") {
            writeToolResult(id, waitSession(arguments));
            return;
        }
        if (state() != SupervisorState::PeerAttached || dataPath() == nullptr) {
            writeToolResult(
                id, toolError("no application session is attached; call wait_session first"));
            return;
        }
        /* sdl_capture keeps the same PNG presentation the child path gives it. */
        if (toolName == "sdl_capture") {
            writeJson(forwardCapture(id, arguments));
            return;
        }
        /* sdl_terminate is an ordinary forward: the app owns its own process. */
        writeJson(forwardChildCall(id, toolName, arguments));
        return;
    }

    /*
      spawn always belongs to the supervisor: while the child runs this is also
      the short-circuit that keeps a forwarded "spawn" from reaching the app.
    */
    if (toolName == "spawn") {
        writeToolResult(id, spawn());
        return;
    }

    if (state() != SupervisorState::Running) {
        writeToolResult(id, toolError("the application is not running; call spawn first"));
        return;
    }

    /*
      sdl_capture is the one tool the supervisor does not forward verbatim: the
      child sends raw pixels, and the supervisor owns PNG encoding and base64.
    */
    if (toolName == "sdl_capture") {
        writeJson(forwardCapture(id, arguments));
        return;
    }

    /*
      sdl_terminate is forwarded like any other tool, but the supervisor then
      owns the exit: a quit request the app ignores must not leave a live child
      behind one stable MCP endpoint.
    */
    if (toolName == "sdl_terminate") {
        writeToolResult(id, terminate(arguments));
        return;
    }

    writeJson(forwardChildCall(id, toolName, arguments));
}

SupervisorToolResult Supervisor::spawn()
{
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ != SupervisorState::Idle) {
            return toolError("spawn is only valid while idle; the application is " +
                             stateName(state_) + ".");
        }
        state_ = SupervisorState::Starting;
        peerTools_ = nlohmann::json::array();
    }
    sendToolsListChanged();

    const Deadline deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options_.startupTimeoutMs);

    nlohmann::json window = nlohmann::json::object();
    std::string failure;
    if (!startChild(deadline, window, failure)) {
        return failSpawn(failure);
    }

    return toolOk(describeWindow(window));
}

bool Supervisor::startChild(Deadline deadline, nlohmann::json &window, std::string &failure)
{
    auto child = std::make_unique<ChildPeer>();
    if (!child->start(ChildCommand{options_.target, options_.targetArguments}, failure)) {
        return false;
    }

    ChildPeer *process = nullptr;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        child_ = std::move(child);
        process = child_.get();
        peer_ = process;
    }

    const int execTimeout = std::max(remainingMs(deadline), 1);
    if (!process->waitForExec(execTimeout, failure)) {
        return false;
    }

    std::string error;
    nlohmann::json ignored;
    const nlohmann::json initializeParams{
        {"protocolVersion", std::string(kProtocolVersion)},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", nlohmann::json{
             {"name", std::string(kServerName)},
             {"version", std::string(SDL3MCP_VERSION_STRING)},
         }},
    };
    if (!requestPeer(process, "initialize", initializeParams, ignored, deadline, error)) {
        failure = error;
        return false;
    }

    /* The child is now a server; a real MCP client acknowledges initialize. */
    if (!process->writeLine(nlohmann::json{
                                    {"jsonrpc", "2.0"},
                                    {"method", "notifications/initialized"},
                                }
                                .dump())) {
        failure = "cannot acknowledge initialize to the application";
        return false;
    }

    nlohmann::json listResult;
    if (!requestPeer(process, "tools/list", nlohmann::json::object(), listResult, deadline,
                     error)) {
        failure = error;
        return false;
    }

    const auto tools = listResult.find("tools");
    if (tools == listResult.end() || !tools->is_array()) {
        failure = "the application answered tools/list without a tools array";
        return false;
    }

    for (;;) {
        if (process->hasExited()) {
            failure = "the application exited during startup with status " +
                      std::to_string(process->exitCode());
            return false;
        }

        const int remaining = remainingMs(deadline);
        if (remaining <= 0) {
            failure = "the application did not open a window within " +
                      std::to_string(options_.startupTimeoutMs) + " ms";
            return false;
        }

        nlohmann::json infoResult;
        const nlohmann::json infoRequest{
            {"name", "sdl_info"},
            {"arguments", nlohmann::json::object()},
        };
        if (!requestPeer(process, "tools/call", infoRequest, infoResult, deadline,
                         error)) {
            failure = error;
            return false;
        }
        if (windowFromInfo(infoResult, window)) {
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(kWindowPollMs, remaining)));
    }

    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        peerTools_ = *tools;
        state_ = SupervisorState::Running;
    }
    monitorCondition_.notify_all();
    sendToolsListChanged();
    return true;
}

SupervisorToolResult Supervisor::failSpawn(const std::string &reason)
{
    ChildPeer *process = nullptr;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        process = child_.get();
    }

    std::string stderrTail;
    if (process != nullptr) {
        process->terminate();
        stderrTail = process->stderrTail();
    }

    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        child_.reset();
        peer_ = nullptr;
        state_ = SupervisorState::Idle;
        peerTools_ = nlohmann::json::array();
    }
    monitorCondition_.notify_all();
    sendToolsListChanged();

    std::string text = "spawn failed: " + reason;
    if (stderrTail.empty()) {
        text += "\nthe application wrote nothing to stderr";
    } else {
        text += "\napplication stderr (last lines):\n" + stderrTail;
    }
    return toolError(std::move(text));
}

SupervisorToolResult Supervisor::terminate(const nlohmann::json &arguments)
{
    ChildPeer *process = nullptr;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ != SupervisorState::Running || !child_) {
            return toolError("the application is not running; call spawn first");
        }
        process = child_.get();
    }

    /* The timer covers the forward call and the wait for the exit together. */
    const Deadline deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options_.terminateTimeoutMs);

    nlohmann::json ignored;
    std::string error;
    const nlohmann::json params{
        {"name", "sdl_terminate"},
        {"arguments", arguments},
    };
    /*
      The child normally answers "termination requested" before it exits, but it
      may close its pipe first. Either way the exit below decides the result.
    */
    (void)requestPeer(process, "tools/call", params, ignored, deadline, error);

    if (process->waitForExit(remainingMs(deadline))) {
        awaitChildExitHandled(process);
        return toolOk("terminated");
    }

    process->terminate(kTerminateGraceMs);
    awaitChildExitHandled(process);

    return toolOk("termination was forced: the application did not exit within " +
                  std::to_string(options_.terminateTimeoutMs) +
                  " ms, so it was sent SIGTERM and then SIGKILL");
}

void Supervisor::awaitChildExitHandled(ChildPeer *process)
{
    std::unique_lock<std::mutex> lock(stateMutex_);
    monitorCondition_.wait_for(lock, std::chrono::seconds(5), [this, process] {
        return stopping_ || state_ != SupervisorState::Running || child_.get() != process;
    });
}

bool Supervisor::requestPeer(Peer *peer,
                             std::string_view method,
                             const nlohmann::json &params,
                             nlohmann::json &result,
                             Deadline deadline,
                             std::string &error)
{
    error.clear();
    if (peer == nullptr) {
        error = "the application is not running";
        return false;
    }

    const long long requestId = nextRequestId_.fetch_add(1);
    const nlohmann::json request{
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", std::string(method)},
        {"params", params},
    };
    if (!peer->writeLine(request.dump())) {
        error = "cannot write " + std::string(method) + " to the application";
        return false;
    }

    for (;;) {
        const int remaining = remainingMs(deadline);
        if (remaining <= 0) {
            error = "the application did not answer " + std::string(method) +
                    " before the startup timeout";
            return false;
        }

        std::string line;
        std::string readError;
        const ReadStatus status = peer->readLine(line, readError, remaining);
        if (status == ReadStatus::Timeout) {
            error = "the application did not answer " + std::string(method) +
                    " before the startup timeout";
            return false;
        }
        if (status != ReadStatus::Line) {
            error = "the application closed its connection during " + std::string(method);
            return false;
        }

        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object() || message.contains("method")) {
            continue;
        }
        const auto id = message.find("id");
        if (id == message.end() || *id != requestId) {
            continue;
        }
        if (message.contains("error")) {
            error = "the application returned an error for " + std::string(method) + ": " +
                    describeRpcError(message["error"]);
            return false;
        }
        const auto payload = message.find("result");
        if (payload == message.end()) {
            error = "the application answered " + std::string(method) + " without a result";
            return false;
        }

        result = *payload;
        return true;
    }
}

nlohmann::json Supervisor::forwardChildCall(const nlohmann::json &id,
                                            const std::string &name,
                                            const nlohmann::json &arguments)
{
    Peer *peer = dataPath();
    if (peer == nullptr) {
        return toolResultJson(id, toolError("the application is not running"));
    }

    const long long requestId = nextRequestId_.fetch_add(1);
    const nlohmann::json request{
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", "tools/call"},
        {"params", nlohmann::json{{"name", name}, {"arguments", arguments}}},
    };
    if (!peer->writeLine(request.dump())) {
        if (options_.relay()) {
            detachPeer(peer);
        }
        return toolResultJson(id, toolError("cannot reach the application"));
    }

    /*
      No deadline: the app's own tool timeouts bound the wait, and a peer that
      dies closes its connection, which is reported as a tool error.
    */
    for (;;) {
        std::string line;
        std::string error;
        const ReadStatus status = peer->readLine(line, error);
        if (status != ReadStatus::Line) {
            if (options_.relay() && status != ReadStatus::Stopped) {
                /* A relayed session that went away returns the relay to wait_session. */
                detachPeer(peer);
            }
            return toolResultJson(
                id, toolError("the application closed its connection during " + name));
        }

        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object() || message.contains("method")) {
            continue;
        }
        const auto responseId = message.find("id");
        if (responseId == message.end() || *responseId != requestId) {
            continue;
        }

        /* A JSON-RPC error from the child travels back unchanged. */
        if (message.contains("error")) {
            return nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", message["error"]},
            };
        }

        const auto result = message.find("result");
        if (result == message.end() || !result->is_object()) {
            return toolResultJson(
                id, toolError("the application returned an invalid tools/call result"));
        }

        SupervisorToolResult forwarded;
        const auto content = result->find("content");
        forwarded.content =
            content != result->end() ? *content : nlohmann::json::array();
        const auto isError = result->find("isError");
        forwarded.isError =
            isError != result->end() && isError->is_boolean() && isError->get<bool>();

        return toolResultJson(id, forwarded);
    }
}

nlohmann::json Supervisor::forwardCapture(const nlohmann::json &id,
                                          const nlohmann::json &arguments)
{
    Peer *peer = dataPath();
    if (peer == nullptr) {
        return toolResultJson(id, toolError("the application is not running"));
    }

    const long long requestId = nextRequestId_.fetch_add(1);
    const nlohmann::json request{
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", "tools/call"},
        {"params", nlohmann::json{{"name", "sdl_capture"}, {"arguments", arguments}}},
    };
    if (!peer->writeLine(request.dump())) {
        if (options_.relay()) {
            detachPeer(peer);
        }
        return toolResultJson(id, toolError("cannot reach the application"));
    }

    for (;;) {
        std::string line;
        std::string error;
        const ReadStatus status = peer->readLine(line, error);
        if (status != ReadStatus::Line) {
            if (options_.relay() && status != ReadStatus::Stopped) {
                detachPeer(peer);
            }
            return toolResultJson(
                id, toolError("the application closed its connection during sdl_capture"));
        }

        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object() || message.contains("method")) {
            continue;
        }
        const auto responseId = message.find("id");
        if (responseId == message.end() || *responseId != requestId) {
            continue;
        }

        /* A JSON-RPC error from the child travels back unchanged. */
        if (message.contains("error")) {
            return nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", message["error"]},
            };
        }

        const auto result = message.find("result");
        if (result == message.end() || !result->is_object()) {
            return toolResultJson(
                id, toolError("the application returned an invalid sdl_capture result"));
        }

        /* A text error result is the child's failure: pass it through unchanged. */
        const auto isError = result->find("isError");
        if (isError != result->end() && isError->is_boolean() && isError->get<bool>()) {
            SupervisorToolResult failed;
            const auto content = result->find("content");
            failed.content =
                content != result->end() ? *content : nlohmann::json::array();
            failed.isError = true;
            return toolResultJson(id, failed);
        }

        int sourceWidth = 0;
        int sourceHeight = 0;
        size_t byteLength = 0;
        std::string headerError;
        if (!parseCaptureHeader(*result, sourceWidth, sourceHeight, byteLength,
                                headerError)) {
            return toolResultJson(id, toolError(std::move(headerError)));
        }

        /* No deadline, like every other forwarded call: the app's timeout bounds it. */
        std::vector<uint8_t> pixels;
        if (!peer->readBytes(byteLength, pixels, error)) {
            if (options_.relay()) {
                detachPeer(peer);
            }
            return toolResultJson(
                id, toolError("cannot read the frame from the application: " + error));
        }

        Frame frame;
        frame.width = sourceWidth;
        frame.height = sourceHeight;
        frame.pixels = std::move(pixels);

        const std::vector<uint8_t> png = encodePng(frame);
        if (png.empty()) {
            return toolResultJson(
                id, toolError("PNG encoding failed for " + std::to_string(frame.width) +
                              "x" + std::to_string(frame.height)));
        }

        SupervisorToolResult image;
        image.content = nlohmann::json::array({
            nlohmann::json{{"type", "text"},
                           {"text", captureSummary(arguments, frame, png.size())}},
            nlohmann::json{{"type", "image"},
                           {"mimeType", "image/png"},
                           {"data", base64Encode(png)}},
        });
        image.isError = false;
        return toolResultJson(id, image);
    }
}

bool Supervisor::writeJson(const nlohmann::json &message)
{
    const std::lock_guard<std::mutex> lock(writeMutex_);
    /* Replace rather than throw: child-provided strings decide the encoding. */
    const std::string line =
        message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (transport_.writeLine(line)) {
        return true;
    }
    failed_.store(true);
    return false;
}

void Supervisor::sendToolsListChanged()
{
    writeJson(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", "notifications/tools/list_changed"},
    });
}

void Supervisor::writeToolResult(const nlohmann::json &id,
                                 const SupervisorToolResult &result)
{
    writeJson(toolResultJson(id, result));
}

} // namespace sdl3mcp
