/*
  The client-facing stdio path as a Peer.

  In the default and relay modes the client is the parent process on stdin and
  stdout, so those fds are borrowed and never closed. The Peer interface only
  needs to forward framing and stop to Transport; closed() is false because a
  stdin EOF is always reported by readLine() itself.

  With --connect the same interface is fulfilled by SocketPeer, which owns the
  dialed socket, so Supervisor never has to know which upstream it got.
*/

#ifndef SDL3MCP_SUPERVISOR_STDIO_PEER_HPP
#define SDL3MCP_SUPERVISOR_STDIO_PEER_HPP

#include "peer.hpp"
#include "transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp
{

class StdioPeer : public Peer
{
public:
    StdioPeer(int readFd, int writeFd);
    ~StdioPeer() override = default;

    StdioPeer(const StdioPeer &) = delete;
    StdioPeer &operator=(const StdioPeer &) = delete;

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

private:
    Transport transport_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_STDIO_PEER_HPP */
