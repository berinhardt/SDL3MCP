/*
  The Handler's web app source and the web run it starts.

  A --web path is a JSON file mapping each alias to a page root. Every entry
  contributes app_spawn_<alias>, exactly like a --targets entry, and both
  sources accumulate into one alias map. The page entry is always index.html;
  a relative root is left as written and resolves against the Handler's working
  directory, because the Handler never chdirs.

  A web alias cannot exec a binary. Its spawn starts three processes that only
  together make the application reachable: a static page server, a websocat
  bridge that turns the page's WebSocket into the Handler's TCP stream, and an
  isolated Firefox that opens the page. Composing their argv is a pure value
  operation (makeWebLaunchPlan) so the command shapes are reviewable and
  testable before anything forks or binds.

  Isolation is mandatory. seedProfile creates a throwaway profile under
  $XDG_RUNTIME_DIR (or /tmp) and writes the user.js that lets the page close its
  own window when the application shuts down; that preference can only live in
  a profile we own. The run holds an exclusive flock on the profile's .lock for
  its lifetime, which is what lets a later launch tell a live profile from an
  abandoned one (sweepAbandonedProfiles).

  WebRun is the runtime half, serviced by the Handler's poll loop: it starts the
  page server and bridge, waits for both loopback ports to accept before the
  browser, and reports a child's exit through its pidfd. The page server and the
  bridge are Handler-owned infrastructure and are stopped together with the
  profile; the browser is the spawned application and is spawn-and-forget.
*/

#pragma once

#include <poll.h>
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp {

/* The entry every web root must hold; --web does not configure it. */
constexpr const char* kWebEntry = "index.html";
/* Every web endpoint is bound and probed on loopback. */
constexpr const char* kWebLoopbackHost = "127.0.0.1";
/* How long the page server and bridge get to accept before the browser. */
constexpr int kWebReadyWindowMs = 1000;
/* How long a stopped web-stack child gets before SIGKILL. */
constexpr int kWebStopGraceMs = 1000;

struct WebApp {
   /* The tool suffix and the app_spawn_<alias> name. */
   std::string alias;
   /* The page root exactly as the file wrote it. */
   std::string root;
   /* The --web file that defined the alias. */
   std::string source;
};

using WebApps = std::vector<WebApp>;

/*
  Reads and validates every --web file in `paths` and accumulates its entries.
  On failure `apps` is empty and `error` names the offending path plus the
  offending key or value.
*/
bool loadWebApps(const std::vector<std::string>& paths,
                 WebApps& apps,
                 std::string& error);

/* One web launch: the resolved tools, the endpoints, and the owned profile. */
struct WebLaunchOptions {
   std::string pageRoot;
   std::string entry = kWebEntry;
   /* The Handler's listener as HOST:PORT; websocat dials it. */
   std::string handlerEndpoint;
   /* Resolved executables, filled by preflightWebLaunch. */
   std::string python;
   std::string websocat;
   std::string browser;
   /* The throwaway profile; profileLockFd is -1 in a plan print. */
   std::string profileDir;
   int profileLockFd = -1;
   /* 0 until the launcher picks an ephemeral port. */
   std::uint16_t pagePort = 0;
   std::uint16_t bridgePort = 0;
};

/*
  Resolves python3 and websocat on PATH, resolves the browser, and checks that
  the page root is a directory holding `entry`. Nothing is created; a failure
  leaves the filesystem and the previously resolved fields alone.
*/
bool preflightWebLaunch(WebLaunchOptions& options, std::string& error);

/*
  Picks two distinct ephemeral loopback ports for the page server and the
  bridge. A port is bound and released immediately; the child binds it for
  real, so the number is a reservation only in the instant it is chosen.
*/
bool pickWebPorts(WebLaunchOptions& options, std::string& error);

/* Picks one ephemeral loopback port, or fails with the reason. */
bool pickLoopbackPort(std::uint16_t& port, std::string& error);

/*
  The whole launch preparation: preflight, sweep abandoned profiles, seed and
  lock the throwaway profile, then pick the two ports. The caller owns the
  profile and the lock descriptor and must stop the run (or remove the profile)
  on every later failure.
*/
bool prepareWebLaunch(WebLaunchOptions& options, std::string& error);

/*
  Fills `plan` with exactly three argv vectors in launch order: the page server,
  the WebSocket bridge, then the browser. On failure `plan` is empty and `error`
  names the field that rejected the options.
*/
bool makeWebLaunchPlan(const WebLaunchOptions& options,
                       std::vector<std::vector<std::string>>& plan,
                       std::string& error);

/*
  Fills `path` with the absolute Firefox executable to run. `$BROWSER` wins when
  it names firefox or firefox-esr and that path is executable; otherwise PATH is
  searched for firefox, then firefox-esr. There is no fallback: any other browser
  would open the page in the user's own session instead of an isolated one.
*/
bool resolveBrowser(std::string& path, std::string& error);

/*
  Creates a unique directory from sdl3mcp-web-XXXXXX under $XDG_RUNTIME_DIR when
  it is set and writable, otherwise under /tmp, and writes the user.js that
  carries the throwaway profile's preferences. `lockFd` receives a descriptor
  holding an exclusive flock() on the directory's `.lock`, which marks the
  profile as owned by a live run. A failure leaves `directory` and `lockFd`
  untouched and names what could not be created.
*/
bool seedProfile(std::string& directory, int& lockFd, std::string& error);

/* Removes a profile tree. Shared by the run's cleanup and the startup sweep. */
bool removeProfileTree(const std::string& path, std::string& error);

/*
  Reclaims profile directories whose owner is gone: a directory whose .lock can
  be taken has no live launcher, and one with no .lock at all is swept only once
  it is older than a minute. Returns the number removed, or -1 with `error` set
  when the parent cannot be scanned.
*/
int sweepAbandonedProfiles(std::string& error);

/*
  Names the directory a launch would seed, without creating it: the mkdtemp()
  template under the same parent, sdl3mcp-web-XXXXXX. A plan print reports this
  so the output still says which profile a launch would use.
*/
std::string plannedProfilePath();

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

   bool start(WebLaunchOptions options, std::string& error);

   /* Appends one pollfd per live child, in page/bridge/browser order. */
   void appendPollFds(std::vector<pollfd>& descriptors) const;

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
   };

   bool startChild(Child& child,
                   const std::vector<std::string>& command,
                   std::string& error);
   bool startBrowser(std::string& error);
   bool bothPortsAccept() const;
   void markExited(Child& child);
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
