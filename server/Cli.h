/*
  The Handler's command line.

  One shape with an optional upstream:

    --listen=HOST:PORT                 the downstream application listener
    --connect=HOST:PORT                dial an upstream MCP client (a Relay)
    --targets=PATH                     an alias-to-argv app source (repeatable)
    --timeout-ms=N                     forward/attach/spawn budget, default 10000
    --dry-run=ALIAS                    print what app_spawn_<alias> would run

  Without --connect the client-facing face is stdio (a Server); with it the
  Handler dials the endpoint and is the MCP server on that TCP connection (a
  Relay). Every Handler needs a downstream source: --listen, --targets, or (from
  ticket 09) --web. When an app source is present --listen defaults to
  127.0.0.1:0; otherwise it is required.

  Splitting the parse from argv keeps the grammar testable without a process.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp {

constexpr int kDefaultHandlerTimeoutMs = 10000;
constexpr int kMinHandlerTimeoutMs = 1;
constexpr int kMaxHandlerTimeoutMs = 60000;

struct HandlerOptions {
   /* Empty unless --listen=HOST:PORT was given. */
   std::string listenHost;
   std::uint16_t listenPort = 0;
   bool hasListen = false;
   /* Empty unless --connect=HOST:PORT was given. */
   std::string connectHost;
   std::uint16_t connectPort = 0;
   bool hasConnect = false;
   /* The default forward budget; a call's timeout_ms overrides it. */
   int timeoutMs = kDefaultHandlerTimeoutMs;

   /* Every --targets source, in command-line order. */
   std::vector<std::string> targetsPaths;

   /* --dry-run=ALIAS prints what app_spawn_<alias> would run and starts nothing. */
   bool hasDryRun = false;
   std::string dryRunAlias;

   /* A Relay dials an upstream; a Server speaks stdio to its client. */
   bool relay() const noexcept { return hasConnect; }

   /*
     True when a --targets or (later) --web source contributes app_spawn_*
     tools. --listen is then optional and defaults to loopback:0.
   */
   bool hasAppSource() const noexcept { return !targetsPaths.empty(); }
};

struct HandlerCliResult {
   bool ok = false;
   /* Set for --help/--version: print `message` and exit with `exitCode`. */
   bool exit = false;
   int exitCode = 0;
   HandlerOptions options;
   std::string message;
   /* Why the parse failed, when ok is false. */
   std::string error;
};

/*
  Parses argv without argv[0]. Returns ok=false with a one-line explanation when
  an option is unknown, missing its value, repeated, out of range, or when no
  downstream listener was given.
*/
HandlerCliResult parseHandlerCommandLine(const std::vector<std::string>& arguments);

/* The usage block main() prints to stderr after a parse error or --help. */
std::string handlerUsageText();

}  // namespace sdl3mcp
