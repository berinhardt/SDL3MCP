#include "Handler-Catalog.h"

#include <cstdio>
#include <string>
#include <utility>

#include "ToolArguments.h"
#include "ToolDefinitions-Schemas.h"
#include "ToolDefinitions.h"

namespace sdl3mcp {

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

}  // namespace sdl3mcp
