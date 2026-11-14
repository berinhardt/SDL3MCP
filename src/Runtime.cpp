#include "Runtime.h"

#include <SDL3MCP/SDL3MCP1.h>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Mouse.h"
#include "SocketStream.h"
#include "Tool.h"
#include "ToolResult.h"

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

nlohmann::json parseDefinition(std::string_view text) {
   return nlohmann::json::parse(text.begin(), text.end());
}

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
      keyReplayer_(keyMap_, 0),
      mouseReplayer_(0, MouseMetrics{}) {
}

Runtime::~Runtime() = default;

bool Runtime::start(std::string& error) {
   buildCatalog();

   if (!configuration_.hasEndpoint()) {
      return true;
   }

   const SDL3MCP_Configuration& configuration = configuration_.value();
   auto stream = std::make_unique<SocketStream>();
   SocketStream* socket = stream.get();

   /* The session sets the stream's handler, so the dial can report onOpen. */
   auto session = std::make_unique<Session>(catalog_, std::move(stream));
   if (!socket->connect(configuration.connect_host, configuration.connect_port,
                        configuration.connect_timeout_ms, error)) {
      return false;
   }

   session_ = std::move(session);
   return true;
}

SDL_AppResult Runtime::setWindow(SDL_Window* window) {
   if (window == nullptr) {
      window_ = nullptr;
      windowID_ = 0;
      keyReplayer_.reset();
      keyReplayer_.setWindowID(0);
      mouseReplayer_.reset();
      mouseReplayer_.setWindowID(0);
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

   keyReplayer_.setWindowID(windowID);
   keyReplayer_.reset();

   mouseReplayer_.setWindowID(windowID);
   mouseReplayer_.setMetrics(
       MouseMetrics{static_cast<float>(info.width),
                    static_cast<float>(info.height), info.relativeMouseMode,
                    configuration_.value().warp_cursor});
   mouseReplayer_.reset();

   float mouseX = 0.0F;
   float mouseY = 0.0F;
   SDL_GetMouseState(&mouseX, &mouseY);
   mouseReplayer_.setCursor(mouseX, mouseY);

   return SDL_APP_CONTINUE;
}

SDL_AppResult Runtime::iterate() {
   /*
     The pump is the server loop: every available line is answered here, on the
     app thread, before the frame counter moves. Input and capture will register
     their requests in the same pass when the pipeline lands.
   */
   if (session_) {
      session_->pump();
   }

   ++frameCounter_;

   /*
     One quit request makes one SDL_EVENT_QUIT. A failed push restores the flag
     so the next iterate retries instead of losing the request.
   */
   if (terminateRequested_) {
      terminateRequested_ = false;
      SDL_Event event{};
      event.type = SDL_EVENT_QUIT;
      if (!SDL_PushEvent(&event)) {
         terminateRequested_ = true;
      }
   }

   return SDL_APP_CONTINUE;
}

void Runtime::shutdown() {
   releaseHeldInput();
   session_.reset();
   keyReplayer_.reset();
   mouseReplayer_.reset();
   window_ = nullptr;
   windowID_ = 0;
}

void Runtime::writeFrame(const std::uint8_t* data,
                         std::size_t size,
                         int width,
                         int height) noexcept {
   (void)data;
   (void)size;
   (void)width;
   (void)height;
}

void Runtime::buildCatalog() {
   catalog_.add(Tool{
       "sdl_info", std::string(kInfoDescription), parseDefinition(kInfoSchema),
       parseDefinition(kInfoAnnotations),
       [this](ToolOrder, ToolCompletionDelegate& completion) {
          completion.complete(ToolResult::text(infoToJson(infoSnapshot()).dump(2)));
       }});

   catalog_.add(Tool{
       "sdl_terminate", std::string(kTerminateDescription),
       parseDefinition(kTerminateSchema), parseDefinition(kTerminateAnnotations),
       [this](ToolOrder, ToolCompletionDelegate& completion) {
          terminateRequested_ = true;
          completion.complete(ToolResult::text("termination requested"));
       }});
}

Info Runtime::infoSnapshot() const {
   Info info;
   info.sdlVersion = formatSdlVersion();
   info.maxFrameBytes = configuration_.value().max_frame_bytes;
   info.frame = frameCounter_;
   info.appliedFrame = appliedFrame_;

   for (const SDL_Scancode scancode : keyReplayer_.heldKeys()) {
      const std::string_view name = keyMap_.canonicalName(scancode);
      if (!name.empty()) {
         info.heldKeys.emplace_back(name);
      }
   }
   for (const MouseButton button : mouseReplayer_.heldButtons()) {
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

void Runtime::releaseHeldInput() {
   std::vector<SDL_Event> events;
   const Uint64 timestamp = SDL_GetTicksNS();
   keyReplayer_.releaseAllKeys(timestamp, events);
   mouseReplayer_.releaseAllButtons(timestamp, events);

   if (windowID_ != 0) {
      for (SDL_Event& event : events) {
         SDL_PushEvent(&event);
      }
   }
   SDL_SetModState(keyReplayer_.modifiers());
}

}  // namespace sdl3mcp
