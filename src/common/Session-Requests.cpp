/*
  The session's request side: dispatch, initialize, ping, tools/list and
  tools/call. The transport lifecycle, the line framing and the pending order
  bookkeeping are in Session.cpp; both units are members of the same Session, so
  a handler answers through the private writeResponse/fail the other half owns.
*/

#include "Session.h"

#include <SDL3MCP_version.h>

#include <string>
#include <string_view>
#include <utility>

#include "ToolCatalog.h"

namespace sdl3mcp {
namespace {

/*
  The revision this server speaks. A client asking for another version is
  answered with this one; MCP leaves the client free to disconnect when it
  cannot use the reply.
*/
constexpr std::string_view kProtocolVersion = "2025-11-25";

constexpr std::string_view kServerName = "sdl3mcp";

}  // namespace

void Session::handleRequest(const RpcRequest& request) {
   if (request.method == "initialize") {
      handleInitialize(request);
      return;
   }
   if (request.method == "ping") {
      writeResponse(makeResult(request.id, nlohmann::json::object()));
      return;
   }
   if (request.method == "tools/list") {
      handleToolsList(request);
      return;
   }
   if (request.method == "tools/call") {
      handleToolsCall(request);
      return;
   }

   fail(request.id, RpcErrorCode::MethodNotFound,
        "Method not found: " + request.method);
}

void Session::handleInitialize(const RpcRequest& request) {
   if (initialized_) {
      fail(request.id, RpcErrorCode::InvalidRequest,
           "initialize was already called");
      return;
   }

   const auto version = request.params.find("protocolVersion");
   if (version == request.params.end() || !version->is_string()) {
      fail(request.id, RpcErrorCode::InvalidParams,
           "initialize requires a string protocolVersion");
      return;
   }

   initialized_ = true;
   writeResponse(makeResult(
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

void Session::handleToolsList(const RpcRequest& request) {
   if (!initialized_) {
      fail(request.id, RpcErrorCode::InvalidRequest,
           "tools are unavailable before initialize");
      return;
   }

   nlohmann::json tools = nlohmann::json::array();
   for (const auto& entry : catalog_.tools()) {
      const Tool& tool = entry.second;
      tools.push_back({
          {"name", tool.name},
          {"description", tool.description},
          {"inputSchema", tool.inputSchema},
          {"annotations", tool.annotations},
      });
   }

   writeResponse(makeResult(request.id, nlohmann::json{{"tools", std::move(tools)}}));
}

void Session::handleToolsCall(const RpcRequest& request) {
   if (!initialized_) {
      fail(request.id, RpcErrorCode::InvalidRequest,
           "tools are unavailable before initialize");
      return;
   }

   const auto name = request.params.find("name");
   if (name == request.params.end() || !name->is_string()) {
      fail(request.id, RpcErrorCode::InvalidParams,
           "tools/call requires a string tool name");
      return;
   }

   const std::string toolName = name->get<std::string>();
   const Tool* tool = catalog_.find(toolName);
   if (tool == nullptr) {
      fail(request.id, RpcErrorCode::InvalidParams, "Unknown tool: " + toolName);
      return;
   }

   nlohmann::json arguments = nlohmann::json::object();
   const auto supplied = request.params.find("arguments");
   if (supplied != request.params.end()) {
      if (!supplied->is_object()) {
         fail(request.id, RpcErrorCode::InvalidParams,
              "tools/call arguments must be a JSON object");
         return;
      }
      arguments = *supplied;
   }

   const std::uint64_t token = nextToken_++;
   auto completion = std::make_shared<OrderCompletion>(*this, token);
   const auto inserted = pending_.emplace(
       token, PendingOrder{ToolOrder{request.id, toolName, std::move(arguments)},
                           completion, false});
   ++pendingCount_;

   /* The local keeps the completion alive if the tool answers inline. */
   const std::shared_ptr<OrderCompletion> keepAlive = completion;
   tool->invoke(inserted.first->second.order, *keepAlive);
}

}  // namespace sdl3mcp
