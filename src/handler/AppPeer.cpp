/*
  AppPeer's lifecycle arm: the session object, the handshake's opening move, the
  pump, the writes every other unit goes through, and the detach that retires the
  session exactly once.

  What a line means once it arrives — initialize, tools/list, a forwarded
  result, a close — is AppPeer-Protocol.cpp.
*/

#include "AppPeer.h"

#include <SDL3MCP_version.h>

#include <string>
#include <utility>

#include "AppPeer-Protocol.h"

namespace sdl3mcp {

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

Result<void, TransportError> AppPeer::start() {
   initializeId_ = nextId_++;
   const Result<void, TransportError> sent = write(nlohmann::json{
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
   });
   if (!sent) {
      return TransportError{"cannot send initialize to the application"};
   }
   return {};
}

void AppPeer::pump() {
   if (state_ == State::Closed) {
      return;
   }
   stream_->pump();
}

void AppPeer::close(std::string reason) {
   detach(std::move(reason));
}

Result<void, TransportError> AppPeer::sendCall(
    std::uint64_t id, const std::string& name, const nlohmann::json& arguments) {
   if (state_ != State::Ready) {
      return TransportError{"the application session is not ready"};
   }
   return write(nlohmann::json{
       {"jsonrpc", "2.0"},
       {"id", id},
       {"method", "tools/call"},
       {"params", nlohmann::json{{"name", name}, {"arguments", arguments}}},
   });
}

Result<void, TransportError> AppPeer::write(nlohmann::json message) {
   if (state_ == State::Closed) {
      return TransportError{"the application session is closed"};
   }
   const std::string line =
       message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
   if (stream_->sendText(line)) {
      return {};
   }
   detach("cannot write to the application");
   return TransportError{"cannot write to the application"};
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
