// TODO(windows): Winsock needs WSAStartup, closesocket, and ioctlsocket for the
// non-blocking arm; this path is POSIX only, matching the platform stance in
// README Platforms.

/*
  The native arm of SocketStream: the dial, adoption and close. The writes, the
  reads and pump() are in SocketStream-IO.cpp; both units share the PIMPL state
  through SocketStream-Native.h.
*/

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
#include "SocketStream-Native.h"

namespace sdl3mcp {
namespace {

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

}  // namespace sdl3mcp
