/*
  The Handler: one MCP server on the client-facing stream and one MCP client on
  the application-facing socket, driven by one poll loop.

  The client-facing face is stdio in a Server and a dialed socket in a Relay. It
  is a common-core Session over the Handler's catalog, so initialize, ping and
  tools/list are answered here. The application face is an AppPeer: the Handler
  accepts one connection at a time, performs the private handshake, merges the
  application's tools, and forwards tools/call to it.

  The Handler owns presentation: a raw sdl_capture/sdl_input frame is PNG-encoded
  at the native dimensions and answered as a text block plus an image block. A
  content result from an application that already presented its own frame is
  relayed verbatim, so the hop nearest the app encodes exactly once.

  Nothing here blocks: the loop polls the two streams, the listener and a
  signalfd; waits and forwards carry deadlines that the loop enforces.
*/

#pragma once

#include <poll.h>
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "AppPeer.h"
#include "Cli.h"
#include "Listener.h"
#include "Session.h"
#include "Targets.h"
#include "Tool.h"
#include "ToolCatalog.h"
#include "Web.h"

namespace sdl3mcp {

class Handler : public AppPeerHandler {
  public:
   explicit Handler(HandlerOptions options);
   ~Handler() override;

   Handler(const Handler&) = delete;
   Handler& operator=(const Handler&) = delete;

   /* Runs until the client stream closes or a signal arrives; 0 on a clean end. */
   int run();

   /* AppPeerHandler */
   void onAppAttached(std::vector<nlohmann::json> tools) override;
   void onAppToolsChanged(std::vector<nlohmann::json> tools) override;
   void onAppDetached(std::string reason) override;
   void onAppContent(std::uint64_t requestId, nlohmann::json result) override;
   void onAppFrame(std::uint64_t requestId,
                   nlohmann::json header,
                   std::vector<std::uint8_t> payload) override;
   void onAppForwardFailure(std::uint64_t requestId, std::string error) override;
   void onAppError(std::uint64_t requestId, nlohmann::json error) override;

  private:
   struct PendingForward {
      std::string name;
      nlohmann::json arguments;
      ToolCompletionDelegate* completion = nullptr;
      std::chrono::steady_clock::time_point deadline;
   };

   struct PendingWait {
      ToolCompletionDelegate* completion = nullptr;
      std::chrono::steady_clock::time_point deadline;
      std::int64_t timeoutMs = 0;
   };

   /*
     One app_spawn_<alias> waiting for its application to connect and handshake.
     The exec channel is close-on-exec, so EOF is the success signal and any
     bytes are the reason the child could not exec. A web spawn has no exec
     channel: its pid is the browser's and it is only known once the page and
     bridge accept.
   */
   struct PendingSpawn {
      enum class Kind { Target,
                        Web };

      Kind kind = Kind::Target;
      std::string alias;
      pid_t pid = -1;
      int errorFd = -1;
      std::string execError;
      ToolCompletionDelegate* completion = nullptr;
      std::chrono::steady_clock::time_point deadline;
      std::int64_t timeoutMs = 0;
   };

   void buildBaseTools();
   void rebuildCatalog();
   std::vector<nlohmann::json> mergedTools() const;
   void publishToolsIfChanged(bool notify);

   /* Loads --targets and --web sources; a failure is fatal before anything is created. */
   bool loadSources(std::string& error);
   /* Prints the argv app_spawn_<alias> would run and starts nothing. */
   int printDryRun();

   bool startClient(std::string& error);
   bool startListener(std::string& error);
   void startSignalFd();
   void acceptApplication();

   void handleWaitSession(const ToolOrder& order,
                          ToolCompletionDelegate& completion);
   void handleSpawn(const ToolOrder& order,
                    ToolCompletionDelegate& completion,
                    const SpawnTarget& target);
   void handleWebSpawn(const ToolOrder& order,
                       ToolCompletionDelegate& completion,
                       const WebApp& app);
   void handleForward(const ToolOrder& order, ToolCompletionDelegate& completion);

   bool startTarget(const SpawnTarget& target,
                    std::string_view endpoint,
                    std::int64_t timeoutMs,
                    ToolCompletionDelegate& completion,
                    std::string& error);
   void serviceSpawnExec();
   void expireSpawn(std::chrono::steady_clock::time_point now);
   void completeSpawn(std::string text, bool isError);
   void cancelSpawn();

   /* Starts the browser when the tools accept; reports an ended stack. */
   void serviceWeb(std::chrono::steady_clock::time_point now,
                   const pollfd* webFds,
                   std::size_t webFdCount);
   void finishWebRun(WebStackEvent event);
   /* Stops the page server, bridge and browser, then removes the profile. */
   void stopWebRun();

   void completeWaiters(const std::string& text);
   void expireWaiters(std::chrono::steady_clock::time_point now);
   void expireForwards(std::chrono::steady_clock::time_point now);
   void failPendingForwards(const std::string& prefix, const std::string& suffix);
   void detachApp(std::string reason);

   bool peerAdvertises(std::string_view name) const;
   ToolResult presentFrame(const std::string& name,
                           const nlohmann::json& arguments,
                           const nlohmann::json& header,
                           std::vector<std::uint8_t> payload) const;

   int nextPollTimeoutMs(std::chrono::steady_clock::time_point now) const;
   void shutdown();

   HandlerOptions options_;
   ToolCatalog catalog_;
   std::vector<Tool> baseTools_;
   SpawnTargets targets_;
   WebApps webApps_;

   std::unique_ptr<Session> clientSession_;
   int clientFd_ = -1;

   Listener listener_;

   std::unique_ptr<AppPeer> appPeer_;
   int appFd_ = -1;
   bool appReady_ = false;
   std::vector<nlohmann::json> peerTools_;

   std::vector<PendingWait> waiters_;
   std::map<std::uint64_t, PendingForward> forwards_;
   std::optional<PendingSpawn> pendingSpawn_;
   std::unique_ptr<WebRun> webRun_;
   nlohmann::json publishedTools_;

   int signalFd_ = -1;
   bool running_ = true;
};

}  // namespace sdl3mcp
