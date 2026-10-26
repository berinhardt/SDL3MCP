/*
  The MCP session protocol: initialization state and dispatch, with no I/O and
  no tool execution.

  Session::handle() consumes one already-parsed message and produces at most one
  response or at most one tool call. That split is the point: the transport only
  parses, writes, and routes a ToolCall to a handler, so every protocol decision
  (what is legal before initialize, which methods exist, which errors apply) is
  ordinary testable code.

  The session is server-thread-only and holds no lock. It is created per client
  connection and dies with it; the initialized flag is the whole state.

  Notifications never produce a response. notifications/initialized is accepted
  and otherwise ignored: the initialize request has already made the session
  ready, and tool calls do not wait for the client's acknowledgement.
*/

#ifndef SDL3MCP_SESSION_HPP
#define SDL3MCP_SESSION_HPP

#include "json_rpc.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace sdl3mcp
{

/* A validated tools/call request handed to the tool executor. */
struct ToolCall
{
    nlohmann::json id;
    std::string name;
    nlohmann::json arguments;
};

/*
  At most one of the two members is set. Neither is set for a notification, a
  successful tools/call, or a request that must produce no output.
*/
struct SessionResult
{
    std::optional<nlohmann::json> response;
    std::optional<ToolCall> toolCall;
};

class Session
{
public:
    SessionResult handle(const RpcMessage &message);
    bool initialized() const noexcept { return initialized_; }

private:
    SessionResult handleRequest(const RpcRequest &request);
    SessionResult handleInitialize(const RpcRequest &request);
    SessionResult handleToolsList(const RpcRequest &request);
    SessionResult handleToolsCall(const RpcRequest &request);

    bool initialized_ = false;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SESSION_HPP */
