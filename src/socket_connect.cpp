// TODO(windows): Winsock needs WSAStartup and closesocket; this path is POSIX
// only, matching transport.cpp and stdio_isolation.cpp. See README Platforms.
#include "socket_connect.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

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

} // namespace

bool connectSocket(const char *host, std::uint16_t port, int &fd, std::string &error)
{
    fd = -1;
    error.clear();

    if (host == nullptr || host[0] == '\0') {
        error = "cannot connect to an empty host";
        return false;
    }
    if (port == 0) {
        error = "cannot connect to port 0";
        return false;
    }

    const std::string endpoint = endpointText(host, port);
    const std::string service = std::to_string(port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

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
        if (::connect(socketFd, address->ai_addr, address->ai_addrlen) == 0) {
            ::freeaddrinfo(addresses);
            fd = socketFd;
            return true;
        }
        lastError = errno;
        ::close(socketFd);
    }

    ::freeaddrinfo(addresses);
    if (lastError == 0) {
        /* Non-null address list with no usable entry: report a generic refusal. */
        lastError = ECONNREFUSED;
    }
    error = "cannot connect to " + endpoint + ": " + std::strerror(lastError);
    return false;
}

} // namespace sdl3mcp
