/*
  The public header is C++20-clean: it includes cleanly, the default
  configuration is a value, and the release macros agree with the project
  version. The C translation unit next to it covers C11 and the signatures.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <SDL3MCP/SDL3MCP.h>
#include <doctest/doctest.h>

#include <cstring>

static_assert(SDL3MCP_VERSION_MAJOR == 1, "version major is 1");
static_assert(SDL3MCP_VERSION_MINOR == 0, "version minor is 0");
static_assert(SDL3MCP_VERSION_PATCH == 0, "version patch is 0");

TEST_CASE("the C++ header exposes the version and the default configuration") {
   const SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();

   CHECK(std::strcmp(SDL3MCP_VERSION_STRING, "1.0.0") == 0);
   CHECK(configuration.connect_host[0] == '\0');
   CHECK(configuration.connect_port == 0);
   CHECK(configuration.connect_timeout_ms == 5000u);
   CHECK(configuration.max_frame_bytes == 32u * 1024u * 1024u);
   CHECK(configuration.warp_cursor);
   CHECK(configuration.log_level == -1);

   /* A default configuration names no endpoint, so shutdown is a no-op. */
   SDL3MCP_AppShutdown();
}
