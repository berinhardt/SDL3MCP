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

}  // namespace

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
