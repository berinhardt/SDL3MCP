/*
  The 1.0 public header must compile as C11 and expose the six entry points with
  their C linkage. This translation unit is compiled by the C compiler, so a C++
  construct that slips into the header fails here instead of at a consumer.
*/

#include <SDL3MCP/SDL3MCP1.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
