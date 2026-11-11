/*
  The web spawner's launch plan.

  A browser cannot dial a TCP endpoint, so a web build is reached through three
  processes: a static page server, a websocat bridge that turns the page's
  WebSocket into the relay's TCP stream, and an isolated browser. Composing
  their argv vectors is a pure value operation, kept apart from launching so the
  command shapes are reviewable and testable before anything forks or binds.

  makeLaunchPlan validates the whole set before it writes a plan: a missing page
  root, entry, browser or relay address, a port still left at zero, or a page
  and bridge port that collide all fail with a message naming the field. The
  launcher fills the two ephemeral ports before calling this, so a zero port
  means the launcher skipped that step instead of asking for one.
*/

#ifndef SDL3MCP_SUPERVISOR_WEB_APP_HPP
#define SDL3MCP_SUPERVISOR_WEB_APP_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp
{

struct WebAppOptions
{
    std::string pageRoot;        /* directory served as the page */
    std::string entry;           /* e.g. catalyst.html */
    std::string relayHost;       /* relay the bridge dials */
    std::uint16_t relayPort = 0;
    std::string browser;         /* resolved browser executable */
    std::string profileDir;      /* throwaway profile directory */
    std::uint16_t pagePort = 0;  /* 0 until the launcher picks one */
    std::uint16_t bridgePort = 0;
};

/*
  Fills `plan` with exactly three argv vectors in launch order: the page server,
  the WebSocket bridge, then the browser. On failure `plan` is empty and `error`
  names the field that rejected the options.
*/
bool makeLaunchPlan(const WebAppOptions &options,
                    std::vector<std::vector<std::string>> &plan,
                    std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_WEB_APP_HPP */
