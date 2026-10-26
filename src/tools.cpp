#include "tools.hpp"

#include <utility>

namespace sdl3mcp
{
namespace
{

nlohmann::json parseDefinition(std::string_view text)
{
    return nlohmann::json::parse(text.begin(), text.end());
}

constexpr std::string_view kInfoDescription =
    "Report the window the library is bound to (title, size in points and pixels, "
    "pixel density, relative mouse mode), the keys and mouse buttons it currently "
    "holds, capture statistics, and the frame timeout and payload limits. Succeeds "
    "with a null window when no window is bound, so it is a useful first call.";

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

constexpr std::string_view kInputDescription =
    "Queue a batch of synthetic key, text, mouse, scroll, or release-all actions. "
    "The batch is applied atomically at the next frame, and the call returns after "
    "the following frame, once the application has consumed the generated events. "
    "Follow with sdl_capture to observe the result.";

/*
  A key action has no repeat property: repeat a press by sending another action
  instead, which keeps the batch a plain ordered list of single events.
*/
constexpr std::string_view kInputSchema = R"({
  "type": "object",
  "properties": {
    "actions": {
      "type": "array",
      "minItems": 1,
      "maxItems": 256,
      "items": {
        "oneOf": [
          {
            "type": "object",
            "description": "Press, hold, or release one key, optionally with modifiers.",
            "properties": {
              "type": { "const": "key" },
              "key": {
                "type": "string",
                "description": "Key name, or a modifier+modifier+key chord for press, for example \"ctrl+s\"."
              },
              "action": { "enum": ["down", "up", "press"], "default": "press" },
              "modifiers": {
                "type": "array",
                "maxItems": 4,
                "items": { "enum": ["ctrl", "shift", "alt", "gui"] }
              }
            },
            "required": ["type", "key"],
            "additionalProperties": false
          },
          {
            "type": "object",
            "description": "Type text as SDL text input.",
            "properties": {
              "type": { "const": "text" },
              "text": { "type": "string", "minLength": 1, "maxLength": 1024 }
            },
            "required": ["type", "text"],
            "additionalProperties": false
          },
          {
            "type": "object",
            "description": "Move the cursor to absolute window points (x, y) or by relative deltas (dx, dy).",
            "properties": {
              "type": { "const": "mouse_move" },
              "x": { "type": "number" },
              "y": { "type": "number" },
              "dx": { "type": "number" },
              "dy": { "type": "number" }
            },
            "required": ["type"],
            "additionalProperties": false
          },
          {
            "type": "object",
            "description": "Press, release, or click a mouse button at the cursor or at optional window points (x, y).",
            "properties": {
              "type": { "const": "mouse_button" },
              "button": { "enum": ["left", "middle", "right", "x1", "x2"] },
              "action": { "enum": ["down", "up", "click"], "default": "click" },
              "x": { "type": "number" },
              "y": { "type": "number" },
              "clicks": { "type": "integer", "minimum": 1, "maximum": 3, "default": 1 }
            },
            "required": ["type", "button"],
            "additionalProperties": false
          },
          {
            "type": "object",
            "description": "Scroll the wheel; 1.0 is one notch and positive y scrolls down.",
            "properties": {
              "type": { "const": "scroll" },
              "x": { "type": "number", "default": 0 },
              "y": { "type": "number" }
            },
            "required": ["type", "y"],
            "additionalProperties": false
          },
          {
            "type": "object",
            "description": "Release every key and mouse button the library holds.",
            "properties": {
              "type": { "const": "release_all" }
            },
            "required": ["type"],
            "additionalProperties": false
          }
        ]
      }
    }
  },
  "required": ["actions"],
  "additionalProperties": false
})";

constexpr std::string_view kInputAnnotations = R"({
  "readOnlyHint": false,
  "idempotentHint": false,
  "openWorldHint": false
})";

/*
  The region is in full-frame pixels, matching sdl_info's pixelWidth and
  pixelHeight, so the crop is exact and happens before any downscale.
*/
constexpr std::string_view kCaptureDescription =
    "Ask the application for a fresh frame through SDL3MCP_FrameRequested() and "
    "return it as a PNG image block. The optional region crops pixels from the "
    "full frame, using pixelWidth x pixelHeight from sdl_info, with a top-left "
    "origin; it must fit inside the frame and is applied before max_width "
    "downscaling. Use max_width to keep the payload small.";

constexpr std::string_view kCaptureSchema = R"({
  "type": "object",
  "properties": {
    "max_width": { "type": "integer", "minimum": 16, "maximum": 16384 },
    "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 },
    "region": {
      "type": "object",
      "description": "Crop rectangle in pixels of the full frame, top-left origin.",
      "properties": {
        "x": { "type": "integer", "minimum": 0 },
        "y": { "type": "integer", "minimum": 0 },
        "width": { "type": "integer", "minimum": 1 },
        "height": { "type": "integer", "minimum": 1 }
      },
      "required": ["x", "y", "width", "height"],
      "additionalProperties": false
    }
  },
  "additionalProperties": false
})";

constexpr std::string_view kCaptureAnnotations = R"({
  "readOnlyHint": true,
  "idempotentHint": false,
  "openWorldHint": false
})";

constexpr std::string_view kTerminateDescription =
    "Ask the application to exit cleanly: the next SDL3MCP_AppIterate() pushes "
    "one SDL_EVENT_QUIT so the app's own event loop can shut down.";

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

std::vector<ToolDefinition> buildCatalog()
{
    std::vector<ToolDefinition> tools;
    tools.reserve(4);

    tools.push_back({"sdl_info", kInfoDescription, parseDefinition(kInfoSchema),
                     parseDefinition(kInfoAnnotations)});
    tools.push_back({"sdl_input", kInputDescription, parseDefinition(kInputSchema),
                     parseDefinition(kInputAnnotations)});
    tools.push_back({"sdl_capture", kCaptureDescription, parseDefinition(kCaptureSchema),
                     parseDefinition(kCaptureAnnotations)});
    tools.push_back({"sdl_terminate", kTerminateDescription,
                     parseDefinition(kTerminateSchema),
                     parseDefinition(kTerminateAnnotations)});

    return tools;
}

} // namespace

const std::vector<ToolDefinition> &toolCatalog()
{
    static const std::vector<ToolDefinition> catalog = buildCatalog();
    return catalog;
}

const ToolDefinition *findTool(std::string_view name)
{
    for (const ToolDefinition &tool : toolCatalog()) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

} // namespace sdl3mcp
