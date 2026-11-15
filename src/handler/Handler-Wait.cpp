#include "Handler.h"

#include <chrono>
#include <cstdint>
#include <string>

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

}  // namespace sdl3mcp
