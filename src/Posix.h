/*
  The small POSIX helpers the native stream, the listener, and the web stack
  share. They are header-only because each is a few lines and the callers already
  live in desktop-only translation units; the browser arm includes neither this
  header nor these symbols.

  Both helpers are async-signal-safe: closeDescriptorsFrom runs between fork and
  exec, so it may only call close_range, getrlimit, and close.
*/

#pragma once

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>

namespace sdl3mcp {

/* Makes `fd` non-blocking; false leaves errno's value in `errorCode`. */
inline bool setNonBlocking(int fd, int& errorCode) {
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

/*
  Closes every descriptor at or above `lowest`. A child must inherit none of the
  Handler's streams; close_range is one syscall, so it is safe between fork and
  exec, and the rlimit loop is the fallback for a kernel without it.
*/
inline void closeDescriptorsFrom(int lowest) {
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

}  // namespace sdl3mcp
