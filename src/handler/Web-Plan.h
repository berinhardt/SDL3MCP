/*
  The launch planning for a web alias: the resolved executables, the two
  loopback ports, the three-command plan, and the throwaway profile that
  isolates the browser.

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

  This unit's definitions live in two translation units: Web-Plan.cpp holds
  preflight, the browser lookup, the ports and the plan composition;
  Web-Plan-Profile.cpp holds the profile's seed, lock, sweep and removal, and the
  prepareWebLaunch composition that owns them.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Result.h"

namespace sdl3mcp {

/* The entry every web root must hold; --web does not configure it. */
constexpr const char* kWebEntry = "index.html";
/* Every web endpoint is bound and probed on loopback. */
constexpr const char* kWebLoopbackHost = "127.0.0.1";

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
   /*
      A dry run binds no port, so the plan prints PAGE_PORT and BRIDGE_PORT
      placeholders instead of reserving numbers it would have to release.
   */
   bool placeholderPorts = false;
};

/*
  Resolves python3 and websocat on PATH, resolves the browser, and checks that
  the page root is a directory holding `entry`. Nothing is created; a failure
  leaves the filesystem and the previously resolved fields alone.
*/
Result<void, TransportError> preflightWebLaunch(WebLaunchOptions& options);

/*
  Picks two distinct ephemeral loopback ports for the page server and the
  bridge. A port is bound and released immediately; the child binds it for
  real, so the number is a reservation only in the instant it is chosen.
*/
Result<void, TransportError> pickWebPorts(WebLaunchOptions& options);

/* Picks one ephemeral loopback port, or fails with the reason. */
Result<std::uint16_t, TransportError> pickLoopbackPort();

/*
  The whole launch preparation: preflight, sweep abandoned profiles, seed and
  lock the throwaway profile, then pick the two ports. The caller owns the
  profile and the lock descriptor and must stop the run (or remove the profile)
  on every later failure.
*/
Result<void, TransportError> prepareWebLaunch(WebLaunchOptions& options);

/*
  Builds exactly three argv vectors in launch order: the page server, the
  WebSocket bridge, then the browser. On failure the plan is empty and the error
  names the field that rejected the options.
*/
Result<std::vector<std::vector<std::string>>, TransportError> makeWebLaunchPlan(
    const WebLaunchOptions& options);

/*
  Resolves the absolute Firefox executable to run. `$BROWSER` wins when it names
  firefox or firefox-esr and that path is executable; otherwise PATH is searched
  for firefox, then firefox-esr. There is no fallback: any other browser would
  open the page in the user's own session instead of an isolated one.
*/
Result<std::string, TransportError> resolveBrowser();

/*
  Creates a unique directory from sdl3mcp-web-XXXXXX under $XDG_RUNTIME_DIR when
  it is set and writable, otherwise under /tmp, and writes the user.js that
  carries the throwaway profile's preferences. `lockFd` receives a descriptor
  holding an exclusive flock() on the directory's `.lock`, which marks the
  profile as owned by a live run. A failure leaves `directory` and `lockFd`
  untouched and names what could not be created.
*/
Result<void, TransportError> seedProfile(std::string& directory, int& lockFd);

/* Removes a profile tree. Shared by the run's cleanup and the startup sweep. */
Result<void, TransportError> removeProfileTree(const std::string& path);

/*
  Reclaims profile directories whose owner is gone: a directory whose .lock can
  be taken has no live launcher, and one with no .lock at all is swept only once
  it is older than a minute. Returns the number removed, or an error when the
  parent cannot be scanned.
*/
Result<int, TransportError> sweepAbandonedProfiles();

/*
  Names the directory a launch would seed, without creating it: the mkdtemp()
  template under the same parent, sdl3mcp-web-XXXXXX. A plan print reports this
  so the output still says which profile a launch would use.
*/
std::string plannedProfilePath();

}  // namespace sdl3mcp
