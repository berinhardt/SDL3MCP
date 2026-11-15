/*
  The 1.0 public headers must compile as C11 and expose the six entry points with
  their C linkage. This translation unit is compiled by the C compiler, so a C++
  construct that slips into a header fails here instead of at a consumer.

  The types header comes first, and the guard checks are what prove the split:
  it compiles on its own, it is SDL-free, and it does not pull the entry-point
  header in behind it.
*/

#include <SDL3MCP_types.h>

#ifdef SDL_h_
#error "SDL3MCP_types.h must not include SDL"
#endif
#ifdef SDL3MCP_H
#error "SDL3MCP_types.h must not include the entry-point header"
#endif

#include <SDL3MCP.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int main(void) {
   SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();

   if (configuration.connect_host[0] != '\0') {
      return 1;
   }
   if (configuration.connect_port != 0) {
      return 2;
   }
   if (configuration.connect_timeout_ms != 5000u) {
      return 3;
   }
   if (configuration.max_frame_bytes != 32u * 1024u * 1024u) {
      return 4;
   }
   if (!configuration.warp_cursor) {
      return 5;
   }
   if (configuration.log_level != -1) {
      return 6;
   }

   /* The release is 1.0.0; the header carries the generated macros. */
   if (SDL3MCP_VERSION_MAJOR != 1 || SDL3MCP_VERSION_MINOR != 0 ||
       SDL3MCP_VERSION_PATCH != 0) {
      return 7;
   }
   if (strcmp(SDL3MCP_VERSION_STRING, "1.0.0") != 0) {
      return 8;
   }

   /* Assigning each symbol to its declared type proves the signatures agree. */
   SDL_AppResult (*appInit)(SDL3MCP_Configuration*, int, char**) = SDL3MCP_AppInit;
   SDL_AppResult (*setWindow)(SDL_Window*) = SDL3MCP_SetWindow;
   SDL_AppResult (*appIterate)(void) = SDL3MCP_AppIterate;
   void (*appShutdown)(void) = SDL3MCP_AppShutdown;
   bool (*frameRequested)(void) = SDL3MCP_FrameRequested;
   void (*writeFrame)(const uint8_t*, size_t, int, int) = SDL3MCP_WriteFrame;

   (void)appInit;
   (void)setWindow;
   (void)appIterate;
   (void)appShutdown;
   (void)frameRequested;
   (void)writeFrame;

   return 0;
}
