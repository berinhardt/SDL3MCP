#include "session.hpp"

#include "tools.hpp"

#include <SDL3MCP/SDL3MCP_version.h>

#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

/*
  The revision this server speaks. A client asking for another version is
  answered with this one; MCP leaves the client free to disconnect when it
  cannot use the reply.
*/
constexpr std::string_view kProtocolVersion = "2025-11-25";

constexpr std::string_view kServerName = "sdl3mcp";

SessionResult respond(nlohmann::json response)
{
    SessionResult result;
    result.response = std::move(response);
    return result;
}

SessionResult fail(const nlohmann::json &id,
                   RpcErrorCode code,
                   std::string_view message)
{
    return respond(makeError(id, code, message));
}

SessionResult notInitialized(const nlohmann::json &id)
{
    return fail(id, RpcErrorCode::InvalidRequest,
                "tools are unavailable before initialize");
}

nlohmann::json toolList()
{
    nlohmann::json tools = nlohmann::json::array();
    for (const ToolDefinition &tool : toolCatalog()) {
        tools.push_back({
            {"name", std::string(tool.name)},
            {"description", std::string(tool.description)},
            {"inputSchema", tool.inputSchema},
            {"annotations", tool.annotations},
        });
    }
    return nlohmann::json{{"tools", std::move(tools)}};
}

} // namespace

SessionResult Session::handle(const RpcMessage &message)
{
    /* A notification is fire-and-forget: no response, and no state change. */
    if (message.kind == RpcMessage::Kind::Notification) {
        return SessionResult{};
    }

    return handleRequest(message.request);
}

SessionResult Session::handleRequest(const RpcRequest &request)
{
    if (request.method == "initialize") {
        return handleInitialize(request);
    }
    if (request.method == "ping") {
        return respond(makeResult(request.id, nlohmann::json::object()));
    }
    if (request.method == "tools/list") {
        return handleToolsList(request);
    }
    if (request.method == "tools/call") {
        return handleToolsCall(request);
    }

    return fail(request.id, RpcErrorCode::MethodNotFound,
                "Method not found: " + request.method);
}

SessionResult Session::handleInitialize(const RpcRequest &request)
{
    if (initialized_) {
        return fail(request.id, RpcErrorCode::InvalidRequest,
                    "initialize was already called");
    }

    const auto version = request.params.find("protocolVersion");
    if (version == request.params.end() || !version->is_string()) {
        return fail(request.id, RpcErrorCode::InvalidParams,
                    "initialize requires a string protocolVersion");
    }

    initialized_ = true;

    return respond(makeResult(
        request.id,
        nlohmann::json{
            {"protocolVersion", std::string(kProtocolVersion)},
            {"capabilities", nlohmann::json{{"tools", nlohmann::json::object()}}},
            {"serverInfo", nlohmann::json{
                 {"name", std::string(kServerName)},
                 {"version", std::string(SDL3MCP_VERSION_STRING)},
             }},
        }));
}

SessionResult Session::handleToolsList(const RpcRequest &request)
{
    if (!initialized_) {
        return notInitialized(request.id);
    }

    return respond(makeResult(request.id, toolList()));
}

SessionResult Session::handleToolsCall(const RpcRequest &request)
{
    if (!initialized_) {
        return notInitialized(request.id);
    }

    const auto name = request.params.find("name");
    if (name == request.params.end() || !name->is_string()) {
        return fail(request.id, RpcErrorCode::InvalidParams,
                    "tools/call requires a string tool name");
    }

    const std::string toolName = name->get<std::string>();
    if (findTool(toolName) == nullptr) {
        return fail(request.id, RpcErrorCode::InvalidParams,
                    "Unknown tool: " + toolName);
    }

    nlohmann::json arguments = nlohmann::json::object();
    const auto supplied = request.params.find("arguments");
    if (supplied != request.params.end()) {
        if (!supplied->is_object()) {
            return fail(request.id, RpcErrorCode::InvalidParams,
                        "tools/call arguments must be a JSON object");
        }
        arguments = *supplied;
    }

    SessionResult result;
    result.toolCall = ToolCall{request.id, toolName, std::move(arguments)};
    return result;
}

} // namespace sdl3mcp
