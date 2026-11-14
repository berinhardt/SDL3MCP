#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sdl3mcp {

/*
  What sdl_info reports, as a value with a pure serializer.

  1.0 answers the tool on the app thread, so this is not a cross-thread snapshot:
  it is the live state read at call time. Keeping it a plain value with no SDL in
  infoToJson() is what makes the wire shape testable without a window.

  There is no frame timeout in 1.0, so `limits` carries only maxFrameBytes. The
  capture fields stay at their defaults until the pipeline ticket fills them.
*/
struct WindowInfo {
   bool bound = false;
   std::string title;
   int width = 0;
   int height = 0;
   int pixelWidth = 0;
   int pixelHeight = 0;
   float pixelDensity = 0.0F;
   bool relativeMouseMode = false;
};

struct CaptureInfo {
   bool pending = false;
   std::uint64_t count = 0;
   std::uint64_t dropped = 0;
   /* Frame dimensions after the region crop, and its raw RGBA byte count. */
   int lastWidth = 0;
   int lastHeight = 0;
   std::uint64_t lastBytes = 0;
   /* Wait for the frame plus the region crop. */
   double lastMs = 0.0;
   std::string lastError;
};

struct Info {
   std::string sdlVersion;
   std::string videoDriver;
   WindowInfo window;
   std::vector<std::string> heldKeys;
   std::vector<std::string> heldButtons;
   CaptureInfo capture;
   std::uint32_t maxFrameBytes = 0;
   std::uint64_t frame = 0;
   std::uint64_t appliedFrame = 0;
};

/*
  The sdl_info content as JSON. `window` is null when nothing is bound,
  `videoDriver` is null when empty, and `capture.lastError` is null when empty,
  so a client never has to distinguish "missing" from "empty string".
*/
nlohmann::json infoToJson(const Info& info);

}  // namespace sdl3mcp
