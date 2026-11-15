#include "Handler.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "Configuration.h"

namespace sdl3mcp {
namespace {

/*
  Prints one argv the way a shell would read it. A word made only of characters
  a shell leaves alone is printed bare; anything else is single-quoted, with an
  embedded quote written as '\'' so the line stays unambiguous.
*/
std::string shellQuote(std::string_view argument) {
   const auto isSafe = [](char character) {
      return (character >= 'a' && character <= 'z') ||
             (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9') || character == '_' ||
             character == '-' || character == '.' || character == '/' ||
             character == ':' || character == '=';
   };
   if (!argument.empty() &&
       std::all_of(argument.begin(), argument.end(), isSafe)) {
      return std::string(argument);
   }

   std::string quoted = "'";
   for (const char character : argument) {
      if (character == '\'') {
         quoted += "'\\''";
      } else {
         quoted += character;
      }
   }
   quoted += "'";
   return quoted;
}

}  // namespace

int Handler::printDryRun() {
   const SpawnTarget* target = nullptr;
   for (const SpawnTarget& candidate : targets_) {
      if (candidate.alias == options_.dryRunAlias) {
         target = &candidate;
         break;
      }
   }
   if (target != nullptr) {
      /*
        A dry run binds nothing, so a port still at zero is the endpoint the
        spawn would have been told about: print the placeholder rather than a
        guess.
      */
      const std::string endpoint =
          options_.listenPort == 0
              ? std::string("HOST:PORT")
              : endpointText(options_.listenHost, options_.listenPort);

      std::string line = "command:";
      for (const std::string& argument : targetCommand(*target, endpoint)) {
         line += " " + shellQuote(argument);
      }
      std::printf("%s\n", line.c_str());
      std::fflush(stdout);
      return 0;
   }

   for (const WebApp& candidate : webApps_) {
      if (candidate.alias != options_.dryRunAlias) {
         continue;
      }

      WebLaunchOptions web;
      web.pageRoot = candidate.root;
      web.handlerEndpoint =
          options_.listenPort == 0
              ? std::string("HOST:PORT")
              : endpointText(options_.listenHost, options_.listenPort);
      /*
        A dry run binds nothing, so it reserves no port: the plan prints the
        placeholders a launch would replace with two ephemeral numbers.
      */
      web.placeholderPorts = true;

      const Result<void, TransportError> preflight = preflightWebLaunch(web);
      if (!preflight) {
         std::fprintf(stderr, "sdl3mcp: --dry-run: %s\n",
                      preflight.error().message.c_str());
         std::fflush(stderr);
         return 2;
      }

      /*
        A plan print names the profile a launch would seed and creates nothing:
        the directory belongs to the run that starts children, and a printing
        process has none to remove it.
      */
      web.profileDir = plannedProfilePath();

      const Result<std::vector<std::vector<std::string>>, TransportError> plan =
          makeWebLaunchPlan(web);
      if (!plan) {
         std::fprintf(stderr, "sdl3mcp: --dry-run: %s\n",
                      plan.error().message.c_str());
         std::fflush(stderr);
         return 2;
      }

      const std::string bridge =
          std::string(kWebLoopbackHost) + ":BRIDGE_PORT";
      std::printf("page=http://%s:PAGE_PORT/%s?mcp=%s\n", kWebLoopbackHost,
                  web.entry.c_str(), bridge.c_str());
      std::printf("bridge=%s\n", bridge.c_str());
      std::printf("profile=%s\n", web.profileDir.c_str());
      for (const std::vector<std::string>& command : *plan) {
         std::string line = "command:";
         for (const std::string& argument : command) {
            line += " " + shellQuote(argument);
         }
         std::printf("%s\n", line.c_str());
      }
      std::fflush(stdout);
      return 0;
   }

   std::fprintf(stderr, "sdl3mcp: --dry-run: unknown alias \"%s\"\n",
                options_.dryRunAlias.c_str());
   std::fflush(stderr);
   return 2;
}

}  // namespace sdl3mcp
