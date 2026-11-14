#include "Web.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Posix.h"

namespace sdl3mcp {
namespace {

constexpr std::string_view kToolPrefix = "app_spawn_";

bool isAliasCharacter(char character) {
   return (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_';
}

bool isValidAlias(const std::string& alias) {
   return !alias.empty() &&
          std::all_of(alias.begin(), alias.end(), isAliasCharacter);
}

bool isReservedAlias(const std::string& alias) {
   return alias.size() >= kToolPrefix.size() &&
          alias.compare(0, kToolPrefix.size(), kToolPrefix) == 0;
}

/*
  Reads one JSON object and appends its validated entries. `sources` remembers
  which file first defined each alias, so a later file that repeats one is an
  error instead of a silent replacement.
*/
Result<void, ProtocolError> appendWebFile(
    const std::string& path,
    WebApps& loaded,
    std::map<std::string, std::string>& sources) {
   std::ifstream file(path, std::ios::binary);
   if (!file) {
      return ProtocolError{"cannot open web file \"" + path +
                           "\": " + std::strerror(errno)};
   }

   const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
   if (document.is_discarded()) {
      return ProtocolError{"web file \"" + path + "\" is not valid JSON"};
   }
   if (!document.is_object()) {
      return ProtocolError{"web file \"" + path +
                           "\" must contain a JSON object"};
   }
   if (document.empty()) {
      return ProtocolError{"web file \"" + path + "\" defines no aliases"};
   }

   for (auto entry = document.begin(); entry != document.end(); ++entry) {
      const std::string alias = entry.key();
      if (isReservedAlias(alias)) {
         return ProtocolError{"web entry \"" + alias +
                              "\" must not use the reserved app_spawn_ prefix"};
      }
      if (!isValidAlias(alias)) {
         return ProtocolError{"web alias \"" + alias +
                              "\" must match [a-z0-9_]+"};
      }
      if (!entry->is_string()) {
         return ProtocolError{"web entry \"" + alias +
                              "\" must be a non-empty string"};
      }

      std::string root = entry->get<std::string>();
      if (root.empty()) {
         return ProtocolError{"web entry \"" + alias +
                              "\" must be a non-empty string"};
      }

      const auto existing = sources.find(alias);
      if (existing != sources.end()) {
         return ProtocolError{"web alias \"" + alias +
                              "\" is defined in both \"" + existing->second +
                              "\" and \"" + path + "\""};
      }

      sources.emplace(alias, path);
      loaded.push_back(WebApp{alias, std::move(root), path});
   }

   return {};
}

}  // namespace

Result<WebApps, ProtocolError> loadWebApps(
    const std::vector<std::string>& paths) {
   if (paths.empty()) {
      return ProtocolError{"no web sources were given"};
   }

   WebApps loaded;
   std::map<std::string, std::string> sources;
   for (const std::string& path : paths) {
      struct stat info{};
      if (::stat(path.c_str(), &info) != 0) {
         return ProtocolError{"cannot open web file \"" + path +
                              "\": " + std::strerror(errno)};
      }
      if (!S_ISREG(info.st_mode)) {
         return ProtocolError{"web path \"" + path + "\" is not a file"};
      }
      const Result<void, ProtocolError> appended =
          appendWebFile(path, loaded, sources);
      if (!appended) {
         return appended.error();
      }
   }

   return loaded;
}

}  // namespace sdl3mcp

namespace sdl3mcp {
namespace {

constexpr const char* kPythonExecutable = "python3";
constexpr const char* kWebSocatExecutable = "websocat";
constexpr const char* kBrowserWidth = "1280";
constexpr const char* kBrowserHeight = "800";

/* The template mkdtemp() fills; its suffix must be the last six characters. */
constexpr const char* kProfileTemplate = "sdl3mcp-web-XXXXXX";
constexpr const char* kLockFile = ".lock";
constexpr const char* kFallbackProfileParent = "/tmp";

/* The prefix a seeded profile carries, which is also what the sweep looks for. */
constexpr const char* kProfilePrefix = "sdl3mcp-web-";
/* Long enough that a directory still being created is never mistaken for one. */
constexpr int kUnlockedGraceSeconds = 60;

/*
  One line per preference. The first is the reason the profile exists: the page
  calls window.close() when the application shuts down, and Firefox only allows
  that for a window a script opened or when this preference says so. The rest
  keep a throwaway profile from stopping on first-run prompts. The console
  preference is the diagnostic channel: Emscripten's console.log/console.error
  (the application's own output and any wasm abort) is written to the browser
  process's stdout, which WebRun forwards to the Handler's stderr.
*/
constexpr const char* kProfilePreferences =
    "user_pref(\"dom.allow_scripts_to_close_windows\", true);\n"
    "user_pref(\"devtools.console.stdout.content\", true);\n"
    "user_pref(\"browser.tabs.warnOnClose\", false);\n"
    "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
    "user_pref(\"browser.startup.page\", 0);\n"
    "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);\n"
    "user_pref(\"app.update.auto\", false);\n";

std::string baseName(const std::string& path) {
   const std::size_t separator = path.find_last_of('/');
   return separator == std::string::npos ? path : path.substr(separator + 1);
}

bool isExecutable(const std::string& path) {
   return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

/* realpath() turns a PATH hit into the absolute path the plan prints. */
bool absolutePath(const std::string& path, std::string& absolute) {
   char* resolved = ::realpath(path.c_str(), nullptr);
   if (resolved == nullptr) {
      return false;
   }
   absolute.assign(resolved);
   std::free(resolved);
   return true;
}

bool findOnPath(const char* name, std::string& found) {
   const char* path = std::getenv("PATH");
   if (path == nullptr) {
      return false;
   }

   /* An empty entry would mean the current directory; skip it. */
   std::string_view remaining(path);
   while (!remaining.empty()) {
      const std::size_t separator = remaining.find(':');
      const std::string_view entry = remaining.substr(0, separator);
      if (!entry.empty()) {
         std::string candidate(entry);
         candidate += '/';
         candidate += name;
         if (isExecutable(candidate) && absolutePath(candidate, found)) {
            return true;
         }
      }
      if (separator == std::string_view::npos) {
         break;
      }
      remaining.remove_prefix(separator + 1);
   }
   return false;
}

bool isWritableDirectory(const char* path) {
   if (path == nullptr || path[0] == '\0') {
      return false;
   }
   struct stat status{};
   return ::stat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
          ::access(path, W_OK) == 0;
}

std::string profileParent() {
   const char* runtime = std::getenv("XDG_RUNTIME_DIR");
   if (isWritableDirectory(runtime)) {
      return runtime;
   }
   return kFallbackProfileParent;
}

Result<void, TransportError> writePreferences(const std::string& directory) {
   const std::string path = directory + "/user.js";
   std::FILE* file = std::fopen(path.c_str(), "w");
   if (file == nullptr) {
      const int openError = errno;
      return TransportError{"cannot write " + path + ": " +
                            std::strerror(openError)};
   }

   const std::size_t length = std::strlen(kProfilePreferences);
   const bool written = std::fwrite(kProfilePreferences, 1, length, file) == length;
   const bool closed = std::fclose(file) == 0;
   if (!written || !closed) {
      return TransportError{"cannot write " + path};
   }
   return {};
}

/* The mtime test that keeps the sweep off a directory a launcher is creating. */
bool olderThan(const std::string& path, int seconds) {
   struct stat status{};
   if (::stat(path.c_str(), &status) != 0) {
      return false;
   }
   return std::time(nullptr) - status.st_mtime > seconds;
}

}  // namespace

Result<void, TransportError> preflightWebLaunch(WebLaunchOptions& options) {
   std::string websocat;
   if (!findOnPath(kWebSocatExecutable, websocat)) {
      return TransportError{
          "websocat not found: install websocat (cargo install websocat)"};
   }

   std::string python;
   if (!findOnPath(kPythonExecutable, python)) {
      return TransportError{"python3 not found: install python3"};
   }

   struct stat status{};
   if (::stat(options.pageRoot.c_str(), &status) != 0 ||
       !S_ISDIR(status.st_mode)) {
      return TransportError{"page root " + options.pageRoot +
                            " is not a directory"};
   }
   const std::string entryPath = options.pageRoot + "/" + options.entry;
   if (::stat(entryPath.c_str(), &status) != 0) {
      return TransportError{"page root " + options.pageRoot +
                            " does not contain " + options.entry};
   }

   const Result<std::string, TransportError> browser = resolveBrowser();
   if (!browser) {
      return browser.error();
   }

   options.websocat = std::move(websocat);
   options.python = std::move(python);
   options.browser = *browser;
   return {};
}

Result<std::uint16_t, TransportError> pickLoopbackPort() {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return TransportError{std::string("cannot open a loopback socket: ") +
                            std::strerror(errno)};
   }

   int reuse = 1;
   (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = 0;
   if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
       0) {
      const int bindError = errno;
      ::close(fd);
      return TransportError{std::string("cannot reserve a loopback port: ") +
                            std::strerror(bindError)};
   }

   socklen_t length = sizeof address;
   if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      const int nameError = errno;
      ::close(fd);
      return TransportError{
          std::string("cannot read the reserved loopback port: ") +
          std::strerror(nameError)};
   }

   const std::uint16_t port = ntohs(address.sin_port);
   ::close(fd);
   return port;
}

Result<void, TransportError> pickWebPorts(WebLaunchOptions& options) {
   const Result<std::uint16_t, TransportError> page = pickLoopbackPort();
   if (!page) {
      return page.error();
   }

   /*
     A released port can be chosen again, so the bridge is retried until it
     differs; the page server binds first, which makes a collision a real
     startup failure rather than a plan-time guess.
   */
   for (int attempt = 0; attempt < 16; ++attempt) {
      const Result<std::uint16_t, TransportError> bridge = pickLoopbackPort();
      if (!bridge) {
         return bridge.error();
      }
      if (*bridge != *page) {
         options.pagePort = *page;
         options.bridgePort = *bridge;
         return {};
      }
   }

   return TransportError{"cannot reserve two distinct loopback ports"};
}

Result<void, TransportError> prepareWebLaunch(WebLaunchOptions& options) {
   const Result<void, TransportError> preflight = preflightWebLaunch(options);
   if (!preflight) {
      return preflight.error();
   }

   const Result<int, TransportError> swept = sweepAbandonedProfiles();
   if (!swept) {
      return swept.error();
   }
   if (*swept > 0) {
      std::fprintf(stderr, "sdl3mcp: web: reclaimed %d abandoned profile(s)\n",
                   *swept);
   }

   std::string directory;
   int lockFd = -1;
   const Result<void, TransportError> seeded = seedProfile(directory, lockFd);
   if (!seeded) {
      return seeded.error();
   }
   options.profileDir = std::move(directory);
   options.profileLockFd = lockFd;

   const Result<void, TransportError> ports = pickWebPorts(options);
   if (!ports) {
      (void)removeProfileTree(options.profileDir);
      ::close(options.profileLockFd);
      options.profileDir.clear();
      options.profileLockFd = -1;
      return ports.error();
   }

   return {};
}

Result<std::vector<std::vector<std::string>>, TransportError>
makeWebLaunchPlan(const WebLaunchOptions& options) {
   std::vector<std::vector<std::string>> plan;

   if (options.pageRoot.empty()) {
      return TransportError{"pageRoot must not be empty"};
   }
   if (options.entry.empty()) {
      return TransportError{"entry must not be empty"};
   }
   if (!isExecutable(options.python)) {
      return TransportError{"python must name an executable"};
   }
   if (!isExecutable(options.websocat)) {
      return TransportError{"websocat must name an executable"};
   }
   if (options.browser.empty()) {
      return TransportError{"browser must not be empty"};
   }
   if (options.handlerEndpoint.empty()) {
      return TransportError{"handlerEndpoint must not be empty"};
   }
   if (!options.placeholderPorts) {
      if (options.pagePort == 0) {
         return TransportError{"pagePort must not be zero"};
      }
      if (options.bridgePort == 0) {
         return TransportError{"bridgePort must not be zero"};
      }
      if (options.pagePort == options.bridgePort) {
         return TransportError{"pagePort and bridgePort must differ"};
      }
   }

   const std::string pagePort = options.placeholderPorts
                                    ? "PAGE_PORT"
                                    : std::to_string(options.pagePort);
   const std::string bridgePort = options.placeholderPorts
                                      ? "BRIDGE_PORT"
                                      : std::to_string(options.bridgePort);
   const std::string bridgeEndpoint =
       std::string(kWebLoopbackHost) + ":" + bridgePort;

   std::vector<std::string> pageServer{
       options.python,
       "-m",
       "http.server",
       pagePort,
       "--bind",
       kWebLoopbackHost,
       "--directory",
       options.pageRoot,
   };

   std::vector<std::string> bridge{
       options.websocat,
       "--binary",
       "ws-l:" + bridgeEndpoint,
       "tcp:" + options.handlerEndpoint,
   };

   std::vector<std::string> browser{
       options.browser,
       "--no-remote",
       "--new-instance",
       "--profile",
       options.profileDir,
       "--width",
       kBrowserWidth,
       "--height",
       kBrowserHeight,
       "http://" + std::string(kWebLoopbackHost) + ":" + pagePort + "/" +
           options.entry + "?mcp=" + bridgeEndpoint,
   };

   plan.push_back(std::move(pageServer));
   plan.push_back(std::move(bridge));
   plan.push_back(std::move(browser));
   return plan;
}

Result<std::string, TransportError> resolveBrowser() {
   const char* configured = std::getenv("BROWSER");
   if (configured != nullptr) {
      const std::string candidate(configured);
      const std::string name = baseName(candidate);
      std::string absolute;
      if ((name == "firefox" || name == "firefox-esr") &&
          isExecutable(candidate) && absolutePath(candidate, absolute)) {
         return absolute;
      }
   }

   for (const char* name : {"firefox", "firefox-esr"}) {
      std::string found;
      if (findOnPath(name, found)) {
         return found;
      }
   }

   return TransportError{
       "no isolated browser available: install firefox; "
       "no other browser is supported"};
}

Result<void, TransportError> seedProfile(std::string& directory, int& lockFd) {
   const std::string parent = profileParent();
   std::string candidate = parent + "/" + kProfileTemplate;
   if (::mkdtemp(candidate.data()) == nullptr) {
      const int createError = errno;
      return TransportError{"cannot create a profile directory under " + parent +
                            ": " + std::strerror(createError)};
   }

   /*
     The lock is the profile's ownership: the kernel drops it when the
     descriptor closes, including on SIGKILL, which a PID file cannot promise.
     O_CLOEXEC keeps the three children from inheriting it and making a dead
     run look alive.
   */
   const int fd = ::open((candidate + "/" + kLockFile).c_str(),
                         O_CREAT | O_RDWR | O_CLOEXEC, 0600);
   if (fd < 0) {
      const int openError = errno;
      ::rmdir(candidate.c_str());
      return TransportError{"cannot create the profile lock: " +
                            std::string(std::strerror(openError))};
   }
   if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int lockError = errno;
      ::close(fd);
      ::rmdir(candidate.c_str());
      return TransportError{"cannot lock the profile directory: " +
                            std::string(std::strerror(lockError))};
   }
   const Result<void, TransportError> preferences = writePreferences(candidate);
   if (!preferences) {
      ::close(fd);
      (void)removeProfileTree(candidate);
      return preferences.error();
   }

   directory = std::move(candidate);
   lockFd = fd;
   return {};
}

/* A browser leaves thousands of files, so the profile needs a whole tree gone. */
Result<void, TransportError> removeProfileTree(const std::string& path) {
   struct stat status{};
   if (::lstat(path.c_str(), &status) != 0) {
      const int lookupError = errno;
      if (lookupError == ENOENT) {
         return {};
      }
      return TransportError{"cannot remove " + path + ": " +
                            std::strerror(lookupError)};
   }
   if (!S_ISDIR(status.st_mode)) {
      if (::unlink(path.c_str()) == 0) {
         return {};
      }
      return TransportError{"cannot remove " + path + ": " +
                            std::strerror(errno)};
   }

   DIR* directory = ::opendir(path.c_str());
   if (directory == nullptr) {
      return TransportError{"cannot remove " + path + ": " +
                            std::strerror(errno)};
   }

   std::optional<TransportError> firstFailure;
   while (const dirent* entry = ::readdir(directory)) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") {
         continue;
      }
      const Result<void, TransportError> removed =
          removeProfileTree(path + "/" + name);
      if (!removed && !firstFailure.has_value()) {
         firstFailure = removed.error();
      }
   }
   ::closedir(directory);

   if (firstFailure.has_value()) {
      return *firstFailure;
   }
   if (::rmdir(path.c_str()) != 0) {
      return TransportError{"cannot remove " + path + ": " +
                            std::strerror(errno)};
   }
   return {};
}

Result<int, TransportError> sweepAbandonedProfiles() {
   const std::string parent = profileParent();
   DIR* directory = ::opendir(parent.c_str());
   if (directory == nullptr) {
      return TransportError{"cannot scan " + parent + ": " +
                            std::strerror(errno)};
   }

   int removed = 0;
   while (const dirent* entry = ::readdir(directory)) {
      const std::string name(entry->d_name);
      if (name.rfind(kProfilePrefix, 0) != 0) {
         continue;
      }
      const std::string path = parent + "/" + name;
      const int fd = ::open((path + "/" + kLockFile).c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) {
         if (errno == ENOENT && olderThan(path, kUnlockedGraceSeconds)) {
            if (removeProfileTree(path)) {
               ++removed;
            }
         }
         continue;
      }
      const bool owned = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
      ::close(fd);
      if (owned) {
         continue;
      }
      if (removeProfileTree(path)) {
         ++removed;
      }
   }
   ::closedir(directory);
   return removed;
}

std::string plannedProfilePath() {
   return profileParent() + "/" + kProfileTemplate;
}

}  // namespace sdl3mcp

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

/* pidfd_open(2) is what lets one poll set watch the web stack's processes. */
int openPidFd(pid_t pid) {
#ifdef SYS_pidfd_open
   return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
   errno = ENOSYS;
   return -1;
#endif
}

/* The shell-style code a wait status means: exit status, or 128 + signal. */
int exitCode(int status) {
   if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
   }
   if (WIFSIGNALED(status)) {
      return 128 + WTERMSIG(status);
   }
   return status;
}

/* One non-blocking connect: true only when the loopback port accepts now. */
bool loopbackPortAccepts(std::uint16_t port) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return false;
   }
   int ignored = 0;
   (void)setNonBlocking(fd, ignored);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = htons(port);

   bool accepts = false;
   if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) ==
       0) {
      accepts = true;
   } else if (errno == EINPROGRESS) {
      pollfd descriptor{fd, POLLOUT, 0};
      if (::poll(&descriptor, 1, 0) > 0) {
         int socketError = 0;
         socklen_t length = sizeof socketError;
         if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 &&
             socketError == 0) {
            accepts = true;
         }
      }
   }

   ::close(fd);
   return accepts;
}

}  // namespace

WebRun::~WebRun() {
   stop();
}

Result<void, TransportError> WebRun::start(WebLaunchOptions options) {
   options_ = std::move(options);
   stopped_ = false;

   const Result<std::vector<std::vector<std::string>>, TransportError> plan =
       makeWebLaunchPlan(options_);
   if (!plan) {
      stop();
      return plan.error();
   }
   const Result<void, TransportError> page = startChild(pageServer_, (*plan)[0]);
   if (!page) {
      stop();
      return page.error();
   }
   const Result<void, TransportError> bridge = startChild(bridge_, (*plan)[1]);
   if (!bridge) {
      stop();
      return bridge.error();
   }

   readyDeadline_ = Clock::now() + std::chrono::milliseconds(kWebReadyWindowMs);
   return {};
}

Result<void, TransportError> WebRun::startChild(
    Child& child, const std::vector<std::string>& command) {
   if (command.empty()) {
      return TransportError{"the web command is empty"};
   }

   std::vector<char*> argv;
   argv.reserve(command.size() + 1);
   for (const std::string& argument : command) {
      argv.push_back(const_cast<char*>(argument.c_str()));
   }
   argv.push_back(nullptr);

   const pid_t pid = ::fork();
   if (pid < 0) {
      return TransportError{std::string("cannot fork the web child: ") +
                            std::strerror(errno)};
   }

   if (pid == 0) {
      /* Only async-signal-safe calls from here to execvp. */
      sigset_t empty;
      sigemptyset(&empty);
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);
      (void)::setpgid(0, 0);

      /* The Handler's SIGCHLD handling and ignored SIGPIPE are its own. */
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      (void)::sigaction(SIGCHLD, &action, nullptr);
      (void)::sigaction(SIGPIPE, &action, nullptr);

      /*
        The client's stdio is the MCP channel when the Handler serves it: a page
        server or a browser that read stdin would eat requests, and one that
        wrote stdout would corrupt the answers. Their output goes to the
        Handler's stderr with the rest of the web diagnostics.
      */
      const int devNull = ::open("/dev/null", O_RDONLY);
      if (devNull >= 0) {
         ::dup2(devNull, STDIN_FILENO);
         if (devNull != STDIN_FILENO) {
            ::close(devNull);
         }
      }
      ::dup2(STDERR_FILENO, STDOUT_FILENO);

      closeDescriptorsFrom(3);
      ::execvp(argv[0], argv.data());
      ::_exit(127);
   }

   /* Both sides set the group so a stop cannot race the child's own setpgid. */
   (void)::setpgid(pid, pid);

   const int pidfd = openPidFd(pid);
   if (pidfd < 0) {
      const int watchError = errno;
      (void)::kill(-pid, SIGKILL);
      (void)::kill(pid, SIGKILL);
      return TransportError{std::string("cannot watch the web child: ") +
                            std::strerror(watchError)};
   }

   child.pid = pid;
   child.pidfd = pidfd;
   child.started = true;
   child.exited = false;
   child.exitStatus = 0;
   return {};
}

Result<void, TransportError> WebRun::startBrowser() {
   const Result<std::vector<std::vector<std::string>>, TransportError> plan =
       makeWebLaunchPlan(options_);
   if (!plan) {
      return plan.error();
   }
   const Result<void, TransportError> started =
       startChild(browser_, (*plan)[2]);
   if (!started) {
      return started.error();
   }
   browserPid_ = browser_.pid;
   return {};
}

bool WebRun::bothPortsAccept() const {
   return loopbackPortAccepts(options_.pagePort) &&
          loopbackPortAccepts(options_.bridgePort);
}

void WebRun::markExited(Child& child, int status) {
   child.exited = true;
   child.exitStatus = exitCode(status);
   if (child.pidfd >= 0) {
      ::close(child.pidfd);
      child.pidfd = -1;
   }
}

void WebRun::recordExit(pid_t pid, int status) {
   for (Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (child->started && !child->exited && child->pid == pid) {
         markExited(*child, status);
         return;
      }
   }
}

void WebRun::appendPollFds(std::vector<pollfd>& descriptors) const {
   for (const Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (child->started && !child->exited && child->pidfd >= 0) {
         descriptors.push_back(pollfd{child->pidfd, POLLIN, 0});
      }
   }
}

WebStackEvent WebRun::service(Clock::time_point now,
                              const pollfd* webFds,
                              std::size_t webFdCount) {
   WebStackEvent event;
   if (stopped_) {
      return event;
   }

   for (Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (!child->started || child->exited || child->pidfd < 0) {
         continue;
      }
      for (std::size_t index = 0; index < webFdCount; ++index) {
         if (webFds[index].fd == child->pidfd &&
             webFds[index].revents != 0) {
            /*
               The Handler's SIGCHLD reaper may have collected the status
               already; when it has not, waitpid() here takes it. Either way the
               pidfd is closed and the child is marked exited once.
            */
            int status = 0;
            const pid_t reaped = ::waitpid(child->pid, &status, WNOHANG);
            if (reaped == child->pid) {
               markExited(*child, status);
            } else if (reaped < 0 && errno == ECHILD) {
               markExited(*child, 0);
            }
            break;
         }
      }
   }

   /*
     The page server and the bridge are tools: their death is never the
     intended end and is reported. The browser is the application, so its exit
     is the normal end of a run.
   */
   if (pageServer_.exited) {
      event.kind = WebStackEvent::Kind::ToolExited;
      event.child = WebChild::PageServer;
      return event;
   }
   if (bridge_.exited) {
      event.kind = WebStackEvent::Kind::ToolExited;
      event.child = WebChild::Bridge;
      return event;
   }
   if (browser_.exited) {
      event.kind = WebStackEvent::Kind::BrowserExited;
      event.child = WebChild::Browser;
      event.exitStatus = browser_.exitStatus;
      return event;
   }

   if (!browserStarted()) {
      const bool ready = bothPortsAccept();
      if (!ready && now < readyDeadline_) {
         return event;
      }
      if (!ready) {
         std::fprintf(stderr,
                      "sdl3mcp: web: the page server and bridge are not accepting "
                      "connections yet; starting the browser anyway\n");
      }

      const Result<void, TransportError> started = startBrowser();
      if (!started) {
         event.kind = WebStackEvent::Kind::ToolExited;
         event.child = WebChild::Browser;
         event.detail = "the browser could not be started: " +
                        started.error().message;
      }
   }

   return event;
}

std::optional<Clock::time_point> WebRun::nextDeadline() const {
   if (stopped_ || browserStarted()) {
      return std::nullopt;
   }
   return readyDeadline_;
}

void WebRun::closeChildFds() {
   for (Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (child->pidfd >= 0) {
         ::close(child->pidfd);
         child->pidfd = -1;
      }
   }
}

void WebRun::stop(WebStopMode mode) {
   if (stopped_) {
      return;
   }
   stopped_ = true;

   /*
     Every failure and shutdown takes the whole run down; only a browser that
     exited itself leaves nothing of its group to signal.
   */
   std::vector<Child*> children{&pageServer_, &bridge_};
   if (mode == WebStopMode::AllProcesses) {
      children.push_back(&browser_);
   }

   const auto signalGroup = [](pid_t pid, int signal) {
      if (pid <= 0) {
         return;
      }
      if (::kill(-pid, signal) != 0) {
         (void)::kill(pid, signal);
      }
   };

   /*
     SIGTERM reaches the group, not just the leader: websocat may have forked a
     connection handler, and Firefox forks its own content processes.
   */
   for (Child* child : children) {
      if (child->started) {
         signalGroup(child->pid, SIGTERM);
      }
   }

   /* One grace period for every child, then SIGKILL for whatever remains. */
   const auto waitForChildren = [this, &children](Clock::time_point deadline) {
      for (;;) {
         std::vector<pollfd> descriptors;
         for (Child* child : children) {
            if (child->started && !child->exited && child->pidfd >= 0) {
               descriptors.push_back(pollfd{child->pidfd, POLLIN, 0});
            }
         }
         if (descriptors.empty()) {
            return;
         }

         const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - Clock::now())
                                    .count();
         if (remaining <= 0) {
            return;
         }
         const int ready = ::poll(descriptors.data(), descriptors.size(),
                                  static_cast<int>(remaining));
         if (ready < 0) {
            if (errno == EINTR) {
               continue;
            }
            return;
         }
         if (ready == 0) {
            return;
         }
         for (Child* child : children) {
            if (!child->started || child->exited || child->pidfd < 0) {
               continue;
            }
            for (const pollfd& descriptor : descriptors) {
               if (descriptor.fd == child->pidfd && descriptor.revents != 0) {
                  int status = 0;
                  const pid_t reaped = ::waitpid(child->pid, &status, WNOHANG);
                  markExited(*child, reaped == child->pid ? status : 0);
                  break;
               }
            }
         }
      }
   };

   waitForChildren(Clock::now() + std::chrono::milliseconds(kWebStopGraceMs));

   for (Child* child : children) {
      if (child->started) {
         /* The group, again: a leader that exited may have left a descendant. */
         signalGroup(child->pid, SIGKILL);
      }
   }
   waitForChildren(Clock::now() + std::chrono::milliseconds(500));

   closeChildFds();

   if (!options_.profileDir.empty()) {
      const Result<void, TransportError> removed =
          removeProfileTree(options_.profileDir);
      if (!removed) {
         std::fprintf(stderr, "sdl3mcp: web: %s\n",
                      removed.error().message.c_str());
      }
      options_.profileDir.clear();
   }
   if (options_.profileLockFd >= 0) {
      ::close(options_.profileLockFd);
      options_.profileLockFd = -1;
   }

   browserPid_ = 0;
}

}  // namespace sdl3mcp
