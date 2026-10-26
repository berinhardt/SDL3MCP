#include <SDL3MCP/SDL3MCP.h>

/*
  Shutdown ships first because an app may call it unconditionally from
  SDL_AppQuit: it has to be valid before SDL3MCP_AppInit and safe to repeat.
  Nothing has been allocated yet, so there is nothing to release.
*/
extern "C" void SDL_DECLSPEC SDLCALL SDL3MCP_AppShutdown(void)
{
}
