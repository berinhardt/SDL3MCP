/*
  The session's transport and order side: construction, the stream callbacks,
  the pending-order bookkeeping and the line framing. The request handlers are
  in Session-Requests.cpp; both units define members of this one class.
*/

#include "Session.h"

#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp {

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
