cmake_minimum_required(VERSION 3.21)

project(SDL3MCP VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SDL3MCP_BUILD_SHARED "Build SDL3MCP as a shared library" OFF)
option(SDL3MCP_BUILD_TESTS "Build the SDL3MCP smoke tests" ${PROJECT_IS_TOP_LEVEL})
option(SDL3MCP_BUILD_EXAMPLES "Build the SDL3MCP examples" OFF)
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

# nlohmann/json is header-only and private. Only its include path is taken, so
# the exported target never asks a consumer to have nlohmann installed.
get_target_property(SDL3MCP_JSON_INCLUDE_DIRS
    nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES
)
if(NOT SDL3MCP_JSON_INCLUDE_DIRS)
    set(SDL3MCP_JSON_INCLUDE_DIRS "")
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
    src/input_json.cpp
    src/mouse.cpp
    src/replay.cpp
    src/app_input.cpp
    src/queue.cpp
    src/json_rpc.cpp
    src/tools.cpp
    src/info.cpp
    src/session.cpp
    src/frame.cpp
    src/capture.cpp
    src/transport.cpp
    src/server.cpp
    src/stdio_isolation.cpp
    src/stb_image_write_impl.cpp
)
add_library(SDL3MCP::SDL3MCP ALIAS SDL3MCP)

target_compile_features(SDL3MCP PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(SDL3MCP
    PUBLIC SDL3::SDL3
    PRIVATE Threads::Threads
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
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories(SDL3MCP PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    ${SDL3MCP_JSON_INCLUDE_DIRS}
)

if(SDL3MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SDL3MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Only the library is packaged: tests and examples are never installed.
install(TARGETS SDL3MCP EXPORT SDL3MCPTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP/SDL3MCP.h"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCP/SDL3MCP_version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/SDL3MCP"
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SDL3MCPConfig.cmake.in"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCPConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SDL3MCP"
)
write_basic_package_version_file(
    "${SDL3MCP_GENERATED_DIR}/SDL3MCPConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${SDL3MCP_GENERATED_DIR}/SDL3MCPConfig.cmake"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCPConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SDL3MCP"
)

install(EXPORT SDL3MCPTargets
    NAMESPACE SDL3MCP::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SDL3MCP"
)
