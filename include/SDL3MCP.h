/*
  SDL3MCP 1.0 - the complete public surface.

  The library owns no window and no thread: the app keeps its loop and answers
  frame requests. There is no transport enum and no stdio transport. A Session
  exists exactly when connect_host names an endpoint, and the only library
  timeout is the one dial attempt, connect_timeout_ms.

  The configuration value and the release macros live in the SDL-free
  SDL3MCP_types.h, which this header includes; that header includes neither SDL
  nor the entry points below.

  Linkage follows SDL: public symbols carry SDL_DECLSPEC and SDLCALL. The shared
  build defines DLL_EXPORT, the static build defines SDL_STATIC_LIB.
*/

#ifndef SDL3MCP_H
#define SDL3MCP_H

#include <SDL3/SDL.h>
#include <SDL3MCP_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_AppInit(SDL3MCP_Configuration* config, int argc, char** argv);
SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_SetWindow(SDL_Window* window);
SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_AppIterate(void);
SDL_DECLSPEC void SDLCALL SDL3MCP_AppShutdown(void);
SDL_DECLSPEC bool SDLCALL SDL3MCP_FrameRequested(void);
/*
  Answers a pending capture with tightly packed RGBA8888: `size` must be exactly
  width * height * 4, top-left origin, no row padding. The app supplies width and
  height, so the pixels may come from any render target rather than only the
  bound window. Ignored when no capture is pending.
*/
SDL_DECLSPEC void SDLCALL
SDL3MCP_WriteFrame(const uint8_t* data, size_t size, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* SDL3MCP_H */
