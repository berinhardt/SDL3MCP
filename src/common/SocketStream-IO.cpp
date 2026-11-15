/*
  The native arm's I/O half: the descriptor accessors, the outbound buffer's
  flush, sendText/sendBinary, and pump()'s reads. The dial and close are in
  SocketStream.cpp; both units share the PIMPL state through
  SocketStream-Native.h.
*/

#include "SocketStream.h"

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "SocketStream-Native.h"

namespace sdl3mcp {
namespace {

/*
  The most a peer's write buffer may hold. A frame answer can be large, but a
  client that never reads must not grow the process without bound: past this the
  stream fails and the session closes.
*/
constexpr std::size_t kMaxOutboundBytes = 128u * 1024u * 1024u;

}  // namespace

bool SocketStream::Native::flush(SocketStream& stream) {
   if (fd < 0) {
      return false;
   }

   std::size_t offset = 0;
   while (offset < outbound.size()) {
      const ssize_t written =
          ::send(fd, outbound.data() + offset, outbound.size() - offset,
                 MSG_NOSIGNAL);
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
      /* A zero-length send on a non-empty buffer means the peer is gone. */
      reportWriteFailure(stream, written == 0 ? EPIPE : errno);
      return false;
   }

   outbound.erase(0, offset);
   stream.setWantsWrite(!outbound.empty());
   return true;
}

void SocketStream::Native::reportWriteFailure(SocketStream& stream, int error) {
   const std::string endpointName = endpoint;
   outbound.clear();
   stream.fail("cannot write to " + endpointName + ": " + std::strerror(error));
}

int SocketStream::fd() const noexcept {
   return native_->fd;
}

int SocketStream::outputFd() const noexcept {
   /* One descriptor carries both directions. */
   return native_->fd;
}

bool SocketStream::sendText(std::string_view text) {
   if (native_->fd < 0) {
      return false;
   }
   native_->outbound.append(text);
   native_->outbound.push_back('\n');
   if (native_->outbound.size() > kMaxOutboundBytes) {
      fail("outbound buffer exceeded " + std::to_string(kMaxOutboundBytes) +
           " bytes");
      return false;
   }
   return native_->flush(*this);
}

bool SocketStream::sendBinary(const std::uint8_t* data, std::size_t size) {
   if (native_->fd < 0) {
      return false;
   }
   native_->outbound.append(reinterpret_cast<const char*>(data), size);
   if (native_->outbound.size() > kMaxOutboundBytes) {
      fail("outbound buffer exceeded " + std::to_string(kMaxOutboundBytes) +
           " bytes");
      return false;
   }
   return native_->flush(*this);
}

void SocketStream::pump() {
   if (native_->fd < 0 || finished()) {
      return;
   }
   if (!native_->flush(*this)) {
      return;
   }

   for (;;) {
      char buffer[4096];
      const ssize_t count = ::recv(native_->fd, buffer, sizeof buffer, 0);
      if (count > 0) {
         feed(reinterpret_cast<const std::uint8_t*>(buffer),
              static_cast<std::size_t>(count));
         /* A handler may close the stream while answering a line. */
         if (finished() || native_->fd < 0) {
            return;
         }
         continue;
      }
      if (count == 0) {
         finish();
         return;
      }
      if (errno == EINTR) {
         continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
         return;
      }
      fail("cannot read from " + native_->endpoint + ": " + std::strerror(errno));
      return;
   }
}

}  // namespace sdl3mcp
