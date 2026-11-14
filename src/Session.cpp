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

Session::Session(ToolCatalog& catalog, std::unique_ptr<Stream> stream)
    : catalog_(catalog), stream_(std::move(stream)) {
   stream_->setHandler(this);
}

Session::~Session() {
   stream_->setHandler(nullptr);
}

void Session::pump() {
   discardAnswered();
   stream_->pump();
}

void Session::close() {
   if (!open_) {
      return;
   }
   open_ = false;
   disconnect();
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
     Every available line is dispatched in the pump. A tool that defers keeps
     its own pending order instead of blocking the requests behind it, which is
     what lets several calls registered at one frame share the next frame.
   */
   handleLine(line);
}

void Session::onPayload(std::vector<uint8_t>) {
   /* A server-side session never announces a payload; the Handler's
      client-facing session will decode raw results through this callback. */
}

void Session::onError(std::string_view error) {
   lastError_ = std::string(error);
   open_ = false;
   disconnect();
}

void Session::onClose() {
   open_ = false;
   disconnect();
}

void Session::disconnect() {
   for (auto& entry : pending_) {
      entry.second.completion->cancel();
      retired_.push_back(std::move(entry.second.completion));
   }
   pending_.clear();
   pendingCount_ = 0;
}

void Session::discardAnswered() {
   for (auto entry = pending_.begin(); entry != pending_.end();) {
      if (entry->second.answered) {
         entry = pending_.erase(entry);
      } else {
         ++entry;
      }
   }
}

void Session::OrderCompletion::complete(ToolResult result) {
   if (cancelled_) {
      return;
   }
   /* A completion may erase this order; keep it alive for the whole call. */
   const std::shared_ptr<OrderCompletion> keepAlive = shared_from_this();
   session_.completeOrder(token_, std::move(result));
}

void Session::OrderCompletion::cancelled() {
   if (cancelled_) {
      return;
   }
   cancelled_ = true;
   const std::shared_ptr<OrderCompletion> keepAlive = shared_from_this();
   session_.cancelOrder(token_);
}

void Session::completeOrder(std::uint64_t token, ToolResult result) {
   const auto entry = pending_.find(token);
   if (!open_ || entry == pending_.end() || entry->second.answered) {
      return;
   }

   const nlohmann::json id = entry->second.order.id;
   entry->second.answered = true;
   --pendingCount_;
   if (!result.encode(*stream_, id)) {
      close();
   }
}

void Session::cancelOrder(std::uint64_t token) {
   const auto entry = pending_.find(token);
   if (entry == pending_.end()) {
      return;
   }
   if (!entry->second.answered) {
      --pendingCount_;
   }
   entry->second.answered = true;
   retired_.push_back(std::move(entry->second.completion));
   pending_.erase(entry);
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

}  // namespace sdl3mcp
