#include "Web-Plan.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>

namespace sdl3mcp {
namespace {

/* The prefix a seeded profile carries, which is also what the sweep looks for. */
constexpr const char* kProfilePrefix = "sdl3mcp-web-";
/* Long enough that a directory still being created is never mistaken for one. */
constexpr int kUnlockedGraceSeconds = 60;

/* The mtime test that keeps the sweep off a directory a launcher is creating. */
bool olderThan(const std::string& path, int seconds) {
   struct stat status{};
   if (::stat(path.c_str(), &status) != 0) {
      return false;
   }
   return std::time(nullptr) - status.st_mtime > seconds;
}

}  // namespace

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
      const int fd = ::open((path + "/" + kProfileLockFile).c_str(),
                            O_RDWR | O_CLOEXEC);
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

}  // namespace sdl3mcp
