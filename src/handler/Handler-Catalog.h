/*
  The Handler's tool text: the schemas, annotations and descriptions
  buildBaseTools assembles for wait_session and the spawn tools, and the
  Handler's own text for sdl_input and sdl_capture. It is data, so it
  sits beside the merge code rather than inside it.
*/

#pragma once

#include <string>
#include <string_view>

#include "Handler.h"

namespace sdl3mcp {

constexpr std::string_view kSpawnSchema = R"({
  "type": "object",
  "properties": {
    "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
  },
  "additionalProperties": false
})";

constexpr std::string_view kSpawnAnnotations = R"({
  "readOnlyHint": false,
  "idempotentHint": false,
  "openWorldHint": true
})";

inline std::string spawnDescription(std::string_view alias) {
   return "Start the application configured for the alias " + std::string(alias) +
          " and wait for it to connect and complete the MCP handshake. The "
          "application is started with --mcp=<the Handler's listener endpoint> "
          "appended to its configured argv. One application at a time: a call "
          "while one is attached or starting fails. Arguments: optional timeout_ms "
          "(1-60000, default the Handler's --timeout-ms). Returns "
          "`spawned <alias> (pid N)`; a timeout or exec failure names the reason "
          "and the pid, and the process is not stopped (spawn-and-forget). "
          "Example: {\"timeout_ms\":5000}";
}

inline std::string webSpawnDescription(std::string_view alias) {
   return "Start the web application configured for the alias " +
          std::string(alias) +
          ": a page server, a WebSocket bridge and an isolated Firefox, then "
          "wait for the browser application to connect to the bridge and "
          "complete the MCP handshake. The page URL, bridge endpoint and "
          "throwaway profile are printed on stderr. One application at a time: "
          "a call while one is attached or starting fails. Arguments: optional "
          "timeout_ms (1-60000, default the Handler's --timeout-ms) covering the "
          "launch and the handshake. Returns `spawned <alias> (browser pid N)`; "
          "the Handler owns the page server, bridge and browser, so a timeout or "
          "shutdown stops all three and removes the profile. "
          "Example: {\"timeout_ms\":5000}";
}

constexpr std::string_view kWaitDescription =
    "Wait for an application to connect to the listener. Arguments: optional "
    "timeout_ms (1-60000, default the Handler's --timeout-ms). Returns as soon as "
    "a session is attached; an expired timeout is a normal tool error. The "
    "application's tools are advertised alongside this one whether or not it is "
    "attached. Example: {\"timeout_ms\":5000}";

constexpr std::string_view kWaitSchema = R"({
  "type": "object",
  "properties": {
    "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
  },
  "additionalProperties": false
})";

constexpr std::string_view kWaitAnnotations = R"({
  "readOnlyHint": true,
  "idempotentHint": false,
  "openWorldHint": true
})";

constexpr std::string_view kHandlerInputDescription =
    "Queue a batch of synthetic input actions; the call returns the frame that "
    "follows the batch: text naming both the frame the actions were applied at "
    "and the frame attached, plus that frame's pixels, presented as an image/png "
    "part. The batch is applied atomically at the next frame and the call returns "
    "after the following frame, once the application has consumed the generated "
    "events. Arguments: actions (required; 1-256 action objects), optional region, "
    "and optional timeout_ms (1-60000, default the Handler's --timeout-ms) "
    "bounding the whole call. Each action object is one of six types: key {key "
    "name or chord, action=down|up|press (default press), "
    "modifiers=[ctrl|shift|alt|gui]}; text {text, 1-1024 UTF-8 bytes}; mouse_move "
    "{x,y in window points or dx,dy relative}; mouse_button "
    "{button=left|middle|right|x1|x2, action=down|up|click (default click), "
    "clicks=1-3, optional x,y}; scroll {y in notches, x default 0}; release_all. "
    "The optional region crops the returned frame in pixels of the full frame "
    "with a top-left origin. Example: "
    "{\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"}],"
    "\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}";

constexpr std::string_view kHandlerCaptureDescription =
    "Ask the application for a fresh frame and return it as a PNG image block at "
    "the app's native pixel size. Arguments: optional timeout_ms (1-60000, "
    "default the Handler's --timeout-ms) and optional region. region crops pixels "
    "from the captured frame with a top-left origin and must fit inside "
    "capture.lastWidth x capture.lastHeight from sdl_info. sdl_input coordinates "
    "are window points, while region is in pixels of the full frame; sdl_info "
    "reports pixelWidth, pixelHeight and pixelDensity so a caller can convert "
    "between them. Captures are never resized, so region is the only way to keep "
    "the payload small. The text part names the frame attached, so a caller can "
    "tell a fresh frame from a stale one. Example: "
    "{\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360},"
    "\"timeout_ms\":2000}";

}  // namespace sdl3mcp
