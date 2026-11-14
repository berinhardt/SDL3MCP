#include "Cli.h"

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
constexpr std::string_view kTimeoutOption = "--timeout-ms";
constexpr std::string_view kTimeoutPrefix = "--timeout-ms=";
constexpr std::string_view kHelpOption = "--help";
constexpr std::string_view kVersionOption = "--version";

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
         if (result.options.hasListen) {
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
         result.options.hasListen = true;
         index += 1;
         continue;
      }

      if (argument == kConnectOption) {
         result.error =
             "--connect requires an address written as --connect=HOST:PORT";
         return result;
      }
      if (argument.rfind(kConnectPrefix, 0) == 0) {
         if (result.options.hasConnect) {
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
         result.options.hasConnect = true;
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

   if (!result.options.hasListen) {
      result.error = "missing --listen=HOST:PORT";
      return result;
   }

   result.ok = true;
   return result;
}

std::string handlerUsageText() {
   return "usage: sdl3mcp --listen=HOST:PORT [--connect=HOST:PORT] "
          "[--timeout-ms=N]\n"
          "  --listen=HOST:PORT   accept one application connection at HOST:PORT\n"
          "  --connect=HOST:PORT  dial an upstream MCP client (a Relay)\n"
          "  --timeout-ms=N       forward/attach budget in ms (1..60000, default "
          "10000)\n"
          "  --help               print this help and exit\n"
          "  --version            print the Handler version and exit";
}

}  // namespace sdl3mcp
