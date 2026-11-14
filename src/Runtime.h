#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Configuration.h"
#include "Info.h"
#include "KeyMap.h"
#include "Pipeline.h"
#include "Session.h"
#include "ToolCatalog.h"

namespace sdl3mcp {

/*
  The process-wide library state behind the C ABI.

  It is not public. AppInit builds it, AppIterate drives it, AppShutdown tears it
  down. Everything runs on the app thread: the session is pumped by AppIterate,
  so a tool handler reads live SDL state instead of a cross-thread snapshot.

  An optional Session is the whole transport choice. No endpoint means no
  session and AppIterate is a no-op; an endpoint means exactly one dial in
  start(). The native dial completes inside start(), so a failure leaves no
  Runtime behind; the browser dial is asynchronous, so a failure arrives as a
  close or error and ends the page on the next AppIterate.
*/
class Runtime {
  public:
   explicit Runtime(Configuration configuration);
   ~Runtime();

   Runtime(const Runtime&) = delete;
   Runtime& operator=(const Runtime&) = delete;

   /* Builds the catalog and dials when an endpoint is configured. */
   bool start(std::string& error);

   SDL_AppResult setWindow(SDL_Window* window);
   SDL_AppResult iterate();
   void shutdown();

   /* True while a capture or input request waits for the next frame. */
   bool frameRequested() const noexcept { return pipeline_.frameRequested(); }
   void writeFrame(const std::uint8_t* data,
                   std::size_t size,
                   int width,
                   int height) noexcept;

  private:
   void buildCatalog();
   Info infoSnapshot() const;

   Configuration configuration_;

   /* App thread only: the key map outlives the pipeline that resolves names. */
   KeyMap keyMap_;
   Pipeline pipeline_;

   /* Declared before the session so the session dies first. */
   ToolCatalog catalog_;
   std::unique_ptr<Session> session_;

   SDL_Window* window_ = nullptr;
   SDL_WindowID windowID_ = 0;
   std::uint64_t frameCounter_ = 0;

   /*
     Set by sdl_terminate and exchanged by the app thread, which owns the only
     SDL call (SDL_PushEvent). One thread, so a plain bool is enough.
   */
   bool terminateRequested_ = false;

#ifdef __EMSCRIPTEN__
   /*
     A browser has no server thread to fail: the WebSocket closing or erroring
     is the session ending, and the page has nothing left to serve, so the app
     quits. The latches keep a closed session from pushing an event every frame,
     and keep the asynchronous dial - where "not open" is the normal state
     between AppInit and the first frame - from being mistaken for a close.
   */
   bool sessionOpened_ = false;
   bool sessionClosed_ = false;
#endif
};

}  // namespace sdl3mcp
