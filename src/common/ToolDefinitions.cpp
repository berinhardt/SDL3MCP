/*
  The application catalog: the four records in the library's advertised order,
  and the lookup the Handler and the Runtime both use. The records themselves
  are split by tool family; see ToolDefinitions-Schemas.h.
*/

#include "ToolDefinitions.h"

#include <string_view>

#include "ToolDefinitions-Schemas.h"

namespace sdl3mcp {

const std::vector<ToolDefinition>& applicationToolDefinitions() {
   static const std::vector<ToolDefinition> definitions = {
       infoToolDefinition(),
       inputToolDefinition(),
       captureToolDefinition(),
       terminateToolDefinition(),
   };
   return definitions;
}

const ToolDefinition* findApplicationToolDefinition(std::string_view name) {
   for (const ToolDefinition& definition : applicationToolDefinitions()) {
      if (definition.name == name) {
         return &definition;
      }
   }
   return nullptr;
}

}  // namespace sdl3mcp
