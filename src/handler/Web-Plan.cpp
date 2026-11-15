#include "Web-Plan.h"

#include <string>
#include <utility>
#include <vector>

namespace sdl3mcp {
namespace {

constexpr const char* kBrowserWidth = "1280";
constexpr const char* kBrowserHeight = "800";

}  // namespace

Result<std::vector<std::vector<std::string>>, TransportError>
makeWebLaunchPlan(const WebLaunchOptions& options) {
   std::vector<std::vector<std::string>> plan;

   if (options.pageRoot.empty()) {
      return TransportError{"pageRoot must not be empty"};
   }
   if (options.entry.empty()) {
      return TransportError{"entry must not be empty"};
   }
   if (!isExecutable(options.python)) {
      return TransportError{"python must name an executable"};
   }
   if (!isExecutable(options.websocat)) {
      return TransportError{"websocat must name an executable"};
   }
   if (options.browser.empty()) {
      return TransportError{"browser must not be empty"};
   }
   if (options.handlerEndpoint.empty()) {
      return TransportError{"handlerEndpoint must not be empty"};
   }
   if (!options.placeholderPorts) {
      if (options.pagePort == 0) {
         return TransportError{"pagePort must not be zero"};
      }
      if (options.bridgePort == 0) {
         return TransportError{"bridgePort must not be zero"};
      }
      if (options.pagePort == options.bridgePort) {
         return TransportError{"pagePort and bridgePort must differ"};
      }
   }

   const std::string pagePort = options.placeholderPorts
                                    ? "PAGE_PORT"
                                    : std::to_string(options.pagePort);
   const std::string bridgePort = options.placeholderPorts
                                      ? "BRIDGE_PORT"
                                      : std::to_string(options.bridgePort);
   const std::string bridgeEndpoint =
       std::string(kWebLoopbackHost) + ":" + bridgePort;

   std::vector<std::string> pageServer{
       options.python,
       "-m",
       "http.server",
       pagePort,
       "--bind",
       kWebLoopbackHost,
       "--directory",
       options.pageRoot,
   };

   std::vector<std::string> bridge{
       options.websocat,
       "--binary",
       "ws-l:" + bridgeEndpoint,
       "tcp:" + options.handlerEndpoint,
   };

   std::vector<std::string> browser{
       options.browser,
       "--no-remote",
       "--new-instance",
       "--profile",
       options.profileDir,
       "--width",
       kBrowserWidth,
       "--height",
       kBrowserHeight,
       "http://" + std::string(kWebLoopbackHost) + ":" + pagePort + "/" +
           options.entry + "?mcp=" + bridgeEndpoint,
   };

   plan.push_back(std::move(pageServer));
   plan.push_back(std::move(bridge));
   plan.push_back(std::move(browser));
   return plan;
}

}  // namespace sdl3mcp
