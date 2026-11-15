#include "HandlerCli.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "Configuration.h"

namespace sdl3mcp {
namespace {

constexpr std::string_view kListenOption = "--listen";
constexpr std::string_view kListenPrefix = "--listen=";
constexpr std::string_view kConnectOption = "--connect";
constexpr std::string_view kConnectPrefix = "--connect=";

}  // namespace

bool applyEndpointOption(std::string_view argument, HandlerCliResult& result) {
   if (argument == kListenOption) {
      result.error = "--listen requires an address written as --listen=HOST:PORT";
      return true;
   }
   if (argument.rfind(kListenPrefix, 0) == 0) {
      if (!result.options.listenHost.empty()) {
         result.error = "--listen may be given only once";
         return true;
      }

      const std::string_view value(argument.data() + kListenPrefix.size(),
                                   argument.size() - kListenPrefix.size());
      std::string_view host;
      std::uint16_t port = 0;
      /* Port 0 asks the kernel for an ephemeral port. */
      if (!parseEndpoint(value, host, port, true)) {
         result.error =
             "invalid value for --listen: expected HOST:PORT or [V6]:PORT";
         return true;
      }
      result.options.listenHost.assign(host.data(), host.size());
      result.options.listenPort = port;
      return true;
   }

   if (argument == kConnectOption) {
      result.error =
          "--connect requires an address written as --connect=HOST:PORT";
      return true;
   }
   if (argument.rfind(kConnectPrefix, 0) == 0) {
      if (!result.options.connectHost.empty()) {
         result.error = "--connect may be given only once";
         return true;
      }

      const std::string_view value(argument.data() + kConnectPrefix.size(),
                                   argument.size() - kConnectPrefix.size());
      std::string_view host;
      std::uint16_t port = 0;
      if (!parseEndpoint(value, host, port, false)) {
         result.error =
             "invalid value for --connect: expected HOST:PORT or [V6]:PORT";
         return true;
      }
      result.options.connectHost.assign(host.data(), host.size());
      result.options.connectPort = port;
      return true;
   }

   return false;
}

}  // namespace sdl3mcp
