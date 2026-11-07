/*
  The supervisor's own command line.

  Three modes:
    --target <executable> [app args...]   manage a child app over stdio
    --listen=HOST:PORT                    act as a relay for a dialing app
    --connect=HOST:PORT --targets=FILE    spawn apps for a relay, one at a time

  --listen also serves as the bind address for the spawner's per-spawn listener;
  without it the spawner binds 127.0.0.1:0.

  Splitting the parse from argv keeps the grammar testable without a process:
  parseCommandLine() takes the plain argument list and returns a value. Everything
  after the --target executable belongs to the child verbatim, so a child flag
  that happens to look like a supervisor option is never consumed here. The
  targets file is read here too, so a bad file is a single startup error.
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
    /* --targets=FILE as written, plus its validated contents. */
    std::string targetsPath;
    SpawnTargets targets;

    /* Spawner mode: dial a relay and spawn the targets on demand. */
    bool spawner() const noexcept { return !connectHost.empty(); }
    /* Relay mode: only --listen was given (the app dials us). */
    bool relay() const noexcept { return !listenHost.empty() && !spawner(); }
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
  --targets are not supplied together, or the targets file fails validation.
*/
CliParseResult parseCommandLine(const std::vector<std::string> &arguments);

/* The single usage line main() prints to stderr after a parse error. */
std::string usageLine();

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_CLI_HPP */
