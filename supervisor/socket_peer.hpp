/*
  The application behind the relay, as a data path over one connected socket.

  A relay session is either dialed by the supervisor or accepted from a listener;
  both directions are the same connected fd, so one implementation covers them.
  This step only attaches the accepted socket, but nothing here assumes which
  side ran connect().

  The peer owns the fd it was handed: the acceptor creates it and this object
  closes it when the session ends. Transport still owns the framing and does not
  close anything, exactly as it does for the child's pipes, so the proxy loop
  sees the same Peer operations whichever peer source it is talking to.

  closed() peeks at the socket without consuming a byte. It is the idle-side
  disconnect check; a read that fails mid-call reports the same disconnect to
  the caller, which is why both paths funnel a session end through detachPeer().
*/

#ifndef SDL3MCP_SUPERVISOR_SOCKET_PEER_HPP
#define SDL3MCP_SUPERVISOR_SOCKET_PEER_HPP

#include "peer.hpp"
#include "transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp
{

class SocketPeer : public Peer
{
public:
    explicit SocketPeer(int fd);
    ~SocketPeer() override;

    SocketPeer(const SocketPeer &) = delete;
    SocketPeer &operator=(const SocketPeer &) = delete;

    ReadStatus readLine(std::string &line, std::string &error,
                        int timeoutMs = -1) override;
    bool readBytes(size_t count,
                   std::vector<uint8_t> &out,
                   std::string &error,
                   int timeoutMs = -1) override;
    bool writeLine(std::string_view line) override;
    bool writeBytes(const uint8_t *data, size_t size) override;
    void stop() override;
    bool closed() const override;

    int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    Transport transport_;
};

/*
  Binds a TCP listener on `host`:`port` with SO_REUSEADDR and returns its fd.
  Port 0 asks the kernel for an ephemeral port; the one actually bound comes
  back through `boundPort`. The listener is non-blocking so an acceptor can
  poll it and then accept without ever parking in accept().

  The error names the address plus the resolver or bind failure, the same way
  connectSocket does.
*/
bool listenSocket(const char *host,
                  std::uint16_t port,
                  int &fd,
                  std::uint16_t &boundPort,
                  std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_SOCKET_PEER_HPP */
