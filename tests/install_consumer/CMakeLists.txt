# The installed-tree consumer: it only sees SDL3MCP through the installed
# package, never the source, build, or third_party trees.
cmake_minimum_required(VERSION 3.21)

project(sdl3mcp_install_consumer LANGUAGES C CXX)

find_package(SDL3MCP 0.1 CONFIG REQUIRED)

add_executable(consumer main.c)
set_target_properties(consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    LINKER_LANGUAGE CXX
)
target_link_libraries(consumer PRIVATE SDL3MCP::SDL3MCP)
