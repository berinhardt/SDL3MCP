#include "Web-Plan.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sdl3mcp {
namespace {

/* The template mkdtemp() fills; its suffix must be the last six characters. */
constexpr const char* kProfileTemplate = "sdl3mcp-web-XXXXXX";
constexpr const char* kFallbackProfileParent = "/tmp";

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

bool isWritableDirectory(const char* path) {
   if (path == nullptr || path[0] == '\0') {
      return false;
   }
   struct stat status{};
   return ::stat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
          ::access(path, W_OK) == 0;
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

}  // namespace

std::string profileParent() {
   const char* runtime = std::getenv("XDG_RUNTIME_DIR");
   if (isWritableDirectory(runtime)) {
      return runtime;
   }
   return kFallbackProfileParent;
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
   const int fd = ::open((candidate + "/" + kProfileLockFile).c_str(),
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

std::string plannedProfilePath() {
   return profileParent() + "/" + kProfileTemplate;
}

}  // namespace sdl3mcp
