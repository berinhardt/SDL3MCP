// TODO(windows): Winsock needs WSAStartup, closesocket, and ioctlsocket for the
// non-blocking arm; this path is POSIX only, matching the platform stance in
// README Platforms.
#include "SocketStream.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "Configuration.h"
#include "Posix.h"

namespace sdl3mcp {
namespace {

/*
  The most a peer's write buffer may hold. A frame answer can be large, but a
  client that never reads must not grow the process without bound: past this the
  stream fails and the session closes.
*/
constexpr std::size_t kMaxOutboundBytes = 128u * 1024u * 1024u;

/* Waits for a non-blocking connect to settle, bounded by the dial timeout. */
bool waitForConnect(int fd, std::uint32_t timeoutMs, int& error) {
   pollfd descriptor{};
   descriptor.fd = fd;
   descriptor.events = POLLOUT;

   int ready = 0;
   do {
      ready = ::poll(&descriptor, 1, static_cast<int>(timeoutMs));
   } while (ready < 0 && errno == EINTR);

   if (ready == 0) {
      error = ETIMEDOUT;
      return false;
   }
   if (ready < 0) {
      error = errno;
      return false;
   }

   int socketError = 0;
   socklen_t length = sizeof socketError;
   if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0) {
      error = errno;
      return false;
   }
   if (socketError != 0) {
      error = socketError;
      return false;
   }
   return true;
}

}  // namespace

struct SocketStream::Native {
   int fd = -1;
   std::string endpoint;
   std::string outbound;

   /* Writes as much of the outbound buffer as the socket accepts. */
   bool flush(SocketStream& stream);
   void reportWriteFailure(SocketStream& stream, int error);
};

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

SocketStream::SocketStream() : native_(std::make_unique<Native>()) {
}

SocketStream::~SocketStream() {
   close();
}

Result<void, TransportError> SocketStream::connect(std::string_view host,
                                                   std::uint16_t port,
                                                   std::uint32_t timeoutMs) {
   close();

   if (host.empty()) {
      return TransportError{"cannot connect to an empty host"};
   }
   if (port == 0) {
      return TransportError{"cannot connect to port 0"};
   }

   const std::string endpoint = endpointText(host, port);
   const std::string service = std::to_string(port);
   const std::string hostText(host);

   addrinfo hints{};
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   addrinfo* addresses = nullptr;
   const int resolved =
       ::getaddrinfo(hostText.c_str(), service.c_str(), &hints, &addresses);
   if (resolved != 0) {
      return TransportError{"cannot resolve " + endpoint + ": " +
                            ::gai_strerror(resolved)};
   }

   int lastError = 0;
   for (const addrinfo* address = addresses; address != nullptr;
        address = address->ai_next) {
      const int fd =
          ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd < 0) {
         lastError = errno;
         continue;
      }

      if (!setNonBlocking(fd, lastError)) {
         ::close(fd);
         continue;
      }

      bool connected = false;
      if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
         connected = true;
      } else if (errno == EINPROGRESS) {
         connected = waitForConnect(fd, timeoutMs, lastError);
      } else {
         lastError = errno;
      }

      if (!connected) {
         ::close(fd);
         continue;
      }

      ::freeaddrinfo(addresses);
      native_->fd = fd;
      native_->endpoint = endpoint;
      native_->outbound.clear();
      open();
      return {};
   }

   ::freeaddrinfo(addresses);
   if (lastError == 0) {
      /* A non-null address list with no usable entry: report a generic refusal. */
      lastError = ECONNREFUSED;
   }
   return TransportError{"cannot connect to " + endpoint + ": " +
                         std::strerror(lastError)};
}

Result<void, TransportError> SocketStream::adopt(int fd,
                                                 std::string_view endpoint) {
   close();
   if (fd < 0) {
      return TransportError{"cannot adopt an invalid socket"};
   }

   int lastError = 0;
   if (!setNonBlocking(fd, lastError)) {
      ::close(fd);
      return TransportError{"cannot adopt the connection: " +
                            std::string(std::strerror(lastError))};
   }

   native_->fd = fd;
   native_->endpoint = std::string(endpoint);
   native_->outbound.clear();
   open();
   return {};
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

void SocketStream::close() {
   if (!native_ || native_->fd < 0) {
      return;
   }
   ::shutdown(native_->fd, SHUT_RDWR);
   ::close(native_->fd);
   native_->fd = -1;
   native_->outbound.clear();
   setWantsWrite(false);
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
