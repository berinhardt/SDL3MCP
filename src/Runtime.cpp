#include "Runtime.h"

#include <SDL3MCP.h>

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
#include "ToolDefinitions.h"
#include "ToolResult.h"

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
   for (const ToolDefinition& definition : applicationToolDefinitions()) {
      Tool tool{definition.name, definition.description, definition.inputSchema, definition.annotations, {}};
      if (definition.name == "sdl_info") {
         tool.invoke = [this](ToolOrder, ToolCompletionDelegate& completion) {
            completion.complete(ToolResult::text(infoToJson(infoSnapshot()).dump(2)));
         };
      } else if (definition.name == "sdl_input") {
         tool.invoke = [this](ToolOrder order, ToolCompletionDelegate& completion) {
            Result<InputRequest, ProtocolError> request =
                parseInputArguments(order.arguments);
            if (!request) {
               completion.complete(
                   ToolResult::text(std::move(request.error().message), true));
               return;
            }
            pipeline_.registerInput(std::move(request->batch),
                                    std::move(request->region), completion);
         };
      } else if (definition.name == "sdl_capture") {
         tool.invoke = [this](ToolOrder order, ToolCompletionDelegate& completion) {
            Result<CaptureArguments, ProtocolError> arguments =
                parseCaptureArguments(order.arguments);
            if (!arguments) {
               completion.complete(
                   ToolResult::text(std::move(arguments.error().message), true));
               return;
            }
            if (window_ == nullptr) {
               completion.complete(ToolResult::text("no window bound", true));
               return;
            }
            pipeline_.registerCapture(std::move(arguments->region), completion);
         };
      } else if (definition.name == "sdl_terminate") {
         tool.invoke = [this](ToolOrder, ToolCompletionDelegate& completion) {
            terminateRequested_ = true;
            completion.complete(ToolResult::text("termination requested"));
         };
      }
      catalog_.add(std::move(tool));
   }
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
