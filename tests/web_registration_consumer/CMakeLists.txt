# The web-registration check's project. It includes the registration module
# directly, so a configure exercises the function with no SDL, no toolchain and
# no build. tests/check_web_registration.cmake drives it, one configure per
# case, because a FATAL_ERROR is only observable from outside the configure that
# raises it.
#
# The only input is SDL3MCP_WEB_CASE, and SDL3MCP_SOURCE_DIR names the tree the
# module comes from. EMSCRIPTEN is passed on the command line for every case
# that is meant to run on an Emscripten configure; the desktop case leaves it
# unset, which is what a real desktop configure looks like.
cmake_minimum_required(VERSION 3.21)

project(sdl3mcp_web_registration LANGUAGES C)

set(SDL3MCP_WEB_CASE "success" CACHE STRING "the registration case to configure")

set(sdl3mcp_registration_module
    "${SDL3MCP_SOURCE_DIR}/cmake/SDL3MCPRegistration.cmake")
if(NOT EXISTS "${sdl3mcp_registration_module}")
    message(FATAL_ERROR
        "web registration check: ${sdl3mcp_registration_module} is missing")
endif()
include("${sdl3mcp_registration_module}")

# A page target is an executable whose artifact is `.html`; `binary` is an
# executable without one. None of them is built: registration is configure-time.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/registration.c"
    "int main(void) { return 0; }\n")
foreach(sdl3mcp_registration_target page page2 binary desktop_app)
    add_executable(${sdl3mcp_registration_target}
        "${CMAKE_CURRENT_BINARY_DIR}/registration.c")
endforeach()
set_target_properties(page page2 PROPERTIES SUFFIX ".html")

if(SDL3MCP_WEB_CASE STREQUAL "success")
    sdl3mcp_web(demo page)
    sdl3mcp_web(other page2)
elseif(SDL3MCP_WEB_CASE STREQUAL "non_emscripten")
    sdl3mcp_web(demo page)
elseif(SDL3MCP_WEB_CASE STREQUAL "bad_alias")
    sdl3mcp_web("Bad-Alias" page)
elseif(SDL3MCP_WEB_CASE STREQUAL "reserved_prefix")
    sdl3mcp_web(app_spawn_demo page)
elseif(SDL3MCP_WEB_CASE STREQUAL "duplicate_web_alias")
    sdl3mcp_web(demo page)
    sdl3mcp_web(demo page2)
elseif(SDL3MCP_WEB_CASE STREQUAL "app_alias_taken")
    sdl3mcp_app(shared desktop_app)
    sdl3mcp_web(shared page)
elseif(SDL3MCP_WEB_CASE STREQUAL "web_alias_taken")
    sdl3mcp_web(shared page)
    sdl3mcp_app(shared desktop_app)
elseif(SDL3MCP_WEB_CASE STREQUAL "no_html_artifact")
    sdl3mcp_web(demo binary)
elseif(SDL3MCP_WEB_CASE STREQUAL "missing_target")
    sdl3mcp_web(demo missing)
else()
    message(FATAL_ERROR "unknown SDL3MCP_WEB_CASE \"${SDL3MCP_WEB_CASE}\"")
endif()
