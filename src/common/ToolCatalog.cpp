#include "ToolCatalog.h"

#include <utility>

namespace sdl3mcp {

void ToolCatalog::add(Tool tool) {
   const std::string name = tool.name;
   tools_.insert_or_assign(name, std::move(tool));
}

const Tool* ToolCatalog::find(std::string_view name) const {
   const auto entry = tools_.find(name);
   return entry == tools_.end() ? nullptr : &entry->second;
}

}  // namespace sdl3mcp
