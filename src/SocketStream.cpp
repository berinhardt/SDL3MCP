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

namespace sdl3mcp {
namespace {

/* Prints IPv6 literals in brackets so the error text reads as an endpoint. */
std::string endpointText(std::string_view host, std::uint16_t port) {
   std::string text;
   if (host.find(':') != std::string_view::npos) {
      text = "[" + std::string(host) + "]";
   } else {
      text = std::string(host);
   }
   text += ":" + std::to_string(port);
   return text;
}

bool setNonBlocking(int fd, int& error) {
   const int flags = ::fcntl(fd, F_GETFL, 0);
   if (flags < 0) {
      error = errno;
      return false;
   }
   if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      error = errno;
      return false;
   }
   return true;
}

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
};

SocketStream::SocketStream() : native_(std::make_unique<Native>()) {
}

SocketStream::~SocketStream() {
   close();
}

bool SocketStream::connect(std::string_view host,
                           std::uint16_t port,
                           std::uint32_t timeoutMs,
                           std::string& error) {
   error.clear();
   close();

   if (host.empty()) {
      error = "cannot connect to an empty host";
      return false;
   }
   if (port == 0) {
      error = "cannot connect to port 0";
      return false;
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
      error = "cannot resolve " + endpoint + ": " + ::gai_strerror(resolved);
      return false;
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
      return true;
   }

   ::freeaddrinfo(addresses);
   if (lastError == 0) {
      /* A non-null address list with no usable entry: report a generic refusal. */
      lastError = ECONNREFUSED;
   }
   error = "cannot connect to " + endpoint + ": " + std::strerror(lastError);
   return false;
}

bool SocketStream::sendText(std::string_view text) {
   if (native_->fd < 0) {
      return false;
   }
   native_->outbound.append(text);
   native_->outbound.push_back('\n');
   return flush();
}

bool SocketStream::sendBinary(const std::uint8_t* data, std::size_t size) {
   if (native_->fd < 0) {
      return false;
   }
   native_->outbound.append(reinterpret_cast<const char*>(data), size);
   return flush();
}

void SocketStream::close() {
   if (!native_ || native_->fd < 0) {
      return;
   }
   ::shutdown(native_->fd, SHUT_RDWR);
   ::close(native_->fd);
   native_->fd = -1;
   native_->outbound.clear();
}

void SocketStream::pump() {
   if (native_->fd < 0 || finished()) {
      return;
   }
   if (!flush()) {
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

bool SocketStream::flush() {
   if (native_->fd < 0) {
      return false;
   }

   std::size_t offset = 0;
   while (offset < native_->outbound.size()) {
      const ssize_t written =
          ::send(native_->fd, native_->outbound.data() + offset,
                 native_->outbound.size() - offset, MSG_NOSIGNAL);
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
      reportWriteFailure(written == 0 ? EPIPE : errno);
      return false;
   }

   native_->outbound.erase(0, offset);
   return true;
}

void SocketStream::reportWriteFailure(int error) {
   const std::string endpoint = native_->endpoint;
   native_->outbound.clear();
   fail("cannot write to " + endpoint + ": " + std::strerror(error));
}

}  // namespace sdl3mcp
