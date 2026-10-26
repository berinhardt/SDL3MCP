cmake_minimum_required(VERSION 3.21)

project(SDL3MCP VERSION 0.1.0 LANGUAGES C CXX)

option(SDL3MCP_BUILD_SHARED "Build SDL3MCP as a shared library" OFF)
option(SDL3MCP_BUILD_TESTS "Build the SDL3MCP smoke tests" ${PROJECT_IS_TOP_LEVEL})
option(SDL3MCP_USE_SYSTEM_JSON "Resolve nlohmann_json from the system before vendoring" ON)

find_package(SDL3 CONFIG REQUIRED)

if(SDL3MCP_USE_SYSTEM_JSON)
    find_package(nlohmann_json CONFIG QUIET)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(sdl3mcp_json INTERFACE)
    target_include_directories(sdl3mcp_json INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    )
    add_library(nlohmann_json::nlohmann_json ALIAS sdl3mcp_json)
endif()

if(SDL3MCP_BUILD_SHARED)
    set(sdl3mcp_library_type SHARED)
else()
    set(sdl3mcp_library_type STATIC)
endif()

add_library(SDL3MCP ${sdl3mcp_library_type}
    src/api.cpp
    src/configuration.cpp
    src/keymap.cpp
    src/input.cpp
    src/mouse.cpp
    src/replay.cpp
    src/queue.cpp
    src/json_rpc.cpp
    src/tools.cpp
    src/session.cpp
    src/frame.cpp
    src/stb_image_write_impl.cpp
)
add_library(SDL3MCP::SDL3MCP ALIAS SDL3MCP)

target_compile_features(SDL3MCP PRIVATE cxx_std_20)
target_link_libraries(SDL3MCP
    PUBLIC SDL3::SDL3
    PRIVATE nlohmann_json::nlohmann_json
)
set_target_properties(SDL3MCP PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON
)

if(SDL3MCP_BUILD_SHARED)
    # SDL's SDL_DECLSPEC expands to __declspec(dllexport) when DLL_EXPORT is
    # defined, which is what DEFINE_SYMBOL sets for shared builds.
    set_target_properties(SDL3MCP PROPERTIES DEFINE_SYMBOL "DLL_EXPORT")
else()
    target_compile_definitions(SDL3MCP PRIVATE SDL_STATIC_LIB)
endif()

set(SDL3MCP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SDL3MCP_GENERATED_DIR}/SDL3MCP")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP/SDL3MCP_version.h.in"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCP/SDL3MCP_version.h"
    @ONLY
)

target_include_directories(SDL3MCP PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SDL3MCP_GENERATED_DIR}"
)
target_include_directories(SDL3MCP PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
)

if(SDL3MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
