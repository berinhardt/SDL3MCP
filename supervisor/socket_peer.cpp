// TODO(windows): Winsock needs WSAStartup and closesocket; this path is POSIX
// only, matching the rest of the supervisor. See README Platforms.
#include "socket_peer.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

/* The one connection a relay may hold plus nothing to queue: the acceptor is
   the only reader, so a deeper backlog would only delay closing the extras. */
constexpr int kListenBacklog = 4;

/* Prints IPv6 literals in brackets so the error text reads as an endpoint. */
std::string endpointText(const char *host, std::uint16_t port)
{
    std::string text;
    if (host != nullptr && std::strchr(host, ':') != nullptr) {
        text = std::string("[") + host + "]";
    } else {
        text = host != nullptr ? host : "";
    }
    text += ":" + std::to_string(port);
    return text;
}

std::uint16_t portFromAddress(const sockaddr_storage &address)
{
    if (address.ss_family == AF_INET) {
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&address);
        return ntohs(ipv4->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&address);
        return ntohs(ipv6->sin6_port);
    }
    return 0;
}

bool makeNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* A spawner's listener must not leak into the application it forks. */
bool makeCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

} // namespace

SocketPeer::SocketPeer(int fd) : fd_(fd), transport_(fd, fd)
{
}

SocketPeer::~SocketPeer()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

ReadStatus SocketPeer::readLine(std::string &line, std::string &error, int timeoutMs)
{
    return transport_.readLine(line, error, timeoutMs);
}

bool SocketPeer::readBytes(size_t count,
                           std::vector<uint8_t> &out,
                           std::string &error,
                           int timeoutMs)
{
    return transport_.readBytes(count, out, error, timeoutMs);
}

bool SocketPeer::writeLine(std::string_view line)
{
    return transport_.writeLine(line);
}

bool SocketPeer::writeBytes(const uint8_t *data, size_t size)
{
    return transport_.writeBytes(data, size);
}

void SocketPeer::stop()
{
    transport_.stop();
}

bool SocketPeer::closed() const
{
    if (fd_ < 0) {
        return true;
    }

    char probe = 0;
    for (;;) {
        const ssize_t count = ::recv(fd_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count == 0) {
            return true;
        }
        if (count > 0) {
            /* Data is pending, not a clean close; the reader will consume it. */
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        /* A reset or any other socket error ends the session just as an EOF does. */
        return errno != EAGAIN && errno != EWOULDBLOCK;
    }
}

bool listenSocket(const char *host,
                  std::uint16_t port,
                  int &fd,
                  std::uint16_t &boundPort,
                  std::string &error)
{
    fd = -1;
    boundPort = 0;
    error.clear();

    if (host == nullptr || host[0] == '\0') {
        error = "cannot listen on an empty host";
        return false;
    }

    const std::string endpoint = endpointText(host, port);
    const std::string service = std::to_string(port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo *addresses = nullptr;
    const int resolved = ::getaddrinfo(host, service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        error = "cannot resolve " + endpoint + ": " + ::gai_strerror(resolved);
        return false;
    }

    int lastError = 0;
    for (const addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
        const int socketFd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketFd < 0) {
            lastError = errno;
            continue;
        }

        int reuse = 1;
        (void)::setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

        if (!makeCloseOnExec(socketFd)) {
            lastError = errno;
            ::close(socketFd);
            continue;
        }
        if (::bind(socketFd, address->ai_addr, address->ai_addrlen) != 0) {
            lastError = errno;
            ::close(socketFd);
            continue;
        }
        if (::listen(socketFd, kListenBacklog) != 0) {
            lastError = errno;
            ::close(socketFd);
            continue;
        }
        if (!makeNonBlocking(socketFd)) {
            lastError = errno;
            ::close(socketFd);
            continue;
        }

        sockaddr_storage local{};
        socklen_t length = sizeof local;
        if (::getsockname(socketFd, reinterpret_cast<sockaddr *>(&local), &length) == 0) {
            boundPort = portFromAddress(local);
        }
        if (boundPort == 0) {
            boundPort = port;
        }

        ::freeaddrinfo(addresses);
        fd = socketFd;
        return true;
    }

    ::freeaddrinfo(addresses);
    if (lastError == 0) {
        /* Non-null address list with no usable entry: report a generic refusal. */
        lastError = EADDRNOTAVAIL;
    }
    error = "cannot listen on " + endpoint + ": " + std::strerror(lastError);
    return false;
}

} // namespace sdl3mcp
