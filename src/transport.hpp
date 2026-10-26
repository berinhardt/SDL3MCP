/*
  The line transport: one fd in, one fd out, one JSON-RPC message per line.

  It owns framing only. It does not parse JSON, know method names, or call SDL;
  that keeps the protocol decisions in json_rpc/session and lets the server loop
  be a plain read/parse/dispatch/write cycle. The fds are not owned: closing them
  belongs to whoever created them, so the transport can be pointed at a pipe in a
  test exactly as it is pointed at stdin/stdout in the app.

  stop() only sets a flag. The reader notices it at the next poll tick (50 ms) and
  returns Stopped without the fd being closed. That is deliberate: closing an fd
  another thread is blocked on is how you get use-after-close, and shutdown must
  not depend on racing the client.

  readLine() buffers partial input between calls, so a request that arrives in
  pieces is still one line. A trailing '\r' is stripped to accept CRLF clients.
  A line longer than maxLineBytes is Error rather than an unbounded allocation,
  which is the only way a hostile or broken client can grow our memory.
*/

#ifndef SDL3MCP_TRANSPORT_HPP
#define SDL3MCP_TRANSPORT_HPP

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace sdl3mcp
{

enum class ReadStatus
{
    /* A complete line is in `line`, without its newline or a trailing '\r'. */
    Line,
    /* The peer closed its end and no partial line was pending. */
    Eof,
    /* stop() was called; nothing was read. */
    Stopped,
    /* Read failure, a line over the cap, or a peer that vanished mid-line. */
    Error
};

class Transport
{
public:
    Transport(int readFd, int writeFd, size_t maxLineBytes = 1024 * 1024);

    /*
      Blocks until a full line is buffered, the peer closes, stop() is observed,
      or reading fails. `error` is filled only for Error and is cleared otherwise.
    */
    ReadStatus readLine(std::string &line, std::string &error);

    /* Appends '\n' and writes every byte. False on a write error, such as EPIPE. */
    bool writeLine(std::string_view line);

    /* Asynchronous and idempotent; safe to call from another thread. */
    void stop();
    bool stopped() const;

private:
    std::string lineLimitError() const;

    int readFd_;
    int writeFd_;
    size_t maxLineBytes_;

    std::atomic<bool> stopped_{false};

    /* Bytes read but not yet returned as a line. Only the reader thread touches it. */
    std::string buffer_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_TRANSPORT_HPP */
