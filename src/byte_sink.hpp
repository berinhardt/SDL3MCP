/*
  The write half of a transport, as the protocol layer sees it.

  Answering a request needs exactly two write shapes: one line of JSON, and the
  raw pixel bytes that follow a capture result header. The code that answers
  must not name the POSIX fd transport to do that, because on Emscripten the
  same answers travel over a browser WebSocket instead, so it takes a ByteSink
  and stays free of conditional compilation.

  Nothing here reads, closes, stops, or knows about fds, SDL, or sockets: only
  the ownership of the bytes the caller hands over is defined, and that
  ownership transfers on the call.

  This is a private header. Both the fd transport and the browser peer derive
  from it, which is why its includes are limited to the three types its
  signatures mention.
*/

#ifndef SDL3MCP_BYTE_SINK_HPP
#define SDL3MCP_BYTE_SINK_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sdl3mcp
{

class ByteSink
{
public:
    virtual ~ByteSink() = default;

    /* Appends '\n' and writes every byte. False on a write error, such as EPIPE. */
    virtual bool writeLine(std::string_view line) = 0;

    /*
      Writes exactly `size` bytes with nothing appended. False on a write error;
      either every byte is written or the caller sees a failure.
    */
    virtual bool writeBytes(const uint8_t *data, size_t size) = 0;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_BYTE_SINK_HPP */
