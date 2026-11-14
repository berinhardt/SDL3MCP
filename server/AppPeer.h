/*
  The Handler's MCP client for one application connection.

  It owns the accepted stream, performs the private handshake the contract
  requires (initialize, the initialized notification, tools/list), answers the
  application's list_changed by re-listing, and pairs forwarded tools/call
  requests with their responses. A response is either a content result, a
  JSON-RPC error, or the library's raw framing: one header line followed by
  exactly byteLength bytes. The header is validated and its size bounded before
  the payload is read, so a malformed or hostile peer cannot make the Handler
  allocate an arbitrary buffer.

  Every callback runs on the Handler's poll thread, from pump() or a
  Handler-initiated close. The class is single-threaded and holds no lock.
*/

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "Stream.h"

namespace sdl3mcp {

/* What the Handler hears from one application session. */
class AppPeerHandler {
  public:
   virtual ~AppPeerHandler() = default;

   virtual void onAppAttached(std::vector<nlohmann::json> tools) = 0;
   virtual void onAppToolsChanged(std::vector<nlohmann::json> tools) = 0;
   virtual void onAppDetached(std::string reason) = 0;
   /* A content result, relayed verbatim by the Handler. */
   virtual void onAppContent(std::uint64_t requestId, nlohmann::json result) = 0;
   /* A raw frame, already validated and read; the Handler presents it. */
   virtual void onAppFrame(std::uint64_t requestId,
                           nlohmann::json header,
                           std::vector<std::uint8_t> payload) = 0;
   /* A malformed frame or a result the Handler must answer as a tool error. */
   virtual void onAppForwardFailure(std::uint64_t requestId,
                                    std::string error) = 0;
   /* A JSON-RPC error object from the application, relayed unchanged. */
   virtual void onAppError(std::uint64_t requestId, nlohmann::json error) = 0;
};

class AppPeer : public StreamHandler {
  public:
   AppPeer(std::unique_ptr<Stream> stream,
           AppPeerHandler& handler,
           std::uint32_t handshakeTimeoutMs);
   ~AppPeer() override;

   AppPeer(const AppPeer&) = delete;
   AppPeer& operator=(const AppPeer&) = delete;

   /* Sends initialize and arms the handshake deadline. */
   bool start(std::string& error);
   /* Reads and delivers whatever the application sent. */
   void pump();
   /* Closes the stream and reports the detach once. */
   void close();

   bool isReady() const noexcept { return state_ == State::Ready; }
   bool isClosed() const noexcept { return state_ == State::Closed; }
   bool wantsWrite() const { return stream_->wantsWrite(); }

   std::chrono::steady_clock::time_point handshakeDeadline() const noexcept {
      return handshakeDeadline_;
   }

   std::uint64_t nextCallId() noexcept { return nextId_++; }
   bool sendCall(std::uint64_t id,
                 const std::string& name,
                 const nlohmann::json& arguments);

   /* StreamHandler */
   void onOpen() override;
   void onMessage(std::string_view line) override;
   void onPayload(std::vector<std::uint8_t> payload) override;
   void onError(std::string_view error) override;
   void onClose() override;

  private:
   enum class State {
      Handshaking,
      Ready,
      Closed
   };

   void requestToolsList();
   void handleResponse(const nlohmann::json& message);
   void handleInitializeResponse(const nlohmann::json& message);
   void handleToolsListResponse(const nlohmann::json& message);
   void handleForwardResponse(std::uint64_t id, const nlohmann::json& message);
   bool write(nlohmann::json message);
   void detach(std::string reason);

   std::unique_ptr<Stream> stream_;
   AppPeerHandler& handler_;
   State state_ = State::Handshaking;
   std::uint64_t nextId_ = 1;
   std::uint64_t initializeId_ = 0;
   std::uint64_t listId_ = 0;
   bool toolsListPending_ = false;
   bool refreshQueued_ = false;
   std::uint64_t pendingFrameId_ = 0;
   nlohmann::json pendingFrameHeader_;
   std::vector<nlohmann::json> tools_;
   std::chrono::steady_clock::time_point handshakeDeadline_;
   bool notifiedClose_ = false;
};

}  // namespace sdl3mcp
