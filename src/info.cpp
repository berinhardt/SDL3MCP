#include "info.hpp"

namespace sdl3mcp
{

namespace
{

nlohmann::json stringOrNull(const std::string &value)
{
    if (value.empty()) {
        return nullptr;
    }
    return value;
}

nlohmann::json windowToJson(const WindowInfo &window)
{
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

nlohmann::json captureToJson(const CaptureInfo &capture)
{
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

} // namespace

nlohmann::json infoToJson(const InfoSnapshot &snapshot)
{
    return nlohmann::json{
        {"sdlVersion", snapshot.sdlVersion},
        {"videoDriver", stringOrNull(snapshot.videoDriver)},
        {"window", windowToJson(snapshot.window)},
        {"input", nlohmann::json{
             {"heldKeys", snapshot.heldKeys},
             {"heldButtons", snapshot.heldButtons},
         }},
        {"capture", captureToJson(snapshot.capture)},
        {"limits", nlohmann::json{
             {"frameTimeoutMs", snapshot.frameTimeoutMs},
             {"maxFrameBytes", snapshot.maxFrameBytes},
         }},
        {"frame", snapshot.frame},
        {"appliedFrame", snapshot.appliedFrame},
    };
}

} // namespace sdl3mcp
