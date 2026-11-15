#include "Web-Run.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

WebRun::~WebRun() {
   stop();
}

Result<void, TransportError> WebRun::start(WebLaunchOptions options) {
   options_ = std::move(options);
   stopped_ = false;

   const Result<std::vector<std::vector<std::string>>, TransportError> plan =
       makeWebLaunchPlan(options_);
   if (!plan) {
      stop();
      return plan.error();
   }
   const Result<void, TransportError> page = startChild(pageServer_, (*plan)[0]);
   if (!page) {
      stop();
      return page.error();
   }
   const Result<void, TransportError> bridge = startChild(bridge_, (*plan)[1]);
   if (!bridge) {
      stop();
      return bridge.error();
   }

   readyDeadline_ = Clock::now() + std::chrono::milliseconds(kWebReadyWindowMs);
   return {};
}

void WebRun::closeChildFds() {
   for (Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (child->pidfd >= 0) {
         ::close(child->pidfd);
         child->pidfd = -1;
      }
   }
}

void WebRun::stop(WebStopMode mode) {
   if (stopped_) {
      return;
   }
   stopped_ = true;

   /*
     Every failure and shutdown takes the whole run down; only a browser that
     exited itself leaves nothing of its group to signal.
   */
   std::vector<Child*> children{&pageServer_, &bridge_};
   if (mode == WebStopMode::AllProcesses) {
      children.push_back(&browser_);
   }

   const auto signalGroup = [](pid_t pid, int signal) {
      if (pid <= 0) {
         return;
      }
      if (::kill(-pid, signal) != 0) {
         (void)::kill(pid, signal);
      }
   };

   /*
     SIGTERM reaches the group, not just the leader: websocat may have forked a
     connection handler, and Firefox forks its own content processes.
   */
   for (Child* child : children) {
      if (child->started) {
         signalGroup(child->pid, SIGTERM);
      }
   }

   /* One grace period for every child, then SIGKILL for whatever remains. */
   const auto waitForChildren = [this, &children](Clock::time_point deadline) {
      for (;;) {
         std::vector<pollfd> descriptors;
         for (Child* child : children) {
            if (child->started && !child->exited && child->pidfd >= 0) {
               descriptors.push_back(pollfd{child->pidfd, POLLIN, 0});
            }
         }
         if (descriptors.empty()) {
            return;
         }

         const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - Clock::now())
                                    .count();
         if (remaining <= 0) {
            return;
         }
         const int ready = ::poll(descriptors.data(), descriptors.size(),
                                  static_cast<int>(remaining));
         if (ready < 0) {
            if (errno == EINTR) {
               continue;
            }
            return;
         }
         if (ready == 0) {
            return;
         }
         for (Child* child : children) {
            if (!child->started || child->exited || child->pidfd < 0) {
               continue;
            }
            for (const pollfd& descriptor : descriptors) {
               if (descriptor.fd == child->pidfd && descriptor.revents != 0) {
                  int status = 0;
                  const pid_t reaped = ::waitpid(child->pid, &status, WNOHANG);
                  markExited(*child, reaped == child->pid ? status : 0);
                  break;
               }
            }
         }
      }
   };

   waitForChildren(Clock::now() + std::chrono::milliseconds(kWebStopGraceMs));

   for (Child* child : children) {
      if (child->started) {
         /* The group, again: a leader that exited may have left a descendant. */
         signalGroup(child->pid, SIGKILL);
      }
   }
   waitForChildren(Clock::now() + std::chrono::milliseconds(500));

   closeChildFds();

   if (!options_.profileDir.empty()) {
      const Result<void, TransportError> removed =
          removeProfileTree(options_.profileDir);
      if (!removed) {
         std::fprintf(stderr, "sdl3mcp: web: %s\n",
                      removed.error().message.c_str());
      }
      options_.profileDir.clear();
   }
   if (options_.profileLockFd >= 0) {
      ::close(options_.profileLockFd);
      options_.profileLockFd = -1;
   }

   browserPid_ = 0;
}

}  // namespace sdl3mcp
