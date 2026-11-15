#include "Info.h"

namespace sdl3mcp {
namespace {

nlohmann::json stringOrNull(const std::string& value) {
   if (value.empty()) {
      return nullptr;
   }
   return value;
}

nlohmann::json windowToJson(const WindowInfo& window) {
   if (!window.bound) {
      return nullptr;
   }
   return nlohmann::json{
       {"title", window.title},
       {"width", window.width},
       {"height", window.height},
       {"pixelWidth", window.pixelWidth},
       {"pixelHeight", window.pixelHeight},
       {"pixelDensity", window.pixelDensity},
       {"relativeMouseMode", window.relativeMouseMode},
   };
}

nlohmann::json captureToJson(const CaptureInfo& capture) {
   return nlohmann::json{
       {"pending", capture.pending},
       {"count", capture.count},
       {"dropped", capture.dropped},
       {"lastWidth", capture.lastWidth},
       {"lastHeight", capture.lastHeight},
       {"lastBytes", capture.lastBytes},
       {"lastMs", capture.lastMs},
       {"lastError", stringOrNull(capture.lastError)},
   };
}

}  // namespace

nlohmann::json infoToJson(const Info& info) {
   return nlohmann::json{
       {"sdlVersion", info.sdlVersion},
       {"videoDriver", stringOrNull(info.videoDriver)},
       {"window", windowToJson(info.window)},
       {"input", nlohmann::json{
                     {"heldKeys", info.heldKeys},
                     {"heldButtons", info.heldButtons},
                 }},
       {"capture", captureToJson(info.capture)},
       {"limits", nlohmann::json{
                      {"maxFrameBytes", info.maxFrameBytes},
                  }},
       {"frame", info.frame},
       {"appliedFrame", info.appliedFrame},
   };
}

}  // namespace sdl3mcp
