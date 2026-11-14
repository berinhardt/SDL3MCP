#include "AppPeer.h"

#include <SDL3MCP/SDL3MCP_version.h>

#include <utility>

#include "ToolResult.h"

namespace sdl3mcp {
namespace {

constexpr std::string_view kProtocolVersion = "2025-11-25";
constexpr std::string_view kServerName = "sdl3mcp";

/*
  The largest raw frame the Handler accepts from an application. It bounds the
  allocation a hostile or broken peer can request before one payload byte is
  read; a larger byteLength is rejected without allocating it.
*/
constexpr std::uint64_t kMaxFrameBytes = 64ull * 1024 * 1024;

std::string describeRpcError(const nlohmann::json& error) {
   if (error.is_object()) {
      const auto message = error.find("message");
      if (message != error.end() && message->is_string()) {
         return message->get<std::string>();
      }
   }
   return error.dump();
}

}  // namespace

AppPeer::AppPeer(std::unique_ptr<Stream> stream,
                 AppPeerHandler& handler,
                 std::uint32_t handshakeTimeoutMs)
    : stream_(std::move(stream)), handler_(handler) {
   stream_->setHandler(this);
   handshakeDeadline_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(handshakeTimeoutMs);
}

AppPeer::~AppPeer() {
   stream_->setHandler(nullptr);
   stream_->close();
}

bool AppPeer::start(std::string& error) {
   error.clear();
   initializeId_ = nextId_++;
   if (!write(nlohmann::json{
           {"jsonrpc", "2.0"},
           {"id", initializeId_},
           {"method", "initialize"},
           {"params", nlohmann::json{
                          {"protocolVersion", std::string(kProtocolVersion)},
                          {"capabilities", nlohmann::json::object()},
                          {"clientInfo", nlohmann::json{
                                             {"name", std::string(kServerName)},
                                             {"version", std::string(SDL3MCP_VERSION_STRING)},
                                         }},
                      }},
       })) {
      error = "cannot send initialize to the application";
      return false;
   }
   return true;
}

void AppPeer::pump() {
   if (state_ == State::Closed) {
      return;
   }
   stream_->pump();
}

void AppPeer::close() {
   detach("the Handler closed the application session");
}

bool AppPeer::sendCall(std::uint64_t id,
                       const std::string& name,
                       const nlohmann::json& arguments) {
   if (state_ != State::Ready) {
      return false;
   }
   return write(nlohmann::json{
       {"jsonrpc", "2.0"},
       {"id", id},
       {"method", "tools/call"},
       {"params", nlohmann::json{{"name", name}, {"arguments", arguments}}},
   });
}

void AppPeer::requestToolsList() {
   if (state_ == State::Closed) {
      return;
   }
   if (toolsListPending_) {
      refreshQueued_ = true;
      return;
   }

   toolsListPending_ = true;
   listId_ = nextId_++;
   if (!write(nlohmann::json{
           {"jsonrpc", "2.0"},
           {"id", listId_},
           {"method", "tools/list"},
           {"params", nlohmann::json::object()},
       })) {
      return;
   }
}

void AppPeer::handleResponse(const nlohmann::json& message) {
   const auto idEntry = message.find("id");
   if (idEntry == message.end() || !idEntry->is_number_unsigned()) {
      return;
   }
   const std::uint64_t id = idEntry->get<std::uint64_t>();

   if (id == initializeId_) {
      handleInitializeResponse(message);
      return;
   }
   if (toolsListPending_ && id == listId_) {
      handleToolsListResponse(message);
      return;
   }
   handleForwardResponse(id, message);
}

void AppPeer::handleInitializeResponse(const nlohmann::json& message) {
   const auto error = message.find("error");
   if (error != message.end()) {
      detach("cannot initialize the application session: " + describeRpcError(*error));
      return;
   }

   const auto result = message.find("result");
   if (result == message.end() || !result->is_object()) {
      detach("the application answered initialize without a result");
      return;
   }

   /* The application is now a server; a real MCP client acknowledges. */
   if (!write(nlohmann::json{{"jsonrpc", "2.0"},
                             {"method", "notifications/initialized"}})) {
      return;
   }
   requestToolsList();
}

void AppPeer::handleToolsListResponse(const nlohmann::json& message) {
   toolsListPending_ = false;

   const auto error = message.find("error");
   if (error != message.end()) {
      detach("cannot list the application's tools: " + describeRpcError(*error));
      return;
   }

   const auto result = message.find("result");
   if (result == message.end() || !result->is_object()) {
      detach("the application answered tools/list without a result");
      return;
   }
   const auto entries = result->find("tools");
   if (entries == result->end() || !entries->is_array()) {
      detach("the application answered tools/list without a tools array");
      return;
   }

   tools_.clear();
   for (const nlohmann::json& entry : *entries) {
      if (entry.is_object() && entry.contains("name") &&
          entry["name"].is_string() && !entry["name"].get<std::string>().empty()) {
         tools_.push_back(entry);
      }
   }

   if (state_ == State::Handshaking) {
      state_ = State::Ready;
      handler_.onAppAttached(tools_);
   } else {
      handler_.onAppToolsChanged(tools_);
   }

   if (refreshQueued_) {
      refreshQueued_ = false;
      requestToolsList();
   }
}

void AppPeer::handleForwardResponse(std::uint64_t id,
                                    const nlohmann::json& message) {
   const auto error = message.find("error");
   if (error != message.end()) {
      handler_.onAppError(id, *error);
      return;
   }

   const auto result = message.find("result");
   if (result == message.end() || !result->is_object()) {
      handler_.onAppForwardFailure(
          id, "the application returned an invalid tool result");
      return;
   }

   /*
     A downstream Handler presents a frame itself, so a content result is
     already in its final shape and is relayed verbatim.
   */
   if (result->contains("content")) {
      handler_.onAppContent(id, *result);
      return;
   }

   std::size_t byteLength = 0;
   std::string headerError;
   if (!ToolResult::decodeHeader(*result, byteLength, headerError)) {
      handler_.onAppForwardFailure(
          id, "the application sent an invalid frame result: " + headerError);
      detach("the application sent a malformed frame result");
      return;
   }
   if (byteLength > kMaxFrameBytes) {
      handler_.onAppForwardFailure(
          id, "the application sent " + std::to_string(byteLength) +
                  " frame bytes, above the " + std::to_string(kMaxFrameBytes) +
                  " byte Handler cap");
      detach("the application sent a frame above the Handler cap");
      return;
   }

   pendingFrameId_ = id;
   pendingFrameHeader_ = *result;
   stream_->expectPayload(byteLength);
}

void AppPeer::onOpen() {
}

void AppPeer::onMessage(std::string_view line) {
   if (state_ == State::Closed) {
      return;
   }

   /*
     A response has no method, so it cannot go through parseMessage, which is
     the server-side request grammar. The application only ever sends responses
     and the list_changed notification on this connection.
   */
   const nlohmann::json message =
       nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
   if (message.is_discarded() || !message.is_object()) {
      return;
   }

   if (message.contains("method")) {
      const auto method = message.find("method");
      if (method->is_string() &&
          method->get<std::string>() == "notifications/tools/list_changed") {
         requestToolsList();
      }
      return;
   }

   handleResponse(message);
}

void AppPeer::onPayload(std::vector<std::uint8_t> payload) {
   if (pendingFrameId_ == 0) {
      return;
   }

   const std::uint64_t id = pendingFrameId_;
   nlohmann::json header = std::move(pendingFrameHeader_);
   pendingFrameId_ = 0;
   pendingFrameHeader_ = nlohmann::json();
   handler_.onAppFrame(id, std::move(header), std::move(payload));
}

void AppPeer::onError(std::string_view error) {
   detach(error.empty() ? "the application connection failed" : std::string(error));
}

void AppPeer::onClose() {
   detach("the application closed its connection");
}

bool AppPeer::write(nlohmann::json message) {
   if (state_ == State::Closed) {
      return false;
   }
   const std::string line =
       message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
   if (stream_->sendText(line)) {
      return true;
   }
   detach("cannot write to the application");
   return false;
}

void AppPeer::detach(std::string reason) {
   if (state_ == State::Closed) {
      return;
   }
   state_ = State::Closed;
   pendingFrameId_ = 0;
   pendingFrameHeader_ = nlohmann::json();
   stream_->close();
   if (!notifiedClose_) {
      notifiedClose_ = true;
      handler_.onAppDetached(std::move(reason));
   }
}

}  // namespace sdl3mcp
