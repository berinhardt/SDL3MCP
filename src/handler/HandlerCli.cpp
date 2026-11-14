#include "HandlerCli.h"

#include <SDL3MCP/SDL3MCP_version.h>

#include <charconv>
#include <string>
#include <string_view>

#include "Configuration.h"

namespace sdl3mcp {
namespace {

constexpr std::string_view kListenOption = "--listen";
constexpr std::string_view kListenPrefix = "--listen=";
constexpr std::string_view kConnectOption = "--connect";
constexpr std::string_view kConnectPrefix = "--connect=";
constexpr std::string_view kTargetsOption = "--targets";
constexpr std::string_view kTargetsPrefix = "--targets=";
constexpr std::string_view kWebOption = "--web";
constexpr std::string_view kWebPrefix = "--web=";
constexpr std::string_view kDryRunOption = "--dry-run";
constexpr std::string_view kDryRunPrefix = "--dry-run=";
constexpr std::string_view kTimeoutOption = "--timeout-ms";
constexpr std::string_view kTimeoutPrefix = "--timeout-ms=";
constexpr std::string_view kDumpToolsOption = "--dump-tools";
constexpr std::string_view kHelpOption = "--help";
constexpr std::string_view kVersionOption = "--version";

/* The listener an app source falls back to: loopback, kernel-chosen port. */
constexpr std::string_view kDefaultListenHost = "127.0.0.1";

bool parsePositiveInteger(std::string_view text, int& value) {
   if (text.empty()) {
      return false;
   }

   int parsed = 0;
   const char* begin = text.data();
   const char* end = text.data() + text.size();
   const auto result = std::from_chars(begin, end, parsed);
   if (result.ec != std::errc{} || result.ptr != end || parsed < 1) {
      return false;
   }

   value = parsed;
   return true;
}

}  // namespace

HandlerCliResult parseHandlerCommandLine(
    const std::vector<std::string>& arguments) {
   HandlerCliResult result;
   bool timeoutGiven = false;

   std::size_t index = 0;
   while (index < arguments.size()) {
      const std::string& argument = arguments[index];

      if (argument == kDumpToolsOption) {
         if (result.options.hasDumpTools) {
            result.error = "--dump-tools may be given only once";
            return result;
         }
         result.options.hasDumpTools = true;
         index += 1;
         continue;
      }

      if (argument == kHelpOption) {
         result.ok = true;
         result.exit = true;
         result.exitCode = 0;
         result.message = handlerUsageText();
         return result;
      }
      if (argument == kVersionOption) {
         result.ok = true;
         result.exit = true;
         result.exitCode = 0;
         result.message = "SDL3MCP " SDL3MCP_VERSION_STRING;
         return result;
      }

      if (argument == kListenOption) {
         result.error = "--listen requires an address written as --listen=HOST:PORT";
         return result;
      }
      if (argument.rfind(kListenPrefix, 0) == 0) {
         if (!result.options.listenHost.empty()) {
            result.error = "--listen may be given only once";
            return result;
         }

         const std::string_view value(argument.data() + kListenPrefix.size(),
                                      argument.size() - kListenPrefix.size());
         std::string_view host;
         std::uint16_t port = 0;
         /* Port 0 asks the kernel for an ephemeral port. */
         if (!parseEndpoint(value, host, port, true)) {
            result.error =
                "invalid value for --listen: expected HOST:PORT or [V6]:PORT";
            return result;
         }
         result.options.listenHost.assign(host.data(), host.size());
         result.options.listenPort = port;
         index += 1;
         continue;
      }

      if (argument == kConnectOption) {
         result.error =
             "--connect requires an address written as --connect=HOST:PORT";
         return result;
      }
      if (argument.rfind(kConnectPrefix, 0) == 0) {
         if (!result.options.connectHost.empty()) {
            result.error = "--connect may be given only once";
            return result;
         }

         const std::string_view value(argument.data() + kConnectPrefix.size(),
                                      argument.size() - kConnectPrefix.size());
         std::string_view host;
         std::uint16_t port = 0;
         if (!parseEndpoint(value, host, port, false)) {
            result.error =
                "invalid value for --connect: expected HOST:PORT or [V6]:PORT";
            return result;
         }
         result.options.connectHost.assign(host.data(), host.size());
         result.options.connectPort = port;
         index += 1;
         continue;
      }

      if (argument == kTargetsOption) {
         result.error = "--targets requires a path written as --targets=PATH";
         return result;
      }
      if (argument.rfind(kTargetsPrefix, 0) == 0) {
         const std::string_view value(argument.data() + kTargetsPrefix.size(),
                                      argument.size() - kTargetsPrefix.size());
         if (value.empty()) {
            result.error = "--targets requires a non-empty path";
            return result;
         }
         result.options.targetsPaths.emplace_back(value);
         index += 1;
         continue;
      }

      if (argument == kWebOption) {
         result.error = "--web requires a path written as --web=PATH";
         return result;
      }
      if (argument.rfind(kWebPrefix, 0) == 0) {
         const std::string_view value(argument.data() + kWebPrefix.size(),
                                      argument.size() - kWebPrefix.size());
         if (value.empty()) {
            result.error = "--web requires a non-empty path";
            return result;
         }
         result.options.webPaths.emplace_back(value);
         index += 1;
         continue;
      }

      if (argument == kDryRunOption) {
         result.error =
             "--dry-run requires an alias written as --dry-run=ALIAS";
         return result;
      }
      if (argument.rfind(kDryRunPrefix, 0) == 0) {
         if (result.options.dryRun()) {
            result.error = "--dry-run may be given only once";
            return result;
         }
         const std::string_view value(argument.data() + kDryRunPrefix.size(),
                                      argument.size() - kDryRunPrefix.size());
         if (value.empty()) {
            result.error = "--dry-run requires a non-empty alias";
            return result;
         }
         result.options.dryRunAlias.assign(value.data(), value.size());
         index += 1;
         continue;
      }

      if (argument == kTimeoutOption) {
         if (timeoutGiven) {
            result.error = "--timeout-ms may be given only once";
            return result;
         }
         if (index + 1 >= arguments.size()) {
            result.error = "--timeout-ms requires a value";
            return result;
         }
         if (!parsePositiveInteger(arguments[index + 1],
                                   result.options.timeoutMs) ||
             result.options.timeoutMs < kMinHandlerTimeoutMs ||
             result.options.timeoutMs > kMaxHandlerTimeoutMs) {
            result.error =
                "--timeout-ms requires an integer in 1..60000";
            return result;
         }
         timeoutGiven = true;
         index += 2;
         continue;
      }
      if (argument.rfind(kTimeoutPrefix, 0) == 0) {
         if (timeoutGiven) {
            result.error = "--timeout-ms may be given only once";
            return result;
         }
         const std::string_view value(argument.data() + kTimeoutPrefix.size(),
                                      argument.size() - kTimeoutPrefix.size());
         if (!parsePositiveInteger(value, result.options.timeoutMs) ||
             result.options.timeoutMs < kMinHandlerTimeoutMs ||
             result.options.timeoutMs > kMaxHandlerTimeoutMs) {
            result.error =
                "invalid value for --timeout-ms: expected an integer in 1..60000";
            return result;
         }
         timeoutGiven = true;
         index += 1;
         continue;
      }

      result.error = "unexpected argument: " + argument;
      return result;
   }

   /* A catalog dump prints and exits, so it needs neither a listener nor an app. */
   if (result.options.hasDumpTools) {
      result.ok = true;
      return result;
   }

   if (result.options.dryRun() && !result.options.hasAppSource()) {
      result.error = "--dry-run requires an app source (--targets=PATH or --web=PATH)";
      return result;
   }

   if (result.options.listenHost.empty()) {
      if (!result.options.hasAppSource()) {
         result.error =
             "nothing to serve: give an app source (--targets=PATH or --web=PATH) "
             "or an explicit --listen=HOST:PORT";
         return result;
      }
      /* An app source needs somewhere for the spawned app to dial back. */
      result.options.listenHost = kDefaultListenHost;
      result.options.listenPort = 0;
   }

   result.ok = true;
   return result;
}

std::string handlerUsageText() {
   return "usage: sdl3mcp [--listen=HOST:PORT] [--connect=HOST:PORT] "
          "[--targets=PATH ...] [--web=PATH ...] [--timeout-ms=N] "
          "[--dry-run=ALIAS] [--dump-tools]\n"
          "  --listen=HOST:PORT   accept one application connection at HOST:PORT;\n"
          "                       optional, and with an app source it defaults to\n"
          "                       127.0.0.1:0, an ephemeral loopback port\n"
          "  --connect=HOST:PORT  dial an upstream MCP client (a Relay)\n"
          "  --targets=PATH       an alias-to-argv app source; a JSON file or a\n"
          "                       directory of *.json files (repeatable)\n"
          "  --web=PATH           an alias-to-web-root app source; a JSON file\n"
          "                       mapping each alias to the root it serves\n"
          "                       (repeatable)\n"
          "  --timeout-ms=N       forward/attach/spawn budget in ms (1..60000,\n"
          "                       default 10000)\n"
          "  --dry-run=ALIAS      print the argv app_spawn_<alias> would run, or\n"
          "                       the web commands/page/bridge/profile, and start\n"
          "                       nothing\n"
          "  --dump-tools         print the tools/list catalog as JSON and exit\n"
          "  --help               print this help and exit\n"
          "  --version            print the Handler version and exit\n"
          "An app source or an explicit --listen is required; --dump-tools needs "
          "neither.";
}

}  // namespace sdl3mcp
