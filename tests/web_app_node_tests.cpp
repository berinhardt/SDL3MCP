/*
  The app half of the browser path, run under Node.

  The Emscripten video driver needs a canvas and a DOM, so it cannot run here.
  SDL's dummy video driver can: it supplies a real SDL_Window with a framebuffer
  and a working event queue. The driver is selected in C++ because Emscripten
  starts with an empty ENV, so SDL_VIDEODRIVER from the process environment
  never reaches SDL. That, plus a test-only --post-js shim for the two page
  globals src/lib/Api.cpp reads, is the whole seam: the production path is unchanged
  and the real archive is linked.

  The suite is the app thread. It runs the app loop exactly as a game does -
  iterate, write a frame when one is requested, poll events - and lets the
  harness in run_node_web_app_test.js be the MCP client on the other end of the
  WebSocket. This file asserts the SDL facts (the driver, the window, one quit,
  one page close) and prints the events it consumed, which is the evidence the
  harness checks for real keys and buttons. The protocol conversation lives in
  the harness.

  What this cannot cover is the browser itself - canvas, WebGL, DOM input, the
  browser's WebSocket, a real page's query string and window.close() - and the
  Handler's web stack. Those stay with the manual `--web` acceptance run.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <SDL3/SDL.h>
#include <SDL3MCP.h>
#include <doctest/doctest.h>
#include <emscripten/emscripten.h>

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

constexpr int kWindowWidth = 64;
constexpr int kWindowHeight = 48;
constexpr int kFrameWidth = 4;
constexpr int kFrameHeight = 3;
constexpr std::chrono::seconds kConversationDeadline{20};

/* A frame whose pixels encode their coordinates, so the client can check them. */
std::vector<std::uint8_t> framePattern() {
   std::vector<std::uint8_t> pixels(
       static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 4);
   for (int y = 0; y < kFrameHeight; ++y) {
      for (int x = 0; x < kFrameWidth; ++x) {
         const std::size_t index =
             (static_cast<std::size_t>(y) * kFrameWidth + x) * 4;
         pixels[index + 0] = static_cast<std::uint8_t>(x * 40 + 1);
         pixels[index + 1] = static_cast<std::uint8_t>(y * 40 + 2);
         pixels[index + 2] = 3;
         pixels[index + 3] = 255;
      }
   }
   return pixels;
}

/* The harness reads these lines while the app runs, so each one is flushed. */
void note(const char* format, ...) {
   std::va_list arguments;
   va_start(arguments, format);
   std::printf("app: ");
   std::vprintf(format, arguments);
   std::printf("\n");
   va_end(arguments);
   std::fflush(stdout);
}

/* The post-js shim counts window.close(), which is the page going away. */
int pageCloseCalls() {
   return EM_ASM_INT({ return globalThis.__sdl3mcpPageCloseCalls || 0; });
}

/*
  Prints one event and returns the quit events it carried. The event fields are
  the ones the harness checks: the scancode and key of synthetic keys, and the
  button and position of synthetic clicks.
*/
int printEvent(const SDL_Event& event) {
   switch (event.type) {
      case SDL_EVENT_QUIT:
         note("quit event");
         return 1;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
         note("key %s scancode=%d key=%d mod=%u",
              event.type == SDL_EVENT_KEY_DOWN ? "down" : "up",
              static_cast<int>(event.key.scancode),
              static_cast<int>(event.key.key),
              static_cast<unsigned>(event.key.mod));
         return 0;
      case SDL_EVENT_MOUSE_MOTION:
         note("mouse motion x=%.0f y=%.0f", event.motion.x, event.motion.y);
         return 0;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
         note("mouse button %s button=%u x=%.0f y=%.0f",
              event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up",
              static_cast<unsigned>(event.button.button), event.button.x,
              event.button.y);
         return 0;
      case SDL_EVENT_TEXT_INPUT:
         note("text input=%s", event.text.text != nullptr ? event.text.text : "");
         return 0;
      default:
         return 0;
   }
}

}  // namespace

TEST_CASE("the page runs the app path over its query dial") {
   /*
     The hint has to precede SDL_Init, and Emscripten's empty ENV means the
     environment variable the desktop CTest sets has no effect here.
   */
   REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
   REQUIRE(SDL_Init(SDL_INIT_VIDEO));

   const char* videoDriver = SDL_GetCurrentVideoDriver();
   REQUIRE(videoDriver != nullptr);
   CHECK(std::string_view(videoDriver) == "dummy");
   note("video driver %s", videoDriver);

   SDL_Window* window =
       SDL_CreateWindow("SDL3MCP node path", kWindowWidth, kWindowHeight, 0);
   REQUIRE(window != nullptr);

   /*
     The page query is the only endpoint source in a browser, and AppInit reads
     it through the shim. A failure here is a configuration error on stderr.
   */
   REQUIRE(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_CONTINUE);
   REQUIRE(SDL3MCP_SetWindow(window) == SDL_APP_CONTINUE);

   int pixelWidth = 0;
   int pixelHeight = 0;
   REQUIRE(SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight));
   note("window bound %dx%d", pixelWidth, pixelHeight);

   const std::vector<std::uint8_t> pixels = framePattern();
   int quitEvents = 0;
   int framesWritten = 0;
   bool appFailed = false;
   const auto deadline = std::chrono::steady_clock::now() + kConversationDeadline;

   while (std::chrono::steady_clock::now() < deadline) {
      if (SDL3MCP_AppIterate() != SDL_APP_CONTINUE) {
         appFailed = true;
         break;
      }
      if (SDL3MCP_FrameRequested()) {
         SDL3MCP_WriteFrame(pixels.data(), pixels.size(), kFrameWidth,
                            kFrameHeight);
         ++framesWritten;
      }

      SDL_Event event;
      while (SDL_PollEvent(&event)) {
         quitEvents += printEvent(event);
      }
      if (quitEvents > 0) {
         break;
      }

      /* The only yield: it lets the WebSocket callbacks and the client run. */
      emscripten_sleep(1);
   }

   CHECK(!appFailed);
   /* One sdl_terminate, one SDL_EVENT_QUIT. */
   CHECK(quitEvents == 1);
   /* Two captures and two input observations each asked for a frame. */
   CHECK(framesWritten >= 4);

   SDL3MCP_AppShutdown();
   const int closeCalls = pageCloseCalls();
   note("page close calls %d", closeCalls);
   CHECK(closeCalls == 1);
   note("done");

   SDL_DestroyWindow(window);
   SDL_Quit();
}
