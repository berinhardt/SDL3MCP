#include "Listener.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
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

}  // namespace

Listener::~Listener() {
   close();
}

bool Listener::listen(std::string_view host,
                      std::uint16_t port,
                      std::string& error) {
   error.clear();
   close();

   const std::string endpoint = endpointText(host, port);
   const std::string hostText(host);
   const std::string service = std::to_string(port);

   addrinfo hints{};
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_PASSIVE;

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

      int option = 1;
      (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof option);

      if (!setNonBlocking(fd, lastError)) {
         ::close(fd);
         continue;
      }

      if (::bind(fd, address->ai_addr, address->ai_addrlen) != 0) {
         lastError = errno;
         ::close(fd);
         continue;
      }
      if (::listen(fd, 16) != 0) {
         lastError = errno;
         ::close(fd);
         continue;
      }

      sockaddr_storage bound{};
      socklen_t boundLength = sizeof bound;
      if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLength) ==
          0) {
         if (bound.ss_family == AF_INET) {
            boundPort_ = ntohs(
                reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
         } else if (bound.ss_family == AF_INET6) {
            boundPort_ = ntohs(
                reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
         }
      }

      ::freeaddrinfo(addresses);
      fd_ = fd;
      return true;
   }

   ::freeaddrinfo(addresses);
   if (lastError == 0) {
      lastError = EADDRNOTAVAIL;
   }
   error = "cannot listen on " + endpoint + ": " + std::strerror(lastError);
   return false;
}

int Listener::accept(std::string& error) {
   error.clear();
   if (fd_ < 0) {
      error = "the listener is not open";
      return -1;
   }

   const int client =
       ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
   if (client >= 0) {
      return client;
   }

   if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return -1;
   }
   error = std::string("cannot accept an application connection: ") +
           std::strerror(errno);
   return -1;
}

void Listener::close() {
   if (fd_ < 0) {
      return;
   }
   ::close(fd_);
   fd_ = -1;
   boundPort_ = 0;
}

}  // namespace sdl3mcp
