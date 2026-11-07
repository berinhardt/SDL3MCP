/*
  Outbound socket transport: dials the MCP client instead of owning stdio.

  The app is expected to live outside the client's process tree, so the client
  listens and the app connects to HOST:PORT. This helper owns exactly one
  attempt over every address `getaddrinfo` returns; there is no retry loop,
  because an unreachable debug endpoint must surface as a startup failure rather
  than a server that silently never appears.
*/

#ifndef SDL3MCP_SOCKET_CONNECT_HPP
#define SDL3MCP_SOCKET_CONNECT_HPP

#include <cstdint>
#include <string>

namespace sdl3mcp
{

/*
  Connects a blocking TCP stream to `host`:`port`. On success `fd` is the
  connected socket and `error` is empty; on failure `fd` is -1 and `error`
  names the endpoint plus the resolver or connect error.
*/
bool connectSocket(const char *host, std::uint16_t port, int &fd, std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_SOCKET_CONNECT_HPP */
