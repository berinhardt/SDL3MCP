#include "Handler.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ToolArguments.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kNoAppError =
    "no application session is attached; call wait_session first";

/*
  A call's own timeout_ms is a budget for the application's answer, so the
  Handler gives it a little slack on top: the application is the authority on
  when it has nothing to send, and the Handler only bounds the wait.
*/
constexpr std::int64_t kForwardGraceMs = 500;

}  // namespace

void Handler::handleForward(const ToolOrder& order,
                            ToolCompletionDelegate& completion) {
   if (!appReady_ || appPeer_ == nullptr || appPeer_->isClosed()) {
      completion.complete(ToolResult::text(std::string(kNoAppError), true));
      return;
   }
   if (!peerAdvertises(order.name)) {
      completion.complete(
          ToolResult::text("the application does not provide " + order.name, true));
      return;
   }

   std::int64_t timeoutMs = options_.timeoutMs;
   nlohmann::json arguments = order.arguments;
   if (arguments.contains("timeout_ms")) {
      const Result<std::int64_t, ProtocolError> requested =
          readTimeoutMs(arguments, options_.timeoutMs, order.name);
      if (!requested) {
         completion.complete(
             ToolResult::text(requested.error().message, true));
         return;
      }
      timeoutMs = *requested + kForwardGraceMs;
      arguments.erase("timeout_ms");
   }

   const std::uint64_t id = appPeer_->nextCallId();
   if (!appPeer_->sendCall(id, order.name, arguments)) {
      completion.complete(ToolResult::text(
          "cannot send " + order.name + " to the application", true));
      return;
   }

   forwards_.emplace(
       id, PendingForward{order.name, std::move(arguments), &completion,
                          Clock::now() + std::chrono::milliseconds(timeoutMs)});
}

void Handler::expireForwards(Clock::time_point now) {
   std::vector<std::uint64_t> expired;
   for (const auto& entry : forwards_) {
      if (now >= entry.second.deadline) {
         expired.push_back(entry.first);
      }
   }
   if (expired.empty()) {
      return;
   }

   for (const std::uint64_t id : expired) {
      const auto entry = forwards_.find(id);
      if (entry == forwards_.end()) {
         continue;
      }
      entry->second.completion->complete(ToolResult::text(
          "the application did not answer " + entry->second.name +
              " within the proxy timeout",
          true));
      forwards_.erase(entry);
   }

   detachApp("the application stopped answering");
}

void Handler::failPendingForwards(const std::string& prefix,
                                  const std::string& suffix) {
   for (auto& entry : forwards_) {
      entry.second.completion->complete(
          ToolResult::text(prefix + entry.second.name + suffix, true));
   }
   forwards_.clear();
}

bool Handler::peerAdvertises(std::string_view name) const {
   for (const nlohmann::json& entry : peerTools_) {
      if (entry.is_object() && entry.value("name", std::string()) == name) {
         return true;
      }
   }
   return false;
}

void Handler::onAppContent(std::uint64_t requestId, nlohmann::json result) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }

   Result<ToolResult, ProtocolError> decoded =
       ToolResult::decode(result, {});
   if (!decoded) {
      entry->second.completion->complete(ToolResult::text(
          "the application returned an invalid result: " +
              decoded.error().message,
          true));
   } else {
      entry->second.completion->complete(std::move(*decoded));
   }
   forwards_.erase(entry);
}

void Handler::onAppFrame(std::uint64_t requestId,
                         nlohmann::json header,
                         std::vector<std::uint8_t> payload) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }

   Result<ToolResult, ProtocolError> decoded =
       ToolResult::decode(header, std::move(payload));
   if (!decoded) {
      entry->second.completion->complete(ToolResult::text(
          "the application sent an invalid frame result: " +
              decoded.error().message,
          true));
   } else {
      entry->second.completion->complete(presentFrame(
          entry->second.arguments, decoded->content,
          std::move(decoded->payload)));
   }
   forwards_.erase(entry);
}

void Handler::onAppForwardFailure(std::uint64_t requestId, std::string error) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }
   entry->second.completion->complete(ToolResult::text(std::move(error), true));
   forwards_.erase(entry);
}

void Handler::onAppError(std::uint64_t requestId, nlohmann::json error) {
   const auto entry = forwards_.find(requestId);
   if (entry == forwards_.end()) {
      return;
   }
   entry->second.completion->complete(ToolResult::error(std::move(error)));
   forwards_.erase(entry);
}

}  // namespace sdl3mcp
