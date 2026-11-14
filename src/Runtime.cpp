#include "Runtime.h"

#include <SDL3MCP/SDL3MCP1.h>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Capture.h"
#include "Input.h"
#include "InputJson.h"
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

constexpr std::string_view kInputDescription =
    "Queue a batch of synthetic input actions; the call returns the frame that "
    "follows the batch: text naming both the frame the actions were applied at "
    "and the frame attached, plus that frame's pixels, which the Handler presents "
    "as an image/png part and a direct library client receives as raw RGBA. The "
    "batch is applied atomically at the next frame and the call returns after the "
    "following frame, once the application has consumed the generated events. "
    "Arguments: actions (required; 1-256 action objects) and optional region. "
    "Each action object is one of six types: key {key name or chord, "
    "action=down|up|press (default press), modifiers=[ctrl|shift|alt|gui] (at most "
    "four)}; text {text, 1-1024 UTF-8 bytes}; mouse_move {x,y in window points or "
    "dx,dy relative, never both}; mouse_button {button=left|middle|right|x1|x2, "
    "action=down|up|click (default click), clicks=1-3 (default 1), optional x,y "
    "window points to move first}; scroll {y in notches (1.0 is one notch, "
    "positive scrolls down), x default 0}; release_all {no extra fields}. The "
    "optional region crops the returned frame in pixels of the full frame with a "
    "top-left origin, exactly like sdl_capture; sdl_input coordinates are window "
    "points, so use pixelDensity from sdl_info to convert. There is no timeout: a "
    "direct client imposes its own. Example: "
    "{\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"},"
    "{\"type\":\"mouse_button\",\"button\":\"left\",\"x\":120,\"y\":80}],"
    "\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}";

/*
  A key action has no repeat property: repeat a press by sending another action
  instead, which keeps the batch a plain ordered list of single events. The
  region member is the same object sdl_capture takes, parsed by the same helper,
  so the schemas cannot drift from the accepted documents.
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
  encoding is the Handler's policy; a direct library client receives raw RGBA.
*/
constexpr std::string_view kCaptureDescription =
    "Ask the application for a fresh frame through SDL3MCP_FrameRequested() and "
    "return its raw pixels. Arguments: optional region. The result is one "
    "JSON-RPC line {\"format\":\"rgba8888\",\"width\":W,\"height\":H,"
    "\"byteLength\":B,\"frame\":F}, immediately followed by exactly B tightly "
    "packed RGBA bytes with no newline and no base64. F is the app frame whose "
    "pixels these are; the Handler names it in the image summary. region is a "
    "crop rectangle in pixels of the full frame with a top-left origin and must "
    "fit inside the frame. sdl_input coordinates are window points, not frame "
    "pixels: sdl_info reports pixelWidth, pixelHeight and pixelDensity so a caller "
    "can convert between the two. There is no timeout: a direct client imposes its "
    "own. Example: "
    "{\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}";

constexpr std::string_view kCaptureSchema = R"({
  "type": "object",
  "properties": {
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
      keyMap_(),
      pipeline_(keyMap_, configuration_.value().max_frame_bytes) {
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

SDL_AppResult Runtime::iterate() {
   /*
     The frame the app wrote since the last iterate serves every request that was
     registered before it. Completing first keeps a request registered in this
     pump from being answered by the frame the app wrote for the previous one.
     A batch that failed to apply answers here too, one frame after it was
     applied, with an error and no frame.
   */
   pipeline_.completeFailures();
   pipeline_.completeWrittenFrame();

   /*
     The pump is the server loop: every available line is answered here, on the
     app thread. Input and capture register their requests in this pass and are
     applied to the frame below.
   */
   if (session_) {
      session_->pump();
   }

   ++frameCounter_;
   pipeline_.applyRegisteredInputs(frameCounter_);

   /*
     A transport error during the pump retires the session's orders. Drop the
     requests they were going to answer instead of completing into a closed
     stream.
   */
   if (session_ && !session_->isOpen()) {
      pipeline_.discardRequests();
   }

#ifdef __EMSCRIPTEN__
   /*
     The browser's transport is the WebSocket, and a page with no client has
     nothing left to serve: a session that had opened and then ended, or a dial
     that reported an error, quits the app once. The dial is asynchronous, so
     "not open" is also the normal state between AppInit and the first frame;
     only a session that has opened (or failed) counts as an end.
   */
   if (session_) {
      if (session_->isOpen()) {
         sessionOpened_ = true;
      } else if (!sessionClosed_ &&
                 (sessionOpened_ || !session_->lastError().empty())) {
         sessionClosed_ = true;
         terminateRequested_ = true;
      }
   }
#endif

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

void Runtime::buildCatalog() {
   catalog_.add(Tool{
       "sdl_info", std::string(kInfoDescription), parseDefinition(kInfoSchema),
       parseDefinition(kInfoAnnotations),
       [this](ToolOrder, ToolCompletionDelegate& completion) {
          completion.complete(ToolResult::text(infoToJson(infoSnapshot()).dump(2)));
       }});

   catalog_.add(Tool{
       "sdl_input", std::string(kInputDescription), parseDefinition(kInputSchema),
       parseDefinition(kInputAnnotations),
       [this](ToolOrder order, ToolCompletionDelegate& completion) {
          std::string error;
          std::optional<InputRequest> request =
              parseInputArguments(order.arguments, error);
          if (!request.has_value()) {
             completion.complete(ToolResult::text(std::move(error), true));
             return;
          }
          pipeline_.registerInput(std::move(request->batch),
                                  std::move(request->region), completion);
       }});

   catalog_.add(Tool{
       "sdl_capture", std::string(kCaptureDescription),
       parseDefinition(kCaptureSchema), parseDefinition(kCaptureAnnotations),
       [this](ToolOrder order, ToolCompletionDelegate& completion) {
          std::string error;
          std::optional<CaptureArguments> arguments =
              parseCaptureArguments(order.arguments, error);
          if (!arguments.has_value()) {
             completion.complete(ToolResult::text(std::move(error), true));
             return;
          }
          if (window_ == nullptr) {
             completion.complete(ToolResult::text("no window bound", true));
             return;
          }
          pipeline_.registerCapture(std::move(arguments->region), completion);
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
