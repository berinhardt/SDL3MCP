/*
  The application behind the supervisor, as a data path.

  The proxy loop only ever reads lines, reads a raw payload, writes lines,
  writes raw bytes, and stops. Those five operations are exactly the surface
  Transport already offers, so the seam is named around it: a child over pipes
  and a future peer that arrives from a socket are interchangeable here.

  This interface is deliberately data-only. Lifecycle (spawn, wait, exit
  status, terminate) belongs to the concrete peer for now; the proxy loop has
  no reason to care which transport carries the bytes.
*/

#ifndef SDL3MCP_SUPERVISOR_PEER_HPP
#define SDL3MCP_SUPERVISOR_PEER_HPP

#include "transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp
{

class Peer
{
public:
    virtual ~Peer() = default;

    /* Blocks until a full line, EOF, stop(), a deadline, or a read failure. */
    virtual ReadStatus readLine(std::string &line, std::string &error,
                                int timeoutMs = -1) = 0;

    /* Reads exactly `count` bytes, starting with anything readLine buffered. */
    virtual bool readBytes(size_t count,
                           std::vector<uint8_t> &out,
                           std::string &error,
                           int timeoutMs = -1) = 0;

    /* Appends '\n' and writes every byte. False on a write error. */
    virtual bool writeLine(std::string_view line) = 0;

    /* Writes exactly `size` bytes with nothing appended. False on a write error. */
    virtual bool writeBytes(const uint8_t *data, size_t size) = 0;

    /* Asynchronous and idempotent; safe to call from another thread. */
    virtual void stop() = 0;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_PEER_HPP */
