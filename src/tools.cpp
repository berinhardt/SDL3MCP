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
    "Report the window the library is bound to (title, width and height in "
    "points, pixelWidth and pixelHeight in drawable pixels, pixelDensity, relative "
    "mouse mode), the keys and mouse buttons it currently holds, capture "
    "statistics, and the frame timeout and payload limits. Takes no arguments. "
    "Succeeds with a null window when no window is bound, so it is a useful first "
    "call. Example: {}";

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
    "Queue a batch of synthetic input actions; the call always returns the frame "
    "that follows the batch: text naming the applied frame plus that frame's "
    "pixels, which the supervisor presents as an image/png part and a direct "
    "library client receives as raw RGBA. The batch is applied atomically at the "
    "next frame and the call returns after the following frame, once the "
    "application has consumed the generated events. Arguments: actions (required; "
    "1-256 action objects), optional region, and optional timeout_ms. Each action "
    "object is one of six types: key {key name or chord, action=down|up|press "
    "(default press), modifiers=[ctrl|shift|alt|gui] (at most four)}; text {text, "
    "1-1024 UTF-8 bytes}; mouse_move {x,y in window points or dx,dy relative, "
    "never both}; mouse_button {button=left|middle|right|x1|x2, "
    "action=down|up|click (default click), clicks=1-3 (default 1), optional x,y "
    "window points to move first}; scroll {y in notches (1.0 is one notch, "
    "positive scrolls down), x default 0}; release_all {no extra fields}. The "
    "optional region crops the returned frame in pixels of the full frame with a "
    "top-left origin, exactly like sdl_capture; sdl_input coordinates are window "
    "points, so use pixelDensity from sdl_info to convert. timeout_ms bounds the "
    "whole call, apply and capture together, and defaults to twice the frame "
    "timeout. "
    "Example: "
    "{\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"},"
    "{\"type\":\"mouse_button\",\"button\":\"left\",\"x\":120,\"y\":80}]}";

/*
  A key action has no repeat property: repeat a press by sending another action
  instead, which keeps the batch a plain ordered list of single events. The
  region and timeout_ms members are the same objects sdl_capture takes, parsed by
  the same helpers, so the schemas cannot drift from the accepted documents.
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
            "description": "Press, release, or click a mouse button at the cursor, or at optional window points (x, y) that move the cursor there first.",
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
    },
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
  "required": ["actions"],
  "additionalProperties": false
})";

constexpr std::string_view kInputAnnotations = R"({
  "readOnlyHint": false,
  "idempotentHint": false,
  "openWorldHint": false
})";

/*
  The region is in pixels of the captured frame, matching sdl_info's
  capture.lastWidth and capture.lastHeight, so the crop is exact. Image
  encoding is the supervisor's policy; a direct library client receives raw
  RGBA.
*/
constexpr std::string_view kCaptureDescription =
    "Ask the application for a fresh frame through SDL3MCP_FrameRequested() and "
    "return its raw pixels. Arguments: optional timeout_ms (1-60000, default "
    "frame_timeout_ms) and optional region. The result is one JSON-RPC line "
    "{\"format\":\"rgba8888\",\"width\":W,\"height\":H,\"byteLength\":B}, "
    "immediately followed by exactly B tightly packed RGBA bytes with no newline "
    "and no base64. region is a crop rectangle in pixels of the full frame with a "
    "top-left origin and must fit inside capture.lastWidth x capture.lastHeight "
    "from sdl_info. sdl_input coordinates are window points, not frame pixels: "
    "sdl_info reports pixelWidth, pixelHeight and pixelDensity so a caller can "
    "convert between the two. Example: "
    "{\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360},"
    "\"timeout_ms\":2000}";

constexpr std::string_view kCaptureSchema = R"({
  "type": "object",
  "properties": {
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
