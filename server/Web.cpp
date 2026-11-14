#include "Web.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
#include <string>
#include <string_view>
#include <utility>

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
bool appendWebFile(const std::string& path,
                   WebApps& loaded,
                   std::map<std::string, std::string>& sources,
                   std::string& error) {
   std::ifstream file(path, std::ios::binary);
   if (!file) {
      error = "cannot open web file \"" + path + "\": " + std::strerror(errno);
      return false;
   }

   const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
   if (document.is_discarded()) {
      error = "web file \"" + path + "\" is not valid JSON";
      return false;
   }
   if (!document.is_object()) {
      error = "web file \"" + path + "\" must contain a JSON object";
      return false;
   }
   if (document.empty()) {
      error = "web file \"" + path + "\" defines no aliases";
      return false;
   }

   for (auto entry = document.begin(); entry != document.end(); ++entry) {
      const std::string alias = entry.key();
      if (isReservedAlias(alias)) {
         error = "web entry \"" + alias +
                 "\" must not use the reserved app_spawn_ prefix";
         return false;
      }
      if (!isValidAlias(alias)) {
         error = "web alias \"" + alias + "\" must match [a-z0-9_]+";
         return false;
      }
      if (!entry->is_string()) {
         error = "web entry \"" + alias + "\" must be a non-empty string";
         return false;
      }

      std::string root = entry->get<std::string>();
      if (root.empty()) {
         error = "web entry \"" + alias + "\" must be a non-empty string";
         return false;
      }

      const auto existing = sources.find(alias);
      if (existing != sources.end()) {
         error = "web alias \"" + alias + "\" is defined in both \"" +
                 existing->second + "\" and \"" + path + "\"";
         return false;
      }

      sources.emplace(alias, path);
      loaded.push_back(WebApp{alias, std::move(root), path});
   }

   return true;
}

}  // namespace

bool loadWebApps(const std::vector<std::string>& paths,
                 WebApps& apps,
                 std::string& error) {
   apps.clear();
   error.clear();

   if (paths.empty()) {
      error = "no web sources were given";
      return false;
   }

   WebApps loaded;
   std::map<std::string, std::string> sources;
   for (const std::string& path : paths) {
      struct stat info{};
      if (::stat(path.c_str(), &info) != 0) {
         error = "cannot open web file \"" + path + "\": " +
                 std::strerror(errno);
         return false;
      }
      if (!S_ISREG(info.st_mode)) {
         error = "web path \"" + path + "\" is not a file";
         return false;
      }
      if (!appendWebFile(path, loaded, sources, error)) {
         return false;
      }
   }

   apps = std::move(loaded);
   return true;
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
  keep a throwaway profile from stopping on first-run prompts.
*/
constexpr const char* kProfilePreferences =
    "user_pref(\"dom.allow_scripts_to_close_windows\", true);\n"
    "user_pref(\"browser.tabs.warnOnClose\", false);\n"
    "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
    "user_pref(\"browser.startup.page\", 0);\n"
    "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);\n"
    "user_pref(\"app.update.auto\", false);\n";

bool fail(std::string message, std::string& error) {
   error = std::move(message);
   return false;
}

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

bool writePreferences(const std::string& directory, std::string& error) {
   const std::string path = directory + "/user.js";
   std::FILE* file = std::fopen(path.c_str(), "w");
   if (file == nullptr) {
      const int openError = errno;
      return fail("cannot write " + path + ": " + std::strerror(openError),
                  error);
   }

   const std::size_t length = std::strlen(kProfilePreferences);
   const bool written = std::fwrite(kProfilePreferences, 1, length, file) == length;
   const bool closed = std::fclose(file) == 0;
   if (!written || !closed) {
      return fail("cannot write " + path, error);
   }
   return true;
}

/* The mtime test that keeps the sweep off a directory a launcher is creating. */
bool olderThan(const std::string& path, int seconds) {
   struct stat status{};
   if (::stat(path.c_str(), &status) != 0) {
      return false;
   }
   return std::time(nullptr) - status.st_mtime > seconds;
}

bool setNonBlocking(int fd, int& errorCode) {
   const int flags = ::fcntl(fd, F_GETFL, 0);
   if (flags < 0) {
      errorCode = errno;
      return false;
   }
   if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      errorCode = errno;
      return false;
   }
   return true;
}

}  // namespace

bool preflightWebLaunch(WebLaunchOptions& options, std::string& error) {
   std::string websocat;
   if (!findOnPath(kWebSocatExecutable, websocat)) {
      return fail("websocat not found: install websocat (cargo install websocat)",
                  error);
   }

   std::string python;
   if (!findOnPath(kPythonExecutable, python)) {
      return fail("python3 not found: install python3", error);
   }

   struct stat status{};
   if (::stat(options.pageRoot.c_str(), &status) != 0 ||
       !S_ISDIR(status.st_mode)) {
      return fail("page root " + options.pageRoot + " is not a directory", error);
   }
   const std::string entryPath = options.pageRoot + "/" + options.entry;
   if (::stat(entryPath.c_str(), &status) != 0) {
      return fail("page root " + options.pageRoot + " does not contain " +
                      options.entry,
                  error);
   }

   std::string browser;
   if (!resolveBrowser(browser, error)) {
      return false;
   }

   options.websocat = std::move(websocat);
   options.python = std::move(python);
   options.browser = std::move(browser);
   return true;
}

bool pickLoopbackPort(std::uint16_t& port, std::string& error) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return fail(std::string("cannot open a loopback socket: ") +
                      std::strerror(errno),
                  error);
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
      return fail(std::string("cannot reserve a loopback port: ") +
                      std::strerror(bindError),
                  error);
   }

   socklen_t length = sizeof address;
   if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      const int nameError = errno;
      ::close(fd);
      return fail(std::string("cannot read the reserved loopback port: ") +
                      std::strerror(nameError),
                  error);
   }

   port = ntohs(address.sin_port);
   ::close(fd);
   return true;
}

bool pickWebPorts(WebLaunchOptions& options, std::string& error) {
   std::uint16_t pagePort = 0;
   if (!pickLoopbackPort(pagePort, error)) {
      return false;
   }

   /*
     A released port can be chosen again, so the bridge is retried until it
     differs; the page server binds first, which makes a collision a real
     startup failure rather than a plan-time guess.
   */
   std::uint16_t bridgePort = 0;
   for (int attempt = 0; attempt < 16; ++attempt) {
      if (!pickLoopbackPort(bridgePort, error)) {
         return false;
      }
      if (bridgePort != pagePort) {
         options.pagePort = pagePort;
         options.bridgePort = bridgePort;
         return true;
      }
   }

   return fail("cannot reserve two distinct loopback ports", error);
}

bool prepareWebLaunch(WebLaunchOptions& options, std::string& error) {
   if (!preflightWebLaunch(options, error)) {
      return false;
   }

   std::string sweepError;
   const int swept = sweepAbandonedProfiles(sweepError);
   if (swept < 0) {
      return fail(sweepError, error);
   }
   if (swept > 0) {
      std::fprintf(stderr, "sdl3mcp: web: reclaimed %d abandoned profile(s)\n",
                   swept);
   }

   std::string directory;
   int lockFd = -1;
   if (!seedProfile(directory, lockFd, error)) {
      return false;
   }
   options.profileDir = std::move(directory);
   options.profileLockFd = lockFd;

   if (!pickWebPorts(options, error)) {
      std::string ignored;
      (void)removeProfileTree(options.profileDir, ignored);
      ::close(options.profileLockFd);
      options.profileDir.clear();
      options.profileLockFd = -1;
      return false;
   }

   return true;
}

bool makeWebLaunchPlan(const WebLaunchOptions& options,
                       std::vector<std::vector<std::string>>& plan,
                       std::string& error) {
   plan.clear();

   if (options.pageRoot.empty()) {
      return fail("pageRoot must not be empty", error);
   }
   if (options.entry.empty()) {
      return fail("entry must not be empty", error);
   }
   if (!isExecutable(options.python)) {
      return fail("python must name an executable", error);
   }
   if (!isExecutable(options.websocat)) {
      return fail("websocat must name an executable", error);
   }
   if (options.browser.empty()) {
      return fail("browser must not be empty", error);
   }
   if (options.handlerEndpoint.empty()) {
      return fail("handlerEndpoint must not be empty", error);
   }
   if (options.pagePort == 0) {
      return fail("pagePort must not be zero", error);
   }
   if (options.bridgePort == 0) {
      return fail("bridgePort must not be zero", error);
   }
   if (options.pagePort == options.bridgePort) {
      return fail("pagePort and bridgePort must differ", error);
   }

   const std::string pagePort = std::to_string(options.pagePort);
   const std::string bridgePort = std::to_string(options.bridgePort);
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
   return true;
}

bool resolveBrowser(std::string& path, std::string& error) {
   const char* configured = std::getenv("BROWSER");
   if (configured != nullptr) {
      const std::string candidate(configured);
      const std::string name = baseName(candidate);
      std::string absolute;
      if ((name == "firefox" || name == "firefox-esr") &&
          isExecutable(candidate) && absolutePath(candidate, absolute)) {
         path = std::move(absolute);
         return true;
      }
   }

   for (const char* name : {"firefox", "firefox-esr"}) {
      std::string found;
      if (findOnPath(name, found)) {
         path = std::move(found);
         return true;
      }
   }

   return fail(
       "no isolated browser available: install firefox; "
       "no other browser is supported",
       error);
}

bool seedProfile(std::string& directory, int& lockFd, std::string& error) {
   const std::string parent = profileParent();
   std::string candidate = parent + "/" + kProfileTemplate;
   if (::mkdtemp(candidate.data()) == nullptr) {
      const int createError = errno;
      return fail("cannot create a profile directory under " + parent + ": " +
                      std::strerror(createError),
                  error);
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
      return fail("cannot create the profile lock: " +
                      std::string(std::strerror(openError)),
                  error);
   }
   if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int lockError = errno;
      ::close(fd);
      ::rmdir(candidate.c_str());
      return fail("cannot lock the profile directory: " +
                      std::string(std::strerror(lockError)),
                  error);
   }
   if (!writePreferences(candidate, error)) {
      ::close(fd);
      std::string ignored;
      (void)removeProfileTree(candidate, ignored);
      return false;
   }

   directory = std::move(candidate);
   lockFd = fd;
   return true;
}

/* A browser leaves thousands of files, so the profile needs a whole tree gone. */
bool removeProfileTree(const std::string& path, std::string& error) {
   struct stat status{};
   if (::lstat(path.c_str(), &status) != 0) {
      const int lookupError = errno;
      if (lookupError == ENOENT) {
         return true;
      }
      error = "cannot remove " + path + ": " + std::strerror(lookupError);
      return false;
   }
   if (!S_ISDIR(status.st_mode)) {
      if (::unlink(path.c_str()) == 0) {
         return true;
      }
      error = "cannot remove " + path + ": " + std::strerror(errno);
      return false;
   }

   DIR* directory = ::opendir(path.c_str());
   if (directory == nullptr) {
      error = "cannot remove " + path + ": " + std::strerror(errno);
      return false;
   }

   bool removed = true;
   std::string firstFailure;
   while (const dirent* entry = ::readdir(directory)) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") {
         continue;
      }
      std::string entryFailure;
      if (!removeProfileTree(path + "/" + name, entryFailure) && removed) {
         removed = false;
         firstFailure = std::move(entryFailure);
      }
   }
   ::closedir(directory);

   if (!removed) {
      error = std::move(firstFailure);
      return false;
   }
   if (::rmdir(path.c_str()) != 0) {
      error = "cannot remove " + path + ": " + std::strerror(errno);
      return false;
   }
   return true;
}

int sweepAbandonedProfiles(std::string& error) {
   error.clear();
   const std::string parent = profileParent();
   DIR* directory = ::opendir(parent.c_str());
   if (directory == nullptr) {
      error = "cannot scan " + parent + ": " + std::strerror(errno);
      return -1;
   }

   int removed = 0;
   while (const dirent* entry = ::readdir(directory)) {
      const std::string name(entry->d_name);
      if (name.rfind(kProfilePrefix, 0) != 0) {
         continue;
      }
      const std::string path = parent + "/" + name;
      std::string ignored;
      const int fd = ::open((path + "/" + kLockFile).c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) {
         if (errno == ENOENT && olderThan(path, kUnlockedGraceSeconds)) {
            if (removeProfileTree(path, ignored)) {
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
      if (removeProfileTree(path, ignored)) {
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

/* One non-blocking connect: true only when the loopback port accepts now. */
bool loopbackPortAccepts(std::uint16_t port) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return false;
   }
   const int flags = ::fcntl(fd, F_GETFL, 0);
   if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
   }

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

/* A web child inherits no descriptor of the Handler's, not even a pidfd. */
void closeDescriptorsFrom(int lowest) {
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

}  // namespace

WebRun::~WebRun() { stop(); }

bool WebRun::start(WebLaunchOptions options, std::string& error) {
   options_ = std::move(options);
   stopped_ = false;

   std::vector<std::vector<std::string>> plan;
   if (!makeWebLaunchPlan(options_, plan, error)) {
      stop();
      return false;
   }
   if (!startChild(pageServer_, plan[0], error) ||
       !startChild(bridge_, plan[1], error)) {
      stop();
      return false;
   }

   readyDeadline_ = Clock::now() + std::chrono::milliseconds(kWebReadyWindowMs);
   return true;
}

bool WebRun::startChild(Child& child,
                        const std::vector<std::string>& command,
                        std::string& error) {
   if (command.empty()) {
      return fail("the web command is empty", error);
   }

   std::vector<char*> argv;
   argv.reserve(command.size() + 1);
   for (const std::string& argument : command) {
      argv.push_back(const_cast<char*>(argument.c_str()));
   }
   argv.push_back(nullptr);

   const pid_t pid = ::fork();
   if (pid < 0) {
      return fail(std::string("cannot fork the web child: ") +
                      std::strerror(errno),
                  error);
   }

   if (pid == 0) {
      /* Only async-signal-safe calls from here to execvp. */
      sigset_t empty;
      sigemptyset(&empty);
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);
      (void)::setpgid(0, 0);

      /* The Handler's SIGCHLD policy and ignored SIGPIPE are its own. */
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      (void)::sigaction(SIGCHLD, &action, nullptr);
      (void)::sigaction(SIGPIPE, &action, nullptr);

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
      return fail(std::string("cannot watch the web child: ") +
                      std::strerror(watchError),
                  error);
   }

   child.pid = pid;
   child.pidfd = pidfd;
   child.started = true;
   child.exited = false;
   return true;
}

bool WebRun::startBrowser(std::string& error) {
   std::vector<std::vector<std::string>> plan;
   if (!makeWebLaunchPlan(options_, plan, error)) {
      return false;
   }
   if (!startChild(browser_, plan[2], error)) {
      return false;
   }
   browserPid_ = browser_.pid;
   return true;
}

bool WebRun::bothPortsAccept() const {
   return loopbackPortAccepts(options_.pagePort) &&
          loopbackPortAccepts(options_.bridgePort);
}

void WebRun::markExited(Child& child) {
   child.exited = true;
   if (child.pidfd >= 0) {
      ::close(child.pidfd);
      child.pidfd = -1;
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
            markExited(*child);
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

      std::string error;
      if (!startBrowser(error)) {
         event.kind = WebStackEvent::Kind::ToolExited;
         event.child = WebChild::Browser;
         event.detail = "the browser could not be started: " + error;
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
                  markExited(*child);
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
      std::string error;
      if (!removeProfileTree(options_.profileDir, error)) {
         std::fprintf(stderr, "sdl3mcp: web: %s\n", error.c_str());
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
