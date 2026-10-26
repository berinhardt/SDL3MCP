/*
  The internal tool catalog: metadata only, no execution.

  Every definition is built once from static JSON so the wire schemas are data,
  not code, and tools/list can hand them out unchanged. The catalog is read-only
  after construction and safe to read from any thread, which is why the
  definitions are string_view/json pairs rather than mutable objects.

  The schemas are the settled contract, including the parts a naive reading of
  the prose would miss: a key action has no `repeat` property, scroll units are
  notches (1.0 is one notch), the capture region is expressed in full-frame
  pixels, and mouse_move accepts an absolute (x, y) or a relative (dx, dy) pair.
  Tool execution lives elsewhere; this file only answers "what tools exist and
  what do they accept".
*/

#ifndef SDL3MCP_TOOLS_HPP
#define SDL3MCP_TOOLS_HPP

#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace sdl3mcp
{

struct ToolDefinition
{
    std::string_view name;
    std::string_view description;
    nlohmann::json inputSchema;
    nlohmann::json annotations;
};

/* Built on first use and never mutated afterwards. */
const std::vector<ToolDefinition> &toolCatalog();

/* Returns the definition for `name`, or nullptr when no such tool exists. */
const ToolDefinition *findTool(std::string_view name);

} // namespace sdl3mcp

#endif /* SDL3MCP_TOOLS_HPP */
