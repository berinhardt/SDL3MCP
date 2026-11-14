/*
  The Handler's command line.

  One shape with an optional upstream and an optional listener:

    --listen=HOST:PORT                 the downstream application listener
    --connect=HOST:PORT                dial an upstream MCP client (a Relay)
    --targets=PATH                     an alias-to-argv app source (repeatable)
    --web=PATH                         an alias-to-web-root app source (repeatable)
    --timeout-ms=N                     forward/attach/spawn budget, default 10000
    --dry-run=ALIAS                    print what app_spawn_<alias> would run
    --dump-tools                       print the tool catalog and exit

  Without --connect the client-facing face is stdio (a Server); with it the
  Handler dials the endpoint and is the MCP server on that TCP connection (a
  Relay). Listening, relaying and dry-run are derived from those option values,
  not tracked separately.

  --listen is optional. With an app source it defaults to an ephemeral loopback
  listener, 127.0.0.1:0, whose bound port the startup line reports; an explicit
  --listen wins. A run with an app source, an explicit --listen, or --dump-tools
  is valid; anything else has nothing to serve and fails, naming --targets and
  --web.

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
   /*
     Empty when no listener was requested. With an app source the CLI fills in
     127.0.0.1:0, so the kernel picks an ephemeral port the startup line reports.
   */
   std::string listenHost;
   std::uint16_t listenPort = 0;
   /* Empty unless --connect=HOST:PORT was given. */
   std::string connectHost;
   std::uint16_t connectPort = 0;
   /* The default forward budget; a call's timeout_ms overrides it. */
   int timeoutMs = kDefaultHandlerTimeoutMs;

   /* Every --targets source, in command-line order. */
   std::vector<std::string> targetsPaths;
   /* Every --web source, in command-line order. */
   std::vector<std::string> webPaths;

   /* --dry-run=ALIAS prints what app_spawn_<alias> would run and starts nothing. */
   std::string dryRunAlias;

   /*
     --dump-tools prints the catalog tools/list would advertise and exits. It
     needs no listener: it is the documentation check's view of the real list.
   */
   bool hasDumpTools = false;

   /* A Relay dials an upstream; a Server speaks stdio to its client. */
   bool relay() const noexcept { return !connectHost.empty(); }

   /* --dry-run=ALIAS: print one alias's plan and start nothing. */
   bool dryRun() const noexcept { return !dryRunAlias.empty(); }

   /*
     True when a --targets or --web source contributes app_spawn_* tools. An app
     source also supplies the listener default.
   */
   bool hasAppSource() const noexcept {
      return !targetsPaths.empty() || !webPaths.empty();
   }
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
  an option is unknown, missing its value, repeated, out of range, or when the
  run has neither an app source nor a listener to serve.
*/
HandlerCliResult parseHandlerCommandLine(const std::vector<std::string>& arguments);

/* The usage block main() prints to stderr after a parse error or --help. */
std::string handlerUsageText();

}  // namespace sdl3mcp
