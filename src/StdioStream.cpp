#include "StdioStream.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace sdl3mcp {
namespace {

/* Same bound as SocketStream: a client that never reads must not grow us. */
constexpr std::size_t kMaxOutboundBytes = 128u * 1024u * 1024u;

std::string describe(std::string_view context, int errorCode) {
   return std::string(context) + ": " + std::strerror(errorCode);
}

}  // namespace

StdioStream::StdioStream() : inputFd_(STDIN_FILENO), outputFd_(STDOUT_FILENO) {
}

StdioStream::~StdioStream() {
   close();
   if (savedOutputFlags_ >= 0) {
      (void)::fcntl(outputFd_, F_SETFL, savedOutputFlags_);
      savedOutputFlags_ = -1;
   }
}

Result<void, TransportError> StdioStream::start() {
   if (started_) {
      return {};
   }

   const int flags = ::fcntl(outputFd_, F_GETFL, 0);
   if (flags < 0) {
      return TransportError{describe("cannot read stdout flags", errno)};
   }
   savedOutputFlags_ = flags;
   if (::fcntl(outputFd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      savedOutputFlags_ = -1;
      return TransportError{describe("cannot make stdout non-blocking", errno)};
   }

   started_ = true;
   open();
   return {};
}

bool StdioStream::sendText(std::string_view text) {
   if (closed_) {
      return false;
   }
   if (!append(text.data(), text.size())) {
      return false;
   }
   if (!append("\n", 1)) {
      return false;
   }
   return flush();
}

bool StdioStream::sendBinary(const std::uint8_t* data, std::size_t size) {
   if (closed_) {
      return false;
   }
   if (!append(reinterpret_cast<const char*>(data), size)) {
      return false;
   }
   return flush();
}

bool StdioStream::append(const char* data, std::size_t size) {
   outbound_.append(data, size);
   if (outbound_.size() > kMaxOutboundBytes) {
      fail("outbound buffer exceeded " + std::to_string(kMaxOutboundBytes) +
           " bytes");
      return false;
   }
   return true;
}

void StdioStream::close() {
   if (closed_) {
      return;
   }
   closed_ = true;
   outbound_.clear();
   setWantsWrite(false);
}

void StdioStream::pump() {
   if (closed_) {
      return;
   }
   if (!flush()) {
      return;
   }

   /*
     Only read when poll() already says stdin has data. A single read is enough:
     a blocking stdin could stall a second one, and the loop re-polls after this
     pass.
   */
   pollfd descriptor{};
   descriptor.fd = inputFd_;
   descriptor.events = POLLIN;
   const int ready = ::poll(&descriptor, 1, 0);
   if (ready <= 0) {
      return;
   }

   char buffer[4096];
   const ssize_t count = ::read(inputFd_, buffer, sizeof buffer);
   if (count > 0) {
      feed(reinterpret_cast<const std::uint8_t*>(buffer),
           static_cast<std::size_t>(count));
      return;
   }
   if (count == 0) {
      finish();
      return;
   }
   if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
   }
   fail(describe("cannot read stdin", errno));
}

bool StdioStream::flush() {
   if (closed_) {
      return false;
   }

   std::size_t offset = 0;
   while (offset < outbound_.size()) {
      const ssize_t written =
          ::write(outputFd_, outbound_.data() + offset, outbound_.size() - offset);
      if (written > 0) {
         offset += static_cast<std::size_t>(written);
         continue;
      }
      if (written < 0 && errno == EINTR) {
         continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
         break;
      }
      fail(describe("cannot write stdout", written == 0 ? EPIPE : errno));
      return false;
   }

   outbound_.erase(0, offset);
   setWantsWrite(!outbound_.empty());
   return true;
}

}  // namespace sdl3mcp
