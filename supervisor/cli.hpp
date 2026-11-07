/*
  The supervisor's own command line: options first, then either --target and the
  child argv or --listen=HOST:PORT for relay mode.

  Splitting the parse from argv keeps the grammar testable without a process:
  parseCommandLine() takes the plain argument list and returns a value. Everything
  after the --target executable belongs to the child verbatim, so a child flag
  that happens to look like a supervisor option is never consumed here.
*/

#ifndef SDL3MCP_SUPERVISOR_CLI_HPP
#define SDL3MCP_SUPERVISOR_CLI_HPP

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
    /* Empty unless --listen=HOST:PORT was given; relay mode then serves MCP. */
    std::string listenHost;
    std::uint16_t listenPort = 0;

    bool relay() const noexcept { return !listenHost.empty(); }
};

struct CliParseResult
{
    bool ok = false;
    SupervisorOptions options;
    std::string error;
};

/*
  Parses argv without argv[0]. Returns ok=false with a one-line explanation when
  an option is unknown, missing its value, neither --target nor --listen was
  given, or both were given.
*/
CliParseResult parseCommandLine(const std::vector<std::string> &arguments);

/* The single usage line main() prints to stderr after a parse error. */
std::string usageLine();

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_CLI_HPP */
