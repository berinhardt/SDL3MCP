/*
  The 1.0 C boundary. This file owns the process-global Runtime, the once flag
  that makes a second AppInit fail even after shutdown, and every translation
  between an exception and an SDL error plus SDL_AppResult.

  There is no server thread and no descriptor dance: AppInit records the
  configuration and dials once when an endpoint is set, AppIterate is the pump,
  and AppShutdown closes the session. A failed startup leaves no state behind
  and still claims the lifecycle, so there is no restart path.
*/

#include <SDL3MCP/SDL3MCP1.h>

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

extern "C" SDL_DECLSPEC SDL_AppResult SDLCALL
SDL3MCP_AppInit(SDL3MCP_Configuration* config, int argc, char** argv) {
   try {
      if (g_lifecycleClaimed.exchange(true)) {
         SDL_SetError("SDL3MCP_AppInit has already been called in this process");
         return SDL_APP_FAILURE;
      }

      const SDL3MCP_Configuration base =
          config != nullptr ? *config : SDL3MCP_DefaultConfiguration();
      sdl3mcp::Configuration configuration(base);

      const sdl3mcp::Configuration::Status status =
          configuration.parseCommandLine(argc, argv);
      if (status == sdl3mcp::Configuration::Status::Error) {
         std::fprintf(stderr, "SDL3MCP: %s\n%s\n",
                      configuration.message().c_str(),
                      sdl3mcp::Configuration::usageText().c_str());
         SDL_SetError("%s", configuration.message().c_str());
         return SDL_APP_FAILURE;
      }
      if (status == sdl3mcp::Configuration::Status::ExitSuccess) {
         writeToStderr(configuration.message());
         return SDL_APP_SUCCESS;
      }

      auto runtime = std::make_unique<sdl3mcp::Runtime>(std::move(configuration));
      std::string error;
      if (!runtime->start(error)) {
         SDL_SetError("%s", error.c_str());
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

extern "C" SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_SetWindow(SDL_Window* window) {
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

extern "C" SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_AppIterate(void) {
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

extern "C" SDL_DECLSPEC void SDLCALL SDL3MCP_AppShutdown(void) {
   try {
      if (g_runtime) {
         g_runtime->shutdown();
         g_runtime.reset();
      }
   } catch (...) {
      /* A failed shutdown must still be a no-op rather than an escaping exception. */
      g_runtime.reset();
   }
}

extern "C" SDL_DECLSPEC bool SDLCALL SDL3MCP_FrameRequested(void) {
   try {
      return g_runtime ? g_runtime->frameRequested() : false;
   } catch (...) {
      return false;
   }
}

extern "C" SDL_DECLSPEC void SDLCALL
SDL3MCP_WriteFrame(const uint8_t* data, size_t size, int width, int height) {
   try {
      if (g_runtime) {
         g_runtime->writeFrame(data, size, width, height);
      }
   } catch (...) {
      /* The C boundary never throws; a lost frame becomes a capture timeout. */
   }
}
