/*
  SDL3MCP 1.0 - the complete public surface.

  The library owns no window and no thread: the app keeps its loop and answers
  frame requests. There is no transport enum and no stdio transport. A Session
  exists exactly when connect_host names an endpoint, and the only library
  timeout is the one dial attempt, connect_timeout_ms.

  Linkage follows SDL: public symbols carry SDL_DECLSPEC and SDLCALL. The shared
  build defines DLL_EXPORT, the static build defines SDL_STATIC_LIB.
*/

#ifndef SDL3MCP_H
#define SDL3MCP_H

#include <SDL3/SDL.h>
#include <SDL3MCP/SDL3MCP_version.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL3MCP_Configuration {
   /*
     The client endpoint the app dials, HOST:PORT. An empty connect_host means
     no MCP session at all; a non-empty one means exactly one dial attempt,
     never a fallback to no session.
   */
   char connect_host[64];
   uint16_t connect_port;
   /* One dial attempt is bounded by this; the library has no other timeout. */
   uint32_t connect_timeout_ms;
   uint32_t max_frame_bytes;
   bool warp_cursor;
   int log_level;
} SDL3MCP_Configuration;

/* Header-inline so the defaults cost no symbol and work from C and C++. */
static inline SDL3MCP_Configuration SDL3MCP_DefaultConfiguration(void) {
   SDL3MCP_Configuration configuration;

   SDL_zero(configuration.connect_host);
   configuration.connect_port = 0;
   configuration.connect_timeout_ms = 5000u;
   configuration.max_frame_bytes = 32u * 1024u * 1024u;
   configuration.warp_cursor = true;
   configuration.log_level = -1;

   return configuration;
}

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
