#include "Handler-Catalog.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "Configuration.h"
#include "SocketStream.h"
#include "ToolArguments.h"
#include "ToolDefinitions.h"

namespace sdl3mcp {
namespace {

nlohmann::json parseDefinition(std::string_view text) {
   return nlohmann::json::parse(text.begin(), text.end());
}

nlohmann::json objectField(const nlohmann::json& entry,
                           const char* name,
                           nlohmann::json fallback) {
   const auto value = entry.find(name);
   if (value == entry.end() || !value->is_object()) {
      return fallback;
   }
   return *value;
}

}  // namespace

Result<void, ProtocolError> Handler::loadSources() {
   targets_.clear();
   webApps_.clear();

   if (!options_.targetsPaths.empty()) {
      Result<SpawnTargets, ProtocolError> targets =
          loadSpawnTargets(options_.targetsPaths);
      if (!targets) {
         return targets.error();
      }
      targets_ = std::move(*targets);
   }
   if (!options_.webPaths.empty()) {
      Result<WebApps, ProtocolError> apps = loadWebApps(options_.webPaths);
      if (!apps) {
         return apps.error();
      }
      webApps_ = std::move(*apps);
   }

   /*
     One alias map. A name defined by both sources would make one of the two
     entries unreachable, so it is a startup error naming both files.
   */
   for (const WebApp& web : webApps_) {
      for (const SpawnTarget& target : targets_) {
         if (target.alias == web.alias) {
            return ProtocolError{"alias \"" + web.alias +
                                 "\" is defined in both \"" + target.source +
                                 "\" and \"" + web.source + "\""};
         }
      }
   }

   return {};
}

int Handler::printToolCatalog() {
   buildBaseTools();
   rebuildCatalog();

   const nlohmann::json catalog = mergedTools();
   std::printf("%s\n", catalog.dump().c_str());
   std::fflush(stdout);
   return 0;
}

void Handler::acceptApplication() {
   const Result<std::optional<int>, TransportError> accepted =
       listener_.accept();
   if (!accepted) {
      std::fprintf(stderr, "sdl3mcp: %s\n", accepted.error().message.c_str());
      std::fflush(stderr);
      return;
   }
   if (!accepted->has_value()) {
      return;
   }
   const int fd = **accepted;

   /*
     Exactly one application session. The listener keeps accepting only so a
     second connection is closed at once instead of waiting in the backlog.
   */
   if (appPeer_ != nullptr) {
      ::close(fd);
      return;
   }

   auto socket = std::make_unique<SocketStream>();
   const Result<void, TransportError> adopted = socket->adopt(
       fd, endpointText(options_.listenHost, listener_.boundPort()));
   if (!adopted) {
      std::fprintf(stderr, "sdl3mcp: %s\n", adopted.error().message.c_str());
      std::fflush(stderr);
      return;
   }

   auto peer = std::make_unique<AppPeer>(
       std::move(socket), *this, static_cast<std::uint32_t>(options_.timeoutMs));
   const Result<void, TransportError> started = peer->start();
   if (!started) {
      std::fprintf(stderr, "sdl3mcp: %s\n", started.error().message.c_str());
      std::fflush(stderr);
      return;
   }

   appPeer_ = std::move(peer);
   appFd_ = fd;
}

void Handler::buildBaseTools() {
   baseTools_.clear();

   baseTools_.push_back(Tool{
       "wait_session", std::string(kWaitDescription),
       parseDefinition(kWaitSchema), parseDefinition(kWaitAnnotations),
       [this](ToolOrder order, ToolCompletionDelegate& completion) {
          handleWaitSession(order, completion);
       }});

   for (const ToolDefinition& definition : applicationToolDefinitions()) {
      ToolDefinition handlerDefinition = definition;
      if (definition.name == "sdl_input" || definition.name == "sdl_capture") {
         handlerDefinition.description =
             definition.name == "sdl_input" ? std::string(kHandlerInputDescription)
                                            : std::string(kHandlerCaptureDescription);
         handlerDefinition.inputSchema["properties"]["timeout_ms"] =
             nlohmann::json{{"type", "integer"},
                            {"minimum", 1},
                            {"maximum", kMaxTimeoutMs}};
      }

      baseTools_.push_back(Tool{
          handlerDefinition.name, handlerDefinition.description,
          handlerDefinition.inputSchema, handlerDefinition.annotations,
          [this](ToolOrder order, ToolCompletionDelegate& completion) {
             handleForward(order, completion);
          }});
   }

   /* Each app source contributes one spawn tool, whether or not an app is up. */
   for (const SpawnTarget& target : targets_) {
      const SpawnTarget spawnTarget = target;
      baseTools_.push_back(Tool{
          "app_spawn_" + target.alias, spawnDescription(target.alias),
          parseDefinition(kSpawnSchema), parseDefinition(kSpawnAnnotations),
          [this, spawnTarget](ToolOrder order, ToolCompletionDelegate& completion) {
             handleSpawn(order, completion, spawnTarget);
          }});
   }
   for (const WebApp& app : webApps_) {
      const WebApp webApp = app;
      baseTools_.push_back(Tool{
          "app_spawn_" + app.alias, webSpawnDescription(app.alias),
          parseDefinition(kSpawnSchema), parseDefinition(kSpawnAnnotations),
          [this, webApp](ToolOrder order, ToolCompletionDelegate& completion) {
             handleWebSpawn(order, completion, webApp);
          }});
   }
}

void Handler::rebuildCatalog() {
   catalog_.clear();

   for (const Tool& tool : baseTools_) {
      /*
        sdl_capture and sdl_input carry the Handler's forward budget, so their
        definitions stay the Handler's. The other base entries take the peer's
        metadata but keep the Handler's forward invoke.
      */
      if (tool.name == "sdl_capture" || tool.name == "sdl_input") {
         catalog_.add(tool);
         continue;
      }

      const nlohmann::json* peer = nullptr;
      for (const nlohmann::json& entry : peerTools_) {
         if (entry.is_object() &&
             entry.value("name", std::string()) == tool.name) {
            peer = &entry;
            break;
         }
      }
      if (peer == nullptr) {
         catalog_.add(tool);
         continue;
      }

      Tool merged = tool;
      if (peer->contains("description") && (*peer)["description"].is_string()) {
         merged.description = (*peer)["description"].get<std::string>();
      }
      merged.inputSchema = objectField(*peer, "inputSchema", tool.inputSchema);
      merged.annotations = objectField(*peer, "annotations", tool.annotations);
      catalog_.add(std::move(merged));
   }

   /* A name only the peer advertises is appended and forwarded like the rest. */
   for (const nlohmann::json& entry : peerTools_) {
      if (!entry.is_object() || !entry.contains("name") ||
          !entry["name"].is_string()) {
         continue;
      }
      const std::string name = entry["name"].get<std::string>();
      if (name.empty() || catalog_.find(name) != nullptr) {
         continue;
      }
      catalog_.add(Tool{
          name, entry.value("description", std::string()),
          objectField(entry, "inputSchema", nlohmann::json::object()),
          objectField(entry, "annotations", nlohmann::json::object()),
          [this](ToolOrder order, ToolCompletionDelegate& completion) {
             handleForward(order, completion);
          }});
   }
}

std::vector<nlohmann::json> Handler::mergedTools() const {
   std::vector<nlohmann::json> tools;
   for (const auto& entry : catalog_.tools()) {
      const Tool& tool = entry.second;
      tools.push_back(nlohmann::json{
          {"name", tool.name},
          {"description", tool.description},
          {"inputSchema", tool.inputSchema},
          {"annotations", tool.annotations},
      });
   }
   return tools;
}

void Handler::publishToolsIfChanged(bool notify) {
   nlohmann::json current = nlohmann::json::array();
   for (nlohmann::json& tool : mergedTools()) {
      current.push_back(std::move(tool));
   }

   const bool changed = current != publishedTools_;
   publishedTools_ = std::move(current);
   if (!notify || !changed || clientSession_ == nullptr) {
      return;
   }
   clientSession_->stream().sendText(
       nlohmann::json{{"jsonrpc", "2.0"},
                      {"method", "notifications/tools/list_changed"}}
           .dump());
}

void Handler::onAppAttached(std::vector<nlohmann::json> tools) {
   peerTools_ = std::move(tools);
   appReady_ = true;
   rebuildCatalog();
   /* Attach is silent: a bare app is an identity, and a spawn changes no name. */
   publishToolsIfChanged(false);
   completeWaiters("an application session attached");

   if (pendingSpawn_.has_value()) {
      if (pendingSpawn_->isWeb() &&
          pendingSpawn_->pid < 0 && webRun_) {
         pendingSpawn_->pid = webRun_->browserPid();
      }
      const bool isWeb = pendingSpawn_->isWeb();
      completeSpawn("spawned " + pendingSpawn_->alias + " (" +
                        (isWeb ? "browser pid " : "pid ") +
                        std::to_string(pendingSpawn_->pid) + ")",
                    false);
   }
}

void Handler::onAppToolsChanged(std::vector<nlohmann::json> tools) {
   peerTools_ = std::move(tools);
   rebuildCatalog();
   publishToolsIfChanged(true);
}

void Handler::onAppDetached(std::string reason) {
   appReady_ = false;
   peerTools_.clear();
   rebuildCatalog();
   publishToolsIfChanged(false);
   failPendingForwards(reason + " during ", "");
}

}  // namespace sdl3mcp
