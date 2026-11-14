#include "Handler.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

#include "Configuration.h"
#include "ToolArguments.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

const char* webChildName(WebChild child) {
   switch (child) {
      case WebChild::PageServer:
         return "page server";
      case WebChild::Bridge:
         return "bridge";
      case WebChild::Browser:
         return "browser";
   }
   return "child";
}

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

void Handler::handleWebSpawn(const ToolOrder& order,
                             ToolCompletionDelegate& completion,
                             const WebApp& app) {
   if (appPeer_ != nullptr || appReady_ || pendingSpawn_.has_value() ||
       webRun_ != nullptr) {
      completion.complete(ToolResult::text(
          "an application is already attached; one spawn at a time", true));
      return;
   }

   const Result<std::int64_t, ProtocolError> timeout = readTimeoutMs(
       order.arguments, options_.timeoutMs, "app_spawn_" + app.alias);
   if (!timeout) {
      completion.complete(ToolResult::text(timeout.error().message, true));
      return;
   }
   const std::int64_t timeoutMs = *timeout;

   /*
     The listener is bound before the loop accepts tool calls, so its endpoint
     is the one the bridge is told to dial.
   */
   WebLaunchOptions web;
   web.pageRoot = app.root;
   web.handlerEndpoint =
       endpointText(options_.listenHost, listener_.boundPort());

   const Result<void, TransportError> prepared = prepareWebLaunch(web);
   if (!prepared) {
      completion.complete(ToolResult::text(
          "app_spawn_" + app.alias + ": " + prepared.error().message, true));
      return;
   }

   auto run = std::make_unique<WebRun>();
   const Result<void, TransportError> runStarted = run->start(std::move(web));
   if (!runStarted) {
      completion.complete(ToolResult::text(
          "app_spawn_" + app.alias + ": " + runStarted.error().message, true));
      return;
   }
   webRun_ = std::move(run);

   const WebLaunchOptions& started = webRun_->options();
   const std::string bridge =
       std::string(kWebLoopbackHost) + ":" + std::to_string(started.bridgePort);
   std::fprintf(stderr,
                "sdl3mcp: web: page=http://%s:%u/%s?mcp=%s bridge=%s profile=%s\n",
                kWebLoopbackHost, static_cast<unsigned>(started.pagePort),
                started.entry.c_str(), bridge.c_str(), bridge.c_str(),
                started.profileDir.c_str());
   std::fflush(stderr);

   PendingSpawn pending;
   pending.kind = PendingSpawn::Kind::Web;
   pending.alias = app.alias;
   pending.completion = &completion;
   pending.deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
   pending.timeoutMs = timeoutMs;
   pendingSpawn_ = std::move(pending);
}

void Handler::serviceWeb(Clock::time_point now,
                         const pollfd* webFds,
                         std::size_t webFdCount) {
   if (!webRun_) {
      return;
   }

   const WebStackEvent event = webRun_->service(now, webFds, webFdCount);

   /* The browser's pid is only known once the tools accept. */
   if (pendingSpawn_.has_value() &&
       pendingSpawn_->isWeb() &&
       pendingSpawn_->pid < 0 && webRun_->browserStarted()) {
      pendingSpawn_->pid = webRun_->browserPid();
   }

   if (event.kind != WebStackEvent::Kind::None) {
      finishWebRun(event);
   }
}

void Handler::finishWebRun(WebStackEvent event) {
   if (!webRun_) {
      return;
   }

   const pid_t browserPid = webRun_->browserPid();
   /*
     Firefox exiting is the normal end: it is already gone, so only the tools
     are stopped. A tool's death is a failure, and the browser is part of the
     run, so it goes down with them.
   */
   const WebStopMode mode = event.kind == WebStackEvent::Kind::BrowserExited
                                ? WebStopMode::ToolsOnly
                                : WebStopMode::AllProcesses;
   webRun_->stop(mode);
   webRun_.reset();

   if (pendingSpawn_.has_value() &&
       pendingSpawn_->isWeb()) {
      std::string text;
      if (!event.detail.empty()) {
         text = "app_spawn_" + pendingSpawn_->alias + ": " + event.detail;
      } else if (event.kind == WebStackEvent::Kind::BrowserExited) {
         text = "app_spawn_" + pendingSpawn_->alias +
                ": the browser exited before the application connected";
         if (browserPid > 0) {
            text += " (browser pid " + std::to_string(browserPid) + ")";
         }
         if (event.exitStatus != 0) {
            text += " with exit status " + std::to_string(event.exitStatus);
         }
      } else {
         text = "app_spawn_" + pendingSpawn_->alias + ": the " +
                webChildName(event.child) +
                " exited before the application connected";
      }
      completeSpawn(text, true);
      return;
   }

   if (event.kind == WebStackEvent::Kind::BrowserExited) {
      if (event.exitStatus != 0) {
         std::fprintf(stderr,
                      "sdl3mcp: web: the browser exited with status %d; the web "
                      "stack was stopped\n",
                      event.exitStatus);
      } else {
         std::fprintf(stderr,
                      "sdl3mcp: web: the browser exited; the web stack was "
                      "stopped\n");
      }
   } else if (!event.detail.empty()) {
      std::fprintf(stderr, "sdl3mcp: web: %s; the web stack was stopped\n",
                   event.detail.c_str());
   } else {
      std::fprintf(stderr, "sdl3mcp: web: the %s exited; the web stack was stopped\n",
                   webChildName(event.child));
   }
   std::fflush(stderr);
}

}  // namespace sdl3mcp
