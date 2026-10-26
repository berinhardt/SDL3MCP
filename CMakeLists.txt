cmake_minimum_required(VERSION 3.21)

project(SDL3MCP VERSION 0.1.0 LANGUAGES C CXX)

if(WIN32)
    message(FATAL_ERROR "SDL3MCP: Windows support is a TODO (binary stdio, Win32 transport wake-up, stdio isolation); see README Platforms.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SDL3MCP_BUILD_SHARED "Build SDL3MCP as a shared library" OFF)
option(SDL3MCP_BUILD_TESTS "Build the SDL3MCP smoke tests" ${PROJECT_IS_TOP_LEVEL})
option(SDL3MCP_BUILD_EXAMPLES "Build the SDL3MCP examples" OFF)
option(SDL3MCP_BUILD_SUPERVISOR "Build the sdl3mcp-supervisor executable" OFF)
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

# The translation units live in an object library so unit tests that include
# the private src/ headers can link them directly: a shared library hides those
# symbols from its dynamic symbol table.
add_library(sdl3mcp_objects OBJECT
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
)
add_library(SDL3MCP ${sdl3mcp_library_type} $<TARGET_OBJECTS:sdl3mcp_objects>)
add_library(SDL3MCP::SDL3MCP ALIAS SDL3MCP)

target_compile_features(sdl3mcp_objects PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(sdl3mcp_objects PRIVATE SDL3::SDL3 Threads::Threads)
target_link_libraries(SDL3MCP
    PUBLIC SDL3::SDL3
    PRIVATE Threads::Threads
)
set_target_properties(sdl3mcp_objects PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON
)

if(SDL3MCP_BUILD_SHARED)
    # SDL's SDL_DECLSPEC expands to __declspec(dllexport) when DLL_EXPORT is
    # defined, which is what DEFINE_SYMBOL sets for a shared library.
    target_compile_definitions(sdl3mcp_objects PRIVATE DLL_EXPORT)
else()
    target_compile_definitions(sdl3mcp_objects PRIVATE SDL_STATIC_LIB)
endif()

set(SDL3MCP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SDL3MCP_GENERATED_DIR}/SDL3MCP")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP/SDL3MCP_version.h.in"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCP/SDL3MCP_version.h"
    @ONLY
)

target_include_directories(sdl3mcp_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
    PRIVATE
        ${SDL3MCP_JSON_INCLUDE_DIRS}
)
target_include_directories(SDL3MCP PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories(SDL3MCP PRIVATE
    ${SDL3MCP_JSON_INCLUDE_DIRS}
)

if(SDL3MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SDL3MCP_BUILD_SUPERVISOR)
    # The supervisor is POSIX-only: the top-level WIN32 guard above already
    # rejects Windows, and the implementation uses fork/exec and private pipes.
    # The supervisor owns presentation: the library-to-supervisor link carries
    # raw pixels, so downscaling, PNG, and base64 are compiled here only. stb is
    # a private include of the supervisor objects, never of the library.
    add_library(sdl3mcp_supervisor_objects OBJECT
        supervisor/cli.cpp
        supervisor/child_process.cpp
        supervisor/scaling.cpp
        supervisor/png.cpp
        supervisor/stb_image_write_impl.cpp
        supervisor/supervisor.cpp
    )
    target_compile_features(sdl3mcp_supervisor_objects PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_supervisor_objects
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/supervisor"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    )
    target_link_libraries(sdl3mcp_supervisor_objects
        PUBLIC
            sdl3mcp_objects
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_executable(sdl3mcp-supervisor supervisor/main.cpp)
    target_compile_features(sdl3mcp-supervisor PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp-supervisor PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    # Object files from a linked object library do not propagate transitively, so
    # the library objects are named here as well as in the supervisor objects.
    # The object library's own link dependencies are private, hence SDL3 too.
    target_link_libraries(sdl3mcp-supervisor PRIVATE
        sdl3mcp_supervisor_objects
        sdl3mcp_objects
        SDL3::SDL3
    )
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
