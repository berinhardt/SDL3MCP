#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "socket_connect.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

/*
  Binds an IPv4 loopback socket to an ephemeral port and returns the fd. The
  caller decides whether it listens: a bound but silent socket refuses
  connections deterministically, without racing another process for the port.
*/
int bindLoopback(std::uint16_t &port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0);

    socklen_t length = sizeof address;
    CHECK(::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    port = ntohs(address.sin_port);
    CHECK(port != 0);
    return fd;
}

TEST_CASE("connect to listener")
{
    std::uint16_t port = 0;
    const int listener = bindLoopback(port);
    CHECK(::listen(listener, 1) == 0);

    int fd = -1;
    std::string error = "stale";
    CHECK(sdl3mcp::connectSocket("127.0.0.1", port, fd, error));
    CHECK(fd >= 0);
    CHECK(error.empty());

    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);

    ::close(accepted);
    ::close(fd);
    ::close(listener);
}

TEST_CASE("refused connect")
{
    /* Bound but not listening: the kernel refuses instead of black-holing. */
    std::uint16_t port = 0;
    const int socketFd = bindLoopback(port);

    int fd = -1;
    std::string error;
    CHECK(!sdl3mcp::connectSocket("127.0.0.1", port, fd, error));
    CHECK(fd == -1);
    CHECK(!error.empty());

    const std::string endpoint =
        "127.0.0.1:" + std::to_string(static_cast<unsigned int>(port));
    CHECK(error.find(endpoint) != std::string::npos);

    ::close(socketFd);
}

TEST_CASE("empty host and port")
{
    int fd = 0;
    std::string error;
    CHECK(!sdl3mcp::connectSocket("", 7777, fd, error));
    CHECK(fd == -1);
    CHECK(!error.empty());

    fd = 0;
    error.clear();
    CHECK(!sdl3mcp::connectSocket("127.0.0.1", 0, fd, error));
    CHECK(fd == -1);
    CHECK(!error.empty());
}

} // namespace
