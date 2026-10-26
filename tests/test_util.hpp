/*
  Minimal assertion macro for the plain executable tests. It does not depend on
  NDEBUG, exceptions, or a test framework, so it stays active in Release builds.
*/

#ifndef SDL3MCP_TEST_UTIL_HPP
#define SDL3MCP_TEST_UTIL_HPP

#include <cstdio>
#include <cstdlib>

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #expression);                                         \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

#endif /* SDL3MCP_TEST_UTIL_HPP */
