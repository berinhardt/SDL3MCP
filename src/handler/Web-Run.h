/*
  WebRun: the three processes a prepared web launch owns, one lifetime, driven
  by the Handler's poll loop.

  The run starts the page server and bridge, waits for both loopback ports to
  accept before the browser, and reports a child's exit through its pidfd. The
  page server and the bridge are Handler-owned infrastructure and are stopped
  together with the profile; the browser is the spawned application and is
  spawn-and-forget. The browser's exit status is read with waitpid() when the
  pidfd fires, so a non-zero exit can be reported.

  This unit's definitions live in two translation units: Web-Run.cpp holds the
  class's lifetime, poll and stop, and Web-Child.cpp holds the child launch —
  fork, exec, pidfd watching and the loopback readiness probe.
*/

#pragma once

#include <poll.h>
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Web-Plan.h"

namespace sdl3mcp {

/* How long the page server and bridge get to accept before the browser. */
constexpr int kWebReadyWindowMs = 1000;
/* How long a stopped web-stack child gets before SIGKILL. */
constexpr int kWebStopGraceMs = 1000;

/* The three processes a web run owns, in plan order. */
enum class WebChild { PageServer,
                      Bridge,
                      Browser };

struct WebStackEvent {
   enum class Kind {
      None,
      /* The page server or the bridge exited; the stack must be stopped. */
      ToolExited,
      /* The browser exited; the stack must be stopped and the profile removed. */
      BrowserExited,
   };

   Kind kind = Kind::None;
   WebChild child = WebChild::PageServer;
   /* Why the stack ended when the cause was not a child's own exit. */
   std::string detail;
   /* The browser's wait status, when it was the child that exited. */
   int exitStatus = 0;
};

/* Which processes a stop takes down. */
enum class WebStopMode {
   /* The browser exited itself; stop the page server and bridge. */
   ToolsOnly,
   /* A failure, a spawn timeout or shutdown: stop all three. */
   AllProcesses,
};

/*
   Three processes, one lifetime, driven by the Handler's poll loop.

   start() takes ownership of the prepared options (including the profile lock)
   and starts the page server and bridge. service() then waits for both ports to
   accept and starts the browser, or starts it when the ready window expires. A
   child's exit is observed through its pidfd, which the Handler includes in its
   poll set via appendPollFds().

   stop() is SIGTERM to the process groups it covers, one grace period, SIGKILL
   for whatever is still alive, and removal of the profile. The browser is
   stopped with the tools on every failure path and on shutdown, because the
   three processes are one Handler-owned run; only the normal end — Firefox
   exits itself — uses ToolsOnly, which leaves the already-gone browser alone.
   stop() is idempotent.
*/
class WebRun {
  public:
   WebRun() = default;
   ~WebRun();

   WebRun(const WebRun&) = delete;
   WebRun& operator=(const WebRun&) = delete;

   Result<void, TransportError> start(WebLaunchOptions options);

   /* Appends one pollfd per live child, in page/bridge/browser order. */
   void appendPollFds(std::vector<pollfd>& descriptors) const;

   /*
      Records an exit the Handler's SIGCHLD reaper already collected, so service()
      can report the status even though waitpid() is gone.
   */
   void recordExit(pid_t pid, int status);

   /*
      Starts the browser when the tools are ready, then reads the pidfd revents
      in `webFds` and reports the first exit. `now` bounds the ready window.
   */
   WebStackEvent service(std::chrono::steady_clock::time_point now,
                         const pollfd* webFds,
                         std::size_t webFdCount);

   /* The deadline service() is waiting on, while the browser has not started. */
   std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;

   bool browserStarted() const noexcept { return browserPid_ > 0; }
   pid_t browserPid() const noexcept { return browserPid_; }
   const WebLaunchOptions& options() const noexcept { return options_; }

   void stop(WebStopMode mode = WebStopMode::AllProcesses);

  private:
   struct Child {
      pid_t pid = -1;
      int pidfd = -1;
      bool started = false;
      bool exited = false;
      int exitStatus = 0;
   };

   Result<void, TransportError> startChild(
       Child& child, const std::vector<std::string>& command);
   Result<void, TransportError> startBrowser();
   bool bothPortsAccept() const;
   void markExited(Child& child, int status);
   void closeChildFds();

   WebLaunchOptions options_;
   Child pageServer_;
   Child bridge_;
   Child browser_;
   pid_t browserPid_ = -1;
   bool stopped_ = false;
   std::chrono::steady_clock::time_point readyDeadline_{};
};

}  // namespace sdl3mcp
