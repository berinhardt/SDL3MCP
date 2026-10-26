/*
  The JSON-RPC 2.0 message layer: one line in, one validated message or one
  error object out. It knows the wire grammar only, never method names,
  initialization state, tools, or I/O. That keeps the stdio loop later a plain
  read/parse/dispatch/write cycle and leaves every protocol decision in one
  testable place.

  MCP forbids batches, so a top-level array is an invalid request rather than a
  list of requests. A request without an "id" member is a notification. An "id"
  that is present must be a string or a number: null, booleans, arrays, and
  objects are rejected, because echoing them back would violate JSON-RPC and
  break clients that match responses by id.

  Error ids follow the same rule from the other side: when the id itself is
  valid it is echoed even if another field is wrong, so the client can correlate
  the failure. Otherwise the response carries a null id.

  parseMessage() writes errorResponse only on failure and clears it on success,
  so a caller that reuses one variable cannot emit a stale error.
*/

#ifndef SDL3MCP_JSON_RPC_HPP
#define SDL3MCP_JSON_RPC_HPP

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace sdl3mcp
{

enum class RpcErrorCode : int
{
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603
};

struct RpcRequest
{
    nlohmann::json id;
    std::string method;
    nlohmann::json params;
};

struct RpcNotification
{
    std::string method;
    nlohmann::json params;
};

struct RpcMessage
{
    enum class Kind
    {
        Request,
        Notification
    };

    Kind kind = Kind::Notification;
    RpcRequest request;
    RpcNotification notification;
};

/*
  Parses one transport line. Returns true and fills message when the line is a
  well-formed request or notification; returns false, leaves message untouched,
  and fills errorResponse with the matching error object otherwise.
*/
bool parseMessage(std::string_view line,
                  RpcMessage &message,
                  nlohmann::json &errorResponse);

nlohmann::json makeResult(const nlohmann::json &id, nlohmann::json result);
nlohmann::json makeError(const nlohmann::json &id,
                         RpcErrorCode code,
                         std::string_view message);

} // namespace sdl3mcp

#endif /* SDL3MCP_JSON_RPC_HPP */
