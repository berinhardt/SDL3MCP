#include "Handler.h"

#include <algorithm>
#include <chrono>
#include <optional>

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

/*
  While a web run is waiting for its page server and bridge, the loop probes
  the two ports on every turn; this bounds how long poll may sleep first.
*/
constexpr int kWebProbePollMs = 25;

}  // namespace

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
