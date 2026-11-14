/*
  AppPeer's protocol arm: the private handshake the contract requires, the
  answers it reads, and what the application sends once the session is up.

  A response is either a content result, a JSON-RPC error, or the library's raw
  framing: one header line followed by exactly byteLength bytes. The header is
  validated and its size bounded before the payload is read, so a malformed or
  hostile peer cannot make the Handler allocate an arbitrary buffer.

  The lifecycle, the writes and the detach stay in AppPeer.cpp; this unit only
  decides what a line means and hands it on.
*/

#include "AppPeer.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "AppPeer-Protocol.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

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

   /*
      Only the announced length is read here; the rest of the header is
      validated when the payload arrives, so a mismatched header becomes a tool
      error for this call instead of ending the application session. A rejected
      payload is skipped so the next line is still a line.
   */
   std::uint64_t byteLength = 0;
   if (!ToolResult::declaredFrameByteLength(*result, byteLength)) {
      handler_.onAppForwardFailure(
          id,
          "the application sent an invalid frame result: the result is neither "
          "content nor a raw frame header");
      return;
   }
   if (byteLength > kMaxFrameBytes) {
      handler_.onAppForwardFailure(
          id, "the application sent " + std::to_string(byteLength) +
                  " frame bytes, above the " + std::to_string(kMaxFrameBytes) +
                  " byte Handler cap");
      stream_->discardPayload(static_cast<std::size_t>(
          std::min<std::uint64_t>(byteLength,
                                  std::numeric_limits<std::size_t>::max())));
      return;
   }

   pendingFrameId_ = id;
   pendingFrameHeader_ = *result;
   stream_->expectPayload(static_cast<std::size_t>(byteLength));
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

}  // namespace sdl3mcp
