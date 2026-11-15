#include "Handler.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <utility>

#include "ToolResult.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

void Handler::reapChildren() {
   for (;;) {
      int status = 0;
      const pid_t pid = ::waitpid(-1, &status, WNOHANG);
      if (pid <= 0) {
         return;
      }
      if (webRun_) {
         webRun_->recordExit(pid, status);
      }
   }
}

void Handler::expireSpawn(Clock::time_point now) {
   if (!pendingSpawn_.has_value() || now < pendingSpawn_->deadline) {
      return;
   }

   std::string text =
       "app_spawn_" + pendingSpawn_->alias + " timed out after " +
       std::to_string(pendingSpawn_->timeoutMs) +
       " ms waiting for the application to connect";
   if (pendingSpawn_->isWeb()) {
      /* A timed-out web spawn stops its stack; the browser is spawn-and-forget. */
      text += "; the web stack was stopped";
      if (pendingSpawn_->pid > 0) {
         text += "; browser pid " + std::to_string(pendingSpawn_->pid) +
                 " may still be running";
      }
      stopWebRun();
   } else {
      text += "; process pid " + std::to_string(pendingSpawn_->pid) +
              " may still be running";
   }
   completeSpawn(text, true);
}

void Handler::completeSpawn(std::string text, bool isError) {
   if (!pendingSpawn_.has_value()) {
      return;
   }

   PendingSpawn pending = std::move(*pendingSpawn_);
   pendingSpawn_.reset();
   if (pending.errorFd >= 0) {
      ::close(pending.errorFd);
   }
   pending.completion->complete(ToolResult::text(std::move(text), isError));
}

void Handler::cancelSpawn() {
   if (!pendingSpawn_.has_value()) {
      return;
   }
   if (pendingSpawn_->isWeb()) {
      stopWebRun();
   }
   if (pendingSpawn_->errorFd >= 0) {
      ::close(pendingSpawn_->errorFd);
   }
   pendingSpawn_.reset();
}

}  // namespace sdl3mcp
