/*
  The metadata for the tools that take no arguments and answer no frame:
  sdl_info and sdl_terminate. The composition and the lookup are in
  ToolDefinitions.cpp; the frame-answering pair is ToolDefinitions-Input.cpp.
*/

#include "ToolDefinitions.h"

#include <string>
#include <string_view>

#include "ToolDefinitions-Schemas.h"

namespace sdl3mcp {
namespace {

constexpr std::string_view kInfoDescription =
    "Report the window the library is bound to (title, width and height in "
    "points, pixelWidth and pixelHeight in drawable pixels, pixelDensity, relative "
    "mouse mode), the keys and mouse buttons it currently holds, capture "
    "statistics, and the maximum frame size. Takes no arguments. Succeeds with a "
    "null window when no window is bound, so it is a useful first call. "
    "Example: {}";

constexpr std::string_view kInfoSchema = R"({
  "type": "object",
  "properties": {},
  "additionalProperties": false
})";

constexpr std::string_view kInfoAnnotations = R"({
  "readOnlyHint": true,
  "idempotentHint": true,
  "openWorldHint": false
})";

constexpr std::string_view kTerminateDescription =
    "Ask the application to exit cleanly: the next SDL3MCP_AppIterate() pushes "
    "one SDL_EVENT_QUIT so the app's own event loop can shut down. Takes no "
    "arguments. Example: {}";

constexpr std::string_view kTerminateSchema = R"({
  "type": "object",
  "properties": {},
  "additionalProperties": false
})";

constexpr std::string_view kTerminateAnnotations = R"({
  "readOnlyHint": false,
  "destructiveHint": true,
  "idempotentHint": false,
  "openWorldHint": false
})";

}  // namespace

ToolDefinition infoToolDefinition() {
   return ToolDefinition{"sdl_info", std::string(kInfoDescription),
                         parseDefinition(kInfoSchema),
                         parseDefinition(kInfoAnnotations)};
}

ToolDefinition terminateToolDefinition() {
   return ToolDefinition{"sdl_terminate", std::string(kTerminateDescription),
                         parseDefinition(kTerminateSchema),
                         parseDefinition(kTerminateAnnotations)};
}

}  // namespace sdl3mcp
