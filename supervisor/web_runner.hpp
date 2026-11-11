/*
  The web spawner's launcher: three processes, one lifetime.

  A browser-hosted build is a page server, a WebSocket bridge and an isolated
  browser, and the browser is the one that hurts if it is left behind: it is a
  window on the user's desktop. So every child is forked into its own process
  group, which turns the stop into one signal per group instead of one per pid,
  and this launcher owns all three from start to reap.

  Startup follows the plan exactly: the page server and the bridge first, then a
  bounded wait (kReadyWindowMs) for their two ports to accept a connection, and
  only then the browser. The wait is advisory, because a server that is alive but
  slow must not hold the browser back forever; a child that exits while it runs
  is a real failure and names itself.

  Supervising is a poll loop. A browser that exits with status 0 has closed its
  own page after the application shut down, which is the normal end of a run: the
  others are stopped and the outcome is clean. Every other exit — a browser with
  a non-zero status, or a page server or bridge leaving at any point, because
  those are tools whose death is never the intended end — fails the run and
  names the child. SIGINT and SIGTERM take the same stop path with a clean
  outcome, matching how the supervisor treats a client that leaves.

  Stopping is SIGTERM to every group, one grace period, SIGKILL for whatever is
  still alive, a wait for every child, and removal of the throwaway profile. A
  stop that had to escalate is reported, and so is a profile that could not be
  removed.
*/

#ifndef SDL3MCP_SUPERVISOR_WEB_RUNNER_HPP
#define SDL3MCP_SUPERVISOR_WEB_RUNNER_HPP

#include "web_app.hpp"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace sdl3mcp
{

/* How long the page and bridge get to accept a connection before the browser. */
constexpr int kWebReadyWindowMs = 1000;

struct WebRunOutcome
{
    /* 0 for a clean end (a stop request or the page closing), 1 for a failure. */
    int exitCode = 0;
    /* The failure that ended the run, empty on a clean end. */
    std::string failure;
    /* True when a stop needed SIGKILL rather than SIGTERM. */
    bool escalated = false;
    /* A profile the launcher could not remove, empty when it is gone. */
    std::string cleanupError;
};

/*
  Starts the plan's three commands in order, supervises them until one exits or
  `stopRequested` becomes true, then stops every group and removes
  `options.profileDir`. Diagnostics go to stderr, which the children also
  inherit, so a child's own complaint lands next to ours.

  Must be called with SIGINT/SIGTERM blocked: it never installs a handler, it
  only observes `stopRequested`, which the caller's sigwait() thread sets.
*/
WebRunOutcome runWebApp(const WebAppOptions &options,
                        const std::vector<std::vector<std::string>> &plan,
                        const std::atomic<bool> &stopRequested);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_WEB_RUNNER_HPP */
