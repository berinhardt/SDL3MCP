/*
  The 1.0 C boundary. This file owns the process-global Runtime, the once flag
  that makes a second AppInit fail even after shutdown, and every translation
  between an exception and an SDL error plus SDL_AppResult.

  There is no server thread and no descriptor dance: AppInit records the
  configuration and dials once when an endpoint is set, AppIterate is the pump,
  and AppShutdown closes the session. A failed startup leaves no state behind
  and still claims the lifecycle, so there is no restart path. The browser has
  no argv, so there the page query supplies the same `--mcp` value the desktop
  command line does; AppShutdown closes the tab as well as the stream.
*/

#include <SDL3MCP.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "Configuration.h"
#include "Runtime.h"

namespace {

std::unique_ptr<sdl3mcp::Runtime> g_runtime;

/* Set by the first AppInit and never cleared: one lifecycle per process. */
std::atomic<bool> g_lifecycleClaimed{false};

void writeToStderr(const std::string& text) {
   std::fprintf(stderr, "%s\n", text.c_str());
}

SDL_AppResult reportException(const char* entryPoint, const char* what) {
   SDL_SetError("%s failed: %s", entryPoint, what);
   return SDL_APP_FAILURE;
}

}  // namespace

extern "C" SDL_AppResult
SDL3MCP_AppInit(SDL3MCP_Configuration* config, int argc, char** argv) {
   try {
      if (g_lifecycleClaimed.exchange(true)) {
         SDL_SetError("SDL3MCP_AppInit has already been called in this process");
         return SDL_APP_FAILURE;
      }

      const SDL3MCP_Configuration base =
          config != nullptr ? *config : SDL3MCP_DefaultConfiguration();
      sdl3mcp::Configuration configuration(base);

#ifdef __EMSCRIPTEN__
      /*
        A page has no command line, so the query string is the only place a
        server can be named. It becomes the same --mcp=<value> argument the
        desktop build would parse, which keeps one grammar and one validator.
      */
      (void)argc;
      (void)argv;
      const char* search = emscripten_run_script_string("window.location.search");
      const sdl3mcp::Configuration::Status status = sdl3mcp::applyPageQuery(
          configuration, search != nullptr ? search : "");
#else
      const sdl3mcp::Configuration::Status status =
          configuration.parseCommandLine(argc, argv);
#endif
      if (status == sdl3mcp::Configuration::Status::Error) {
#ifdef __EMSCRIPTEN__
         /* The browser console gets the diagnosis; there is no usage listing. */
         std::fprintf(stderr, "SDL3MCP: %s\n", configuration.message().c_str());
#else
         std::fprintf(stderr, "SDL3MCP: %s\n%s\n",
                      configuration.message().c_str(),
                      sdl3mcp::Configuration::usageText().c_str());
#endif
         SDL_SetError("%s", configuration.message().c_str());
         return SDL_APP_FAILURE;
      }
      if (status == sdl3mcp::Configuration::Status::ExitSuccess) {
         writeToStderr(configuration.message());
         return SDL_APP_SUCCESS;
      }

      auto runtime = std::make_unique<sdl3mcp::Runtime>(std::move(configuration));
      const sdl3mcp::Result<void, sdl3mcp::TransportError> started =
          runtime->start();
      if (!started) {
         SDL_SetError("%s", started.error().message.c_str());
         return SDL_APP_FAILURE;
      }

      g_runtime = std::move(runtime);
      return SDL_APP_CONTINUE;
   } catch (const std::exception& exception) {
      g_runtime.reset();
      return reportException("SDL3MCP_AppInit", exception.what());
   } catch (...) {
      g_runtime.reset();
      return reportException("SDL3MCP_AppInit", "unknown exception");
   }
}

extern "C" SDL_AppResult SDL3MCP_SetWindow(SDL_Window* window) {
   try {
      if (!g_runtime) {
         SDL_SetError("SDL3MCP_SetWindow called before SDL3MCP_AppInit");
         return SDL_APP_FAILURE;
      }
      return g_runtime->setWindow(window);
   } catch (const std::exception& exception) {
      return reportException("SDL3MCP_SetWindow", exception.what());
   } catch (...) {
      return reportException("SDL3MCP_SetWindow", "unknown exception");
   }
}

extern "C" SDL_AppResult SDL3MCP_AppIterate(void) {
   try {
      if (!g_runtime) {
         return SDL_APP_CONTINUE;
      }
      return g_runtime->iterate();
   } catch (const std::exception& exception) {
      return reportException("SDL3MCP_AppIterate", exception.what());
   } catch (...) {
      return reportException("SDL3MCP_AppIterate", "unknown exception");
   }
}

extern "C" void SDL3MCP_AppShutdown(void) {
   try {
      if (g_runtime) {
         g_runtime->shutdown();
         g_runtime.reset();
      }
   } catch (...) {
      /* A failed shutdown must still be a no-op rather than an escaping exception. */
      g_runtime.reset();
   }
#ifdef __EMSCRIPTEN__
   /*
     The page owns the window and only the app knows it has quit, so the tab
     closes itself. shutdown() has already closed the stream, so the client sees
     the session end before the page disappears. The script is an EM_ASM body
     rather than an emscripten_run_script string so it is emitted into the
     generated JavaScript, where the size of the wasm data section cannot hide
     it.

     Firefox refuses window.close() unless dom.allow_scripts_to_close_windows is
     true, which the web run sets in the throwaway profile's user.js. A refusal
     is not an error here: the run kills the browser after its grace period, and
     this call exists so that the normal case is a closed window rather than a
     kill.
   */
   EM_ASM({ window.close(); });
#endif
}

extern "C" bool SDL3MCP_FrameRequested(void) {
   try {
      return g_runtime ? g_runtime->frameRequested() : false;
   } catch (...) {
      return false;
   }
}

extern "C" void
SDL3MCP_WriteFrame(const uint8_t* data, size_t size, int width, int height) {
   try {
      if (g_runtime) {
         g_runtime->writeFrame(data, size, width, height);
      }
   } catch (...) {
      /* The C boundary never throws; a lost frame becomes a capture timeout. */
   }
}
