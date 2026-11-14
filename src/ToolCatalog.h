#pragma once

#include <map>
#include <string>
#include <string_view>

#include "Tool.h"

namespace sdl3mcp {

/*
  The name-to-tool map. A second add with the same name replaces the first, which
  is what lets the Handler merge an attached peer's catalog over its own base and
  keep the peer's definition on a collision.
*/
class ToolCatalog {
  public:
   void add(Tool tool);
   const Tool* find(std::string_view name) const;
   /* Empties the map so the Handler can rebuild the merged view in place. */
   void clear() noexcept { tools_.clear(); }

   const std::map<std::string, Tool, std::less<>>& tools() const noexcept {
      return tools_;
   }
   bool empty() const noexcept { return tools_.empty(); }
   size_t size() const noexcept { return tools_.size(); }

  private:
   std::map<std::string, Tool, std::less<>> tools_;
};

}  // namespace sdl3mcp
