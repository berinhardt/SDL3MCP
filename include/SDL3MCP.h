/*
  SDL3MCP 1.0 - the complete public surface.

  The library owns no window and no thread: the app keeps its loop and answers
  frame requests. There is no transport enum and no stdio transport. A Session
  exists exactly when connect_host names an endpoint, and the only library
  timeout is the one dial attempt, connect_timeout_ms.

  The configuration value and the release macros live in the SDL-free
  SDL3MCP_types.h, which this header includes; that header includes neither SDL
  nor the entry points below.

  Linkage: static only; no linkage macro. The library is built as a static
  archive and Windows is a configure error, so the declarations carry neither an
  import/export attribute nor a calling convention. SDLCALL is empty on Linux,
  so the ABI is unchanged.
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

SDL_AppResult SDL3MCP_AppInit(SDL3MCP_Configuration* config, int argc, char** argv);
SDL_AppResult SDL3MCP_SetWindow(SDL_Window* window);
SDL_AppResult SDL3MCP_AppIterate(void);
void SDL3MCP_AppShutdown(void);
bool SDL3MCP_FrameRequested(void);
/*
  Answers a pending capture with tightly packed RGBA8888: `size` must be exactly
  width * height * 4, top-left origin, no row padding. The app supplies width and
  height, so the pixels may come from any render target rather than only the
  bound window. Ignored when no capture is pending.
*/
void SDL3MCP_WriteFrame(const uint8_t* data, size_t size, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* SDL3MCP_H */
