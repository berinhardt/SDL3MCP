/*
  SDL3MCP 1.0 - the SDL-free half of the public surface: the configuration value
  and the release macros. A translation unit that only builds or reads a
  configuration includes this header, so it needs neither SDL nor the entry
  points in SDL3MCP.h.
*/

#ifndef SDL3MCP_TYPES_H
#define SDL3MCP_TYPES_H

#include <SDL3MCP_version.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

   /* memset, not SDL_zero: this header must compile without SDL. */
   memset(&configuration, 0, sizeof configuration);
   configuration.connect_timeout_ms = 5000u;
   configuration.max_frame_bytes = 32u * 1024u * 1024u;
   configuration.warp_cursor = true;
   configuration.log_level = -1;

   return configuration;
}

#ifdef __cplusplus
}
#endif

#endif /* SDL3MCP_TYPES_H */
