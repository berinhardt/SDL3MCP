/*
  The four tools a bare application advertises, as metadata only.

  The library's Runtime pairs each entry with its SDL-backed invoke function;
  the Handler has no SDL, so it pairs the same metadata with a forward. Keeping
  the metadata here is what lets the Handler's base list be byte-identical to a
  bare application's list: attaching a bare app is then an identity on
  tools/list, and the two catalogs cannot drift.

  `sdl_input` and `sdl_capture` deliberately carry no timeout_ms: the library
  has no frame or capture timeout. The Handler adds its own forward-budget
  timeout_ms when it builds its base list.
*/

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp {

struct ToolDefinition {
   std::string name;
   std::string description;
   nlohmann::json inputSchema;
   nlohmann::json annotations;
};

/* The application catalog, in the library's advertised order. */
const std::vector<ToolDefinition>& applicationToolDefinitions();

const ToolDefinition* findApplicationToolDefinition(std::string_view name);

}  // namespace sdl3mcp
