#include "Handler-Forward.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Png.h"
#include "ToolArguments.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

void Handler::handleWaitSession(const ToolOrder& order,
                                ToolCompletionDelegate& completion) {
   const Result<std::int64_t, ProtocolError> timeout =
       readTimeoutMs(order.arguments, options_.timeoutMs, "wait_session");
   if (!timeout) {
      completion.complete(ToolResult::text(timeout.error().message, true));
      return;
   }
   const std::int64_t timeoutMs = *timeout;

   if (appReady_) {
      completion.complete(
          ToolResult::text("an application session is already attached"));
      return;
   }

   waiters_.push_back(PendingWait{
       &completion,
       Clock::now() + std::chrono::milliseconds(timeoutMs),
       timeoutMs,
   });
}

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

void Handler::completeWaiters(const std::string& text) {
   for (PendingWait& waiter : waiters_) {
      waiter.completion->complete(ToolResult::text(text));
   }
   waiters_.clear();
}

void Handler::expireWaiters(Clock::time_point now) {
   for (auto waiter = waiters_.begin(); waiter != waiters_.end();) {
      if (now < waiter->deadline) {
         ++waiter;
         continue;
      }
      waiter->completion->complete(ToolResult::text(
          "no application session connected within " +
              std::to_string(waiter->timeoutMs) + " ms",
          true));
      waiter = waiters_.erase(waiter);
   }
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

void Handler::detachApp(std::string reason) {
   if (appPeer_ == nullptr) {
      return;
   }
   std::unique_ptr<AppPeer> peer = std::move(appPeer_);
   appFd_ = -1;
   appReady_ = false;
   /* close() reports onAppDetached once, which fails the remaining forwards. */
   peer->close(std::move(reason));
}

bool Handler::peerAdvertises(std::string_view name) const {
   for (const nlohmann::json& entry : peerTools_) {
      if (entry.is_object() && entry.value("name", std::string()) == name) {
         return true;
      }
   }
   return false;
}

ToolResult Handler::presentFrame(const nlohmann::json& arguments,
                                 const nlohmann::json& header,
                                 std::vector<std::uint8_t> payload) const {
   const int width = header.value("width", 0);
   const int height = header.value("height", 0);

   std::vector<std::uint8_t> png;
   if (!encodePngToVector(payload.data(), width, height, png)) {
      return ToolResult::text("PNG encoding failed for " + std::to_string(width) +
                                  "x" + std::to_string(height),
                              true);
   }

   std::string text = header.value("text", std::string());
   if (text.empty()) {
      text = captureSummary(arguments, width, height, png.size(),
                            frameIndexOf(header));
   }

   ToolResult result;
   result.content = nlohmann::json::array({
       nlohmann::json{{"type", "text"}, {"text", std::move(text)}},
       nlohmann::json{{"type", "image"},
                      {"mimeType", "image/png"},
                      {"data", base64Encode(png)}},
   });
   return result;
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

int Handler::nextPollTimeoutMs(Clock::time_point now) const {
   std::optional<Clock::time_point> next;
   const auto consider = [&next](Clock::time_point deadline) {
      if (!next.has_value() || deadline < *next) {
         next = deadline;
      }
   };

   for (const PendingWait& waiter : waiters_) {
      consider(waiter.deadline);
   }
   for (const auto& entry : forwards_) {
      consider(entry.second.deadline);
   }
   if (pendingSpawn_.has_value()) {
      consider(pendingSpawn_->deadline);
   }
   if (webRun_) {
      if (const auto deadline = webRun_->nextDeadline()) {
         consider(*deadline);
      }
   }
   if (appPeer_ != nullptr && !appPeer_->isReady() && !appPeer_->isClosed()) {
      consider(appPeer_->handshakeDeadline());
   }

   /* While the tools are starting, the loop must probe their ports often. */
   const bool probingWeb = webRun_ && !webRun_->browserStarted();

   if (!next.has_value()) {
      return probingWeb ? kWebProbePollMs : -1;
   }
   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                              *next - now)
                              .count();
   if (remaining <= 0) {
      return 0;
   }
   const long long capped = std::min<long long>(remaining, 600000);
   if (probingWeb) {
      return static_cast<int>(std::min<long long>(capped, kWebProbePollMs));
   }
   return static_cast<int>(capped);
}

}  // namespace sdl3mcp
