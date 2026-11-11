/*
  The web spawner's launch plan and the two inputs it cannot invent.

  A browser cannot dial a TCP endpoint, so a web build is reached through three
  processes: a static page server, a websocat bridge that turns the page's
  WebSocket into the relay's TCP stream, and an isolated browser. Composing
  their argv vectors is a pure value operation, kept apart from launching so the
  command shapes are reviewable and testable before anything forks or binds.

  preflightWebLaunch is the check that runs before anything is created: the two
  tools the plan names must be on PATH, and the page root must exist and hold the
  entry. Each failure names the fix, so a missing websocat is one sentence
  instead of a half-launched stack with a server already bound to a port. It also
  resolves the two tools into the options, so the commands it launches are the
  ones it checked.

  makeLaunchPlan validates the whole set before it writes a plan: a missing page
  root, entry, tool, browser or relay address, a port still left at zero, or a
  page and bridge port that collide all fail with a message naming the field. The
  launcher fills the two ephemeral ports before calling this, so a zero port
  means the launcher skipped that step instead of asking for one.

  resolveBrowser and seedProfile resolve the two facts a launch must not
  improvise: which Firefox to run, and which profile it may write. Isolation is
  mandatory, and dom.allow_scripts_to_close_windows — what lets the page close
  itself when the application terminates — can only be set in a profile we own,
  never in the user's settings. Both functions report through `error` and leave
  their out parameter untouched when they fail. plannedProfilePath names the
  directory a launch would seed without creating it, because a plan print has no
  run to clean one up.
*/

#ifndef SDL3MCP_SUPERVISOR_WEB_APP_HPP
#define SDL3MCP_SUPERVISOR_WEB_APP_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp
{

/* The loopback host every web-mode endpoint is bound to and probed on. */
constexpr const char *kWebLoopbackHost = "127.0.0.1";

struct WebAppOptions
{
    std::string pageRoot;        /* directory served as the page */
    std::string entry;           /* e.g. catalyst.html */
    std::string relayHost;       /* relay the bridge dials */
    std::uint16_t relayPort = 0;
    std::string python;          /* resolved python3 executable */
    std::string websocat;        /* resolved websocat executable */
    std::string browser;         /* resolved browser executable */
    std::string profileDir;      /* throwaway profile directory */
    std::uint16_t pagePort = 0;  /* 0 until the launcher picks one */
    std::uint16_t bridgePort = 0;
};

/*
  The launch preflight, in the order the plan depends on it: websocat and
  python3 on PATH (resolved into `options`), then a page root that is a directory
  holding `options.entry`. Nothing is created; a failure leaves the filesystem
  and the previously resolved fields alone.
*/
bool preflightWebLaunch(WebAppOptions &options, std::string &error);

/*
  Fills `plan` with exactly three argv vectors in launch order: the page server,
  the WebSocket bridge, then the browser. On failure `plan` is empty and `error`
  names the field that rejected the options.
*/
bool makeLaunchPlan(const WebAppOptions &options,
                    std::vector<std::vector<std::string>> &plan,
                    std::string &error);

/*
  Fills `path` with the absolute Firefox executable to run. `$BROWSER` wins when
  it names firefox or firefox-esr and that path is executable; otherwise PATH is
  searched for firefox, then firefox-esr. There is no fallback: any other browser
  would open the page in the user's own session instead of an isolated one, so
  failure says to install firefox and names no alternative.
*/
bool resolveBrowser(std::string &path, std::string &error);

/*
  Creates a unique directory from sdl3mcp-web-XXXXXX under $XDG_RUNTIME_DIR when
  it is set and writable, otherwise under /tmp, and writes the user.js that
  carries the throwaway profile's preferences. A failure leaves `directory`
  untouched and names what could not be created.
*/
bool seedProfile(std::string &directory, std::string &error);

/*
  Names the directory a launch would seed, without creating it: the mkdtemp()
  template under the same parent, sdl3mcp-web-XXXXXX. A plan print reports this
  so the output still says which profile a launch would use, while the directory
  itself stays owned by the run that starts children and removes it.
*/
std::string plannedProfilePath();

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_WEB_APP_HPP */
