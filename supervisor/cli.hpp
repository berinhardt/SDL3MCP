/*
  The supervisor's own command line.

  Four modes:
    --target <executable> [app args...]   manage a child app over stdio
    --listen=HOST:PORT                    act as a relay for a dialing app
    --connect=HOST:PORT --targets=PATH    spawn apps for a relay, one at a time
    --web-root=DIR [--web-entry=NAME]     print the launch plan for a web build

  --targets may be repeated; each PATH is a JSON file or a directory of *.json
  files, and all of them accumulate into one alias map. --listen also serves as
  the bind address for the spawner's per-spawn listener; without it the spawner
  binds 127.0.0.1:0. --web-entry defaults to catalyst.html and requires
  --web-root; the web mode owns the whole launch and cannot be combined with
  --target, --listen or --targets. In web mode --connect names the relay the
  bridge dials and --web-port/--bridge-port pin the page and bridge ports; the
  web mode launches and supervises the three processes, and --print-plan asks for
  the plan instead of the launch.

  Splitting the parse from argv keeps the grammar testable without a process:
  parseCommandLine() takes the plain argument list and returns a value. Everything
  after the --target executable belongs to the child verbatim, so a child flag
  that happens to look like a supervisor option is never consumed here. The
  targets sources are read here too, so a bad source is a single startup error.
*/

#ifndef SDL3MCP_SUPERVISOR_CLI_HPP
#define SDL3MCP_SUPERVISOR_CLI_HPP

#include "targets.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp
{

constexpr int kDefaultStartupTimeoutMs = 10000;
constexpr int kDefaultTerminateTimeoutMs = 60000;
constexpr const char *kDefaultWebEntry = "catalyst.html";
/*
  The page and bridge ports default to the pair the documentation shows so a
  plan can be composed without pinning either one; the launcher overrides them
  with the ephemeral ports it binds.
*/
constexpr std::uint16_t kDefaultWebPagePort = 18080;
constexpr std::uint16_t kDefaultWebBridgePort = 18081;

struct SupervisorOptions
{
    int startupTimeoutMs = kDefaultStartupTimeoutMs;
    int terminateTimeoutMs = kDefaultTerminateTimeoutMs;
    std::string target;
    /* Child argv after argv[0], passed through unchanged. */
    std::vector<std::string> targetArguments;
    /* Empty unless --listen=HOST:PORT was given. In relay mode it is the
       listener; in spawner mode it is the per-spawn bind address. */
    std::string listenHost;
    std::uint16_t listenPort = 0;
    /* The relay to dial; empty unless --connect=HOST:PORT was given. */
    std::string connectHost;
    std::uint16_t connectPort = 0;
    /* --targets=PATH as written, one entry per flag. */
    std::vector<std::string> targetsPaths;
    SpawnTargets targets;
    /* --web-root=DIR, empty unless web mode was selected. */
    std::string webRoot;
    /* --web-entry=NAME, defaulting to kDefaultWebEntry. */
    std::string webEntry = kDefaultWebEntry;
    /* --web-port=N and --bridge-port=N, defaulting to the documented pair. */
    std::uint16_t webPort = kDefaultWebPagePort;
    std::uint16_t bridgePort = kDefaultWebBridgePort;
    /* --print-plan: print the plan a launch would use, start nothing. */
    bool printPlan = false;

    /* Spawner mode: dial a relay and spawn the targets on demand. */
    bool spawner() const noexcept { return !connectHost.empty(); }
    /* Relay mode: only --listen was given (the app dials us). */
    bool relay() const noexcept { return !listenHost.empty() && !spawner(); }
    /* Web mode: compose a page, bridge and browser launch from a web build. */
    bool web() const noexcept { return !webRoot.empty(); }
};

struct CliParseResult
{
    bool ok = false;
    SupervisorOptions options;
    std::string error;
};

/*
  Parses argv without argv[0]. Returns ok=false with a one-line explanation when
  an option is unknown, missing its value, the modes conflict, both --connect and
  --targets are not supplied together, a web option is given without --web-root,
  or a targets source fails validation.
*/
CliParseResult parseCommandLine(const std::vector<std::string> &arguments);

/* The single usage line main() prints to stderr after a parse error. */
std::string usageLine();

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_CLI_HPP */
