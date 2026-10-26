/*
  The smallest installed-tree host: it proves the exported target supplies the
  public header and a linkable library, and that the default transport really is
  NONE so AppInit needs no descriptors.
*/
#include <SDL3MCP/SDL3MCP.h>

#include <stdio.h>

int main(void)
{
    const SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();

    if (configuration.transport != SDL3MCP_TRANSPORT_NONE) {
        fprintf(stderr, "FAIL: default transport is not SDL3MCP_TRANSPORT_NONE\n");
        return 1;
    }
    if (SDL3MCP_AppInit(NULL, 0, NULL) != SDL_APP_CONTINUE) {
        fprintf(stderr, "FAIL: SDL3MCP_AppInit failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL3MCP_AppShutdown();

    printf("installed consumer: SDL3MCP %s\n", SDL3MCP_VERSION_STRING);
    return 0;
}
