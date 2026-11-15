/*
  The Runtime's state and lifecycle: construction, the dial, the window binding,
  shutdown, and the sdl_info snapshot. The frame loop is in Runtime-Iterate.cpp
  and the catalog construction in Runtime-Catalog.cpp.
*/

#include "Runtime.h"

#include <string>
#include <string_view>
#include <utility>

#include "SocketStream.h"

namespace sdl3mcp {
namespace {

/* "major.minor.micro" from the SDL the library was linked against. */
std::string formatSdlVersion() {
   const int version = SDL_GetVersion();
   return std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." +
          std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." +
          std::to_string(SDL_VERSIONNUM_MICRO(version));
}

/*
  App thread only. Reads every SDL field sdl_info reports for a bound window;
  false means SDL could not measure it and `info` is unusable.
*/
bool measureWindow(SDL_Window* window, WindowInfo& info) {
   info = WindowInfo{};
   info.bound = true;

   const char* title = SDL_GetWindowTitle(window);
   info.title = title != nullptr ? title : "";
   SDL_GetWindowSize(window, &info.width, &info.height);
   if (!SDL_GetWindowSizeInPixels(window, &info.pixelWidth, &info.pixelHeight)) {
      info.bound = false;
      return false;
   }
   info.pixelDensity = SDL_GetWindowPixelDensity(window);
   info.relativeMouseMode = SDL_GetWindowRelativeMouseMode(window);
   return true;
}

/* The wire name of a held button, matching the sdl_info example. */
std::string_view buttonName(MouseButton button) {
   switch (button) {
      case MouseButton::Left:
         return "left";
      case MouseButton::Middle:
         return "middle";
      case MouseButton::Right:
         return "right";
      case MouseButton::X1:
         return "x1";
      case MouseButton::X2:
         return "x2";
   }
   return {};
}

}  // namespace

Runtime::Runtime(Configuration configuration)
    : configuration_(std::move(configuration)),
      keyMap_(),
      pipeline_(keyMap_, configuration_.value().max_frame_bytes) {
}

Runtime::~Runtime() = default;

Result<void, TransportError> Runtime::start() {
   buildCatalog();

   if (!configuration_.hasEndpoint()) {
      return {};
   }

   const SDL3MCP_Configuration& configuration = configuration_.value();
   auto stream = std::make_unique<SocketStream>();
   SocketStream* socket = stream.get();

   /* The session sets the stream's handler, so the dial can report onOpen. */
   auto session = std::make_unique<Session>(catalog_, std::move(stream));
   const Result<void, TransportError> dial =
       socket->connect(configuration.connect_host, configuration.connect_port,
                       configuration.connect_timeout_ms);
   if (!dial) {
      return dial.error();
   }

   session_ = std::move(session);
   return {};
}

SDL_AppResult Runtime::setWindow(SDL_Window* window) {
   if (window == nullptr) {
      window_ = nullptr;
      windowID_ = 0;
      pipeline_.clearWindow();
      return SDL_APP_CONTINUE;
   }

   WindowInfo info;
   if (!measureWindow(window, info)) {
      SDL_SetError("SDL3MCP_SetWindow cannot measure the window: %s",
                   SDL_GetError());
      return SDL_APP_FAILURE;
   }
   if (info.pixelWidth <= 0 || info.pixelHeight <= 0) {
      SDL_SetError("SDL3MCP_SetWindow needs a drawable window, got %dx%d",
                   info.pixelWidth, info.pixelHeight);
      return SDL_APP_FAILURE;
   }

   /* A rebind starts from clean input state: nothing held crosses windows. */
   const SDL_WindowID windowID = SDL_GetWindowID(window);
   window_ = window;
   windowID_ = windowID;

   MouseMetrics metrics;
   metrics.width = static_cast<float>(info.width);
   metrics.height = static_cast<float>(info.height);
   metrics.relativeMode = info.relativeMouseMode;
   metrics.warpCursor = configuration_.value().warp_cursor;
   pipeline_.setWindow(window, windowID, metrics);

   return SDL_APP_CONTINUE;
}

void Runtime::shutdown() {
   pipeline_.releaseHeldInput();
   pipeline_.discardRequests();
   session_.reset();
   pipeline_.clearWindow();
   window_ = nullptr;
   windowID_ = 0;
}

void Runtime::writeFrame(const std::uint8_t* data,
                         std::size_t size,
                         int width,
                         int height) noexcept {
   pipeline_.writeFrame(data, size, width, height, frameCounter_ + 1);
}

Info Runtime::infoSnapshot() const {
   Info info;
   info.sdlVersion = formatSdlVersion();
   info.maxFrameBytes = configuration_.value().max_frame_bytes;
   info.frame = frameCounter_;
   info.appliedFrame = pipeline_.appliedFrame();
   info.capture = pipeline_.captureInfo();

   for (const SDL_Scancode scancode : pipeline_.heldKeys()) {
      const std::string_view name = keyMap_.canonicalName(scancode);
      if (!name.empty()) {
         info.heldKeys.emplace_back(name);
      }
   }
   for (const MouseButton button : pipeline_.heldButtons()) {
      info.heldButtons.emplace_back(buttonName(button));
   }

   if (window_ != nullptr) {
      WindowInfo window;
      if (measureWindow(window_, window)) {
         info.window = std::move(window);
         const char* driver = SDL_GetCurrentVideoDriver();
         info.videoDriver = driver != nullptr ? driver : "";
      }
   }

   return info;
}

}  // namespace sdl3mcp
