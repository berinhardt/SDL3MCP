#pragma once

/*
  The four metadata records, declared here and owned by the family units:
  ToolDefinitions-Info.cpp carries sdl_info and sdl_terminate, the tools that
  take no arguments; ToolDefinitions-Input.cpp carries sdl_input and
  sdl_capture, the tools that answer with a frame. ToolDefinitions.cpp composes
  them in the advertised order and is the unit the rest of the tree includes.

  parseDefinition is inline here so every definer — the two family units and
  the Handler's own wait/session/spawn schemas — parses with one statement of
  the same policy instead of a second copy per unit.
*/

#include <nlohmann/json.hpp>
#include <string_view>

#include "ToolDefinitions.h"

namespace sdl3mcp {

inline nlohmann::json parseDefinition(std::string_view text) {
   return nlohmann::json::parse(text.begin(), text.end());
}

ToolDefinition infoToolDefinition();
ToolDefinition terminateToolDefinition();

ToolDefinition inputToolDefinition();
ToolDefinition captureToolDefinition();

}  // namespace sdl3mcp
