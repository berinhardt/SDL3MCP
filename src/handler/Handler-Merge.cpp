#include "Handler.h"

#include <string>
#include <utility>
#include <vector>

namespace sdl3mcp {
namespace {

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

}  // namespace sdl3mcp
