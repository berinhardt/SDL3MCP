#include <SDL3MCP/SDL3MCP.h>

#include <cstdio>
#include <cstring>

static_assert(SDL3MCP_VERSION_MAJOR == 0, "version major is 0");
static_assert(SDL3MCP_VERSION_MINOR == 1, "version minor is 1");
static_assert(SDL3MCP_VERSION_PATCH == 0, "version patch is 0");
static_assert(SDL3MCP_TRANSPORT_NONE == 0, "transport NONE is 0");

namespace {

int check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

} // namespace

int main()
{
    SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
    int failures = 0;

    failures += check(std::strcmp(SDL3MCP_VERSION_STRING, "0.1.0") == 0,
                      "version string is 0.1.0");
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

    SDL3MCP_AppShutdown();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
