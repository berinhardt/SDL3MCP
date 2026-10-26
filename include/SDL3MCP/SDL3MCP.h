/*
  SDL3MCP - an MCP server you link into any SDL3 application.

  The library owns no renderer and no window: the app keeps its event loop and
  answers frame requests. This header is the complete public surface and must
  compile as C11 and as C++20.

  Linkage follows SDL itself: public symbols are tagged with SDL_DECLSPEC and
  SDLCALL. Shared builds define DLL_EXPORT through CMake's DEFINE_SYMBOL, static
  builds define SDL_STATIC_LIB privately.
*/

#ifndef SDL3MCP_H
#define SDL3MCP_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3MCP/SDL3MCP_version.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SDL3MCP_Transport
{
    SDL3MCP_TRANSPORT_NONE = 0,
    SDL3MCP_TRANSPORT_STDIO
} SDL3MCP_Transport;

typedef struct SDL3MCP_Configuration
{
    SDL3MCP_Transport transport;
    uint32_t frame_timeout_ms;
    uint32_t max_frame_bytes;
    bool warp_cursor;
    int log_level;
} SDL3MCP_Configuration;

/* Header-inline so the defaults cost no symbol and work from C and C++. */
static inline SDL3MCP_Configuration SDL3MCP_DefaultConfiguration(void)
{
    SDL3MCP_Configuration configuration;

    configuration.transport = SDL3MCP_TRANSPORT_NONE;
    configuration.frame_timeout_ms = 2000u;
    configuration.max_frame_bytes = 32u * 1024u * 1024u;
    configuration.warp_cursor = true;
    configuration.log_level = -1;

    return configuration;
}

SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_AppInit(SDL3MCP_Configuration *config, int argc, char **argv);
SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_SetWindow(SDL_Window *window);
SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_AppIterate(void);
SDL_DECLSPEC void SDLCALL SDL3MCP_AppShutdown(void);
SDL_DECLSPEC bool SDLCALL SDL3MCP_FrameRequested(void);
SDL_DECLSPEC void SDLCALL SDL3MCP_WriteFrame(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SDL3MCP_H */
