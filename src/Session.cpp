#include "Session.h"

#include <SDL3MCP/SDL3MCP_version.h>

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

Session::Session(ToolCatalog& catalog, std::unique_ptr<Stream> stream)
    : catalog_(catalog), stream_(std::move(stream)) {
   stream_->setHandler(this);
}

Session::~Session() {
   stream_->setHandler(nullptr);
}

void Session::pump() {
   if (open_) {
      stream_->pump();
   }
}

void Session::close() {
   if (!open_) {
      return;
   }
   open_ = false;
   pending_.reset();
   queued_.clear();
   stream_->close();
}

void Session::onOpen() {
   open_ = true;
}

void Session::onMessage(std::string_view line) {
   if (!open_) {
      return;
   }
   /*
     One tool call at a time: a line that arrives while an order is pending
     waits until that order answers, which is what serializes a deferred
     capture behind the requests before it.
   */
   if (pending_.has_value() || !queued_.empty()) {
      queued_.emplace_back(line);
      return;
   }
   handleLine(line);
}

void Session::onPayload(std::vector<uint8_t>) {
   /* A server-side session never announces a payload; the Handler's
      client-facing session will decode raw results through this callback. */
}

void Session::onError(std::string_view error) {
   lastError_ = std::string(error);
   open_ = false;
   pending_.reset();
   queued_.clear();
}

void Session::onClose() {
   open_ = false;
   pending_.reset();
   queued_.clear();
}

void Session::complete(ToolResult result) {
   if (!open_ || !pending_.has_value()) {
      return;
   }

   const nlohmann::json id = pending_->id;
   pending_.reset();
   if (!result.encode(*stream_, id)) {
      close();
      return;
   }
   drainQueue();
}

void Session::cancelled() {
   pending_.reset();
   drainQueue();
}

void Session::handleLine(std::string_view line) {
   RpcMessage message;
   nlohmann::json errorResponse;
   if (!parseMessage(line, message, errorResponse)) {
      /* Malformed input is answered, not fatal: the session continues. */
      writeResponse(errorResponse);
      return;
   }
   /* A notification is fire-and-forget: no response and no state change. */
   if (message.kind == RpcMessage::Kind::Notification) {
      return;
   }
   handleRequest(message.request);
}

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

   pending_ = ToolOrder{request.id, toolName, std::move(arguments)};
   tool->invoke(*pending_, *this);
}

bool Session::writeResponse(const nlohmann::json& response) {
   if (!open_) {
      return false;
   }
   if (stream_->sendText(response.dump())) {
      return true;
   }
   close();
   return false;
}

void Session::fail(const nlohmann::json& id,
                   RpcErrorCode code,
                   std::string_view message) {
   writeResponse(makeError(id, code, message));
}

void Session::drainQueue() {
   while (open_ && !pending_.has_value() && !queued_.empty()) {
      std::string line = std::move(queued_.front());
      queued_.pop_front();
      handleLine(line);
   }
}

}  // namespace sdl3mcp
