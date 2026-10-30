/*
  What sdl_info reports: a value copy of the app-thread state plus a pure
  serializer.

  The snapshot exists because the server thread may not call SDL. The app thread
  fills the SDL-derived fields (version, driver, window geometry) and publishes a
  copy under its own mutex; the handler copies that value again and serializes it
  with no SDL and no locks. Keeping infoToJson() free of SDL is also what makes
  the wire shape testable without a window.

  Fields that have no state yet stay at their defaults: held keys and buttons are
  empty, capture is pending=false with zero statistics, and appliedFrame is 0
  until the input and capture wiring lands.
*/

#ifndef SDL3MCP_INFO_HPP
#define SDL3MCP_INFO_HPP

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp
{

/*
  Geometry follows SDL's two size spaces: width/height are window points, the
  space mouse coordinates use, while pixelWidth/pixelHeight are drawable pixels.
  pixelDensity is their ratio. Capture reports the dimensions the app wrote in
  capture.lastWidth/lastHeight, which may differ from the window's pixels.
*/
struct WindowInfo
{
    bool bound = false;
    std::string title;
    int width = 0;
    int height = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    float pixelDensity = 0;
    bool relativeMouseMode = false;
};

struct CaptureInfo
{
    bool pending = false;
    uint64_t count = 0;
    uint64_t dropped = 0;
    /* Frame dimensions after the region crop, and its raw RGBA byte count. */
    int lastWidth = 0;
    int lastHeight = 0;
    uint64_t lastBytes = 0;
    /* Wait for the frame plus the region crop. */
    double lastMs = 0;
    std::string lastError;
};

struct InfoSnapshot
{
    std::string sdlVersion;
    std::string videoDriver;
    WindowInfo window;
    std::vector<std::string> heldKeys;
    std::vector<std::string> heldButtons;
    CaptureInfo capture;
    uint32_t frameTimeoutMs = 0;
    uint32_t maxFrameBytes = 0;
    uint64_t frame = 0;
    uint64_t appliedFrame = 0;
};

/*
  The sdl_info content as JSON. `window` is null when nothing is bound,
  `videoDriver` is null when empty, and `capture.lastError` is null when empty,
  so a client never has to distinguish "missing" from "empty string".
*/
nlohmann::json infoToJson(const InfoSnapshot &snapshot);

} // namespace sdl3mcp

#endif /* SDL3MCP_INFO_HPP */
