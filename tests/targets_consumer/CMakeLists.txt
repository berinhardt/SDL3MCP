# The public-target consumer: it embeds SDL3MCP the way a parent project does
# and asserts the three public target names are visible. Later tickets attach
# real sources to them; this check guards the names themselves.
cmake_minimum_required(VERSION 3.21)

project(sdl3mcp_targets_consumer LANGUAGES C CXX)

set(SDL3MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SDL3MCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL3MCP_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL3MCP_SOURCE_DIR}" sdl3mcp)

foreach(sdl3mcp_public_target SDL3MCP::SDL3MCP SDL3MCP::SERVER SDL3MCP::COMMON)
    if(NOT TARGET "${sdl3mcp_public_target}")
        message(FATAL_ERROR
            "SDL3MCP: public target ${sdl3mcp_public_target} is missing")
    endif()
endforeach()
