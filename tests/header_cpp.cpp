#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <SDL3MCP/SDL3MCP.h>

#include <cstring>

static_assert(SDL3MCP_VERSION_MAJOR == 0, "version major is 0");
static_assert(SDL3MCP_VERSION_MINOR == 1, "version minor is 1");
static_assert(SDL3MCP_VERSION_PATCH == 0, "version patch is 0");
static_assert(SDL3MCP_TRANSPORT_NONE == 0, "transport NONE is 0");

TEST_CASE("the C++ header exposes the version and the default configuration")
{
    const SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();

    CHECK(std::strcmp(SDL3MCP_VERSION_STRING, "0.1.0") == 0);
    CHECK(configuration.transport == SDL3MCP_TRANSPORT_NONE);
    CHECK(configuration.frame_timeout_ms == 2000u);
    CHECK(configuration.max_frame_bytes == 32u * 1024u * 1024u);
    CHECK(configuration.warp_cursor);
    CHECK(configuration.log_level == -1);

    SDL3MCP_AppShutdown();
}
