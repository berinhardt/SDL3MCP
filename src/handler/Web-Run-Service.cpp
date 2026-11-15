#include "Web-Run.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <vector>

namespace sdl3mcp {
namespace {

/* The shell-style code a wait status means: exit status, or 128 + signal. */
int exitCode(int status) {
   if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
   }
   if (WIFSIGNALED(status)) {
      return 128 + WTERMSIG(status);
   }
   return status;
}

using Clock = std::chrono::steady_clock;

}  // namespace

void WebRun::markExited(Child& child, int status) {
   child.exited = true;
   child.exitStatus = exitCode(status);
   if (child.pidfd >= 0) {
      ::close(child.pidfd);
      child.pidfd = -1;
   }
}

void WebRun::recordExit(pid_t pid, int status) {
   for (Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (child->started && !child->exited && child->pid == pid) {
         markExited(*child, status);
         return;
      }
   }
}

void WebRun::appendPollFds(std::vector<pollfd>& descriptors) const {
   for (const Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (child->started && !child->exited && child->pidfd >= 0) {
         descriptors.push_back(pollfd{child->pidfd, POLLIN, 0});
      }
   }
}

WebStackEvent WebRun::service(Clock::time_point now,
                              const pollfd* webFds,
                              std::size_t webFdCount) {
   WebStackEvent event;
   if (stopped_) {
      return event;
   }

   for (Child* child : {&pageServer_, &bridge_, &browser_}) {
      if (!child->started || child->exited || child->pidfd < 0) {
         continue;
      }
      for (std::size_t index = 0; index < webFdCount; ++index) {
         if (webFds[index].fd == child->pidfd &&
             webFds[index].revents != 0) {
            /*
               The Handler's SIGCHLD reaper may have collected the status
               already; when it has not, waitpid() here takes it. Either way the
               pidfd is closed and the child is marked exited once.
            */
            int status = 0;
            const pid_t reaped = ::waitpid(child->pid, &status, WNOHANG);
            if (reaped == child->pid) {
               markExited(*child, status);
            } else if (reaped < 0 && errno == ECHILD) {
               markExited(*child, 0);
            }
            break;
         }
      }
   }

   /*
     The page server and the bridge are tools: their death is never the
     intended end and is reported. The browser is the application, so its exit
     is the normal end of a run.
   */
   if (pageServer_.exited) {
      event.kind = WebStackEvent::Kind::ToolExited;
      event.child = WebChild::PageServer;
      return event;
   }
   if (bridge_.exited) {
      event.kind = WebStackEvent::Kind::ToolExited;
      event.child = WebChild::Bridge;
      return event;
   }
   if (browser_.exited) {
      event.kind = WebStackEvent::Kind::BrowserExited;
      event.child = WebChild::Browser;
      event.exitStatus = browser_.exitStatus;
      return event;
   }

   if (!browserStarted()) {
      const bool ready = bothPortsAccept();
      if (!ready && now < readyDeadline_) {
         return event;
      }
      if (!ready) {
         std::fprintf(stderr,
                      "sdl3mcp: web: the page server and bridge are not accepting "
                      "connections yet; starting the browser anyway\n");
      }

      const Result<void, TransportError> started = startBrowser();
      if (!started) {
         event.kind = WebStackEvent::Kind::ToolExited;
         event.child = WebChild::Browser;
         event.detail = "the browser could not be started: " +
                        started.error().message;
      }
   }

   return event;
}

std::optional<Clock::time_point> WebRun::nextDeadline() const {
   if (stopped_ || browserStarted()) {
      return std::nullopt;
   }
   return readyDeadline_;
}

}  // namespace sdl3mcp
