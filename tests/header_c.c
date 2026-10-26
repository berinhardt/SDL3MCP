#include <SDL3MCP/SDL3MCP.h>

#include <stdbool.h>
#include <stdio.h>

static int check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

int main(void)
{
    SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
    int failures = 0;

    failures += check(SDL3MCP_TRANSPORT_NONE == 0,
                      "SDL3MCP_TRANSPORT_NONE is 0");
    failures += check(configuration.transport == SDL3MCP_TRANSPORT_NONE,
                      "default transport is SDL3MCP_TRANSPORT_NONE");
    failures += check(configuration.frame_timeout_ms == 2000u,
                      "default frame_timeout_ms is 2000");
    failures += check(configuration.max_frame_bytes == 32u * 1024u * 1024u,
                      "default max_frame_bytes is 32 MiB");
    failures += check(configuration.warp_cursor,
                      "default warp_cursor is true");
    failures += check(configuration.log_level == -1,
                      "default log_level is -1");

    /* Safe before AppInit and idempotent. */
    SDL3MCP_AppShutdown();
    SDL3MCP_AppShutdown();

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
