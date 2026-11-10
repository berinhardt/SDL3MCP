/*
  The request dispatcher: one line in, one answer out, over a ByteSink.

  The server thread owns the thread, the fd transport and the failure state; it
  reads a line and hands it here. Everything between "a line arrived" and "the
  answer is written" lives in this type, so the same code answers a piped
  stdin/stdout and a browser WebSocket, and the raw-frame framing exists once.

  The session is passed by reference because it is per connection and its
  lifetime belongs to whoever created both; the handler is a value so the app can
  capture its queues. Neither may touch SDL from this thread: the handler is
  expected to marshal to the app thread and return.

  A write failure is a value, not an exception: the responder returns false with
  the message filled in, because the server decides what a broken client means.
  The message is built where the failing write runs, since errno is per thread.

  Notifications produce no response; parse and invalid-request errors are
  answered and the session continues.
*/

#ifndef SDL3MCP_RESPONDER_HPP
#define SDL3MCP_RESPONDER_HPP

#include "byte_sink.hpp"
#include "frame.hpp"
#include "json_rpc.hpp"
#include "session.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace sdl3mcp
{

/*
  What a tool handler returns. Most tools answer with MCP content blocks plus
  the error flag. A handler that fills `frame` returns raw pixels instead:
  sdl_capture writes one result line carrying the raw framing header followed
  by exactly the pixel bytes, and sdl_input adds `frameText` so the same line
  names the frame its batch was applied at. A frame is never a content array:
  it is one line plus B bytes.
*/
struct ToolResult
{
    nlohmann::json content;
    bool isError = false;
    /* The text block that accompanies a raw frame; empty when there is none. */
    std::string frameText;
    std::optional<Frame> frame;
};

using ToolHandler = std::function<ToolResult(const ToolCall &)>;

class Responder
{
public:
    /* The sink and the session outlive the responder. */
    Responder(ByteSink &sink, Session &session, ToolHandler handler);

    Responder(const Responder &) = delete;
    Responder &operator=(const Responder &) = delete;

    /*
      Parses one line, dispatches it, and writes the answer. False on a write
      failure, with `error` naming it; nothing is written in that case beyond
      whatever the failing write already put on the wire.
    */
    bool handleLine(std::string_view line, std::string &error);

private:
    bool writeResponse(const nlohmann::json &response, std::string &error);
    /* Runs the handler; the frame path writes the header line then the pixels. */
    bool writeToolCall(const ToolCall &call, std::string &error);

    ByteSink &sink_;
    Session &session_;
    ToolHandler handler_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_RESPONDER_HPP */
