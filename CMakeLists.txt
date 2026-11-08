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
option(SDL3MCP_BUILD_CLI "Build the sdl3mcp executable" OFF)
option(SDL3MCP_USE_SYSTEM_JSON "Resolve nlohmann_json from the system before vendoring" ON)

# Executable targets opt into spawner mode by registering here:
#   sdl3mcp_app(<alias> <target> [ARGS <arg>...])
# Registration only records into global properties. The write is deferred to the
# end of the top-level directory, so entries added from any subdirectory land in
# one file and nothing has to call the writer explicitly.
function(sdl3mcp_app sdl3mcp_alias sdl3mcp_target)
    if(NOT sdl3mcp_alias MATCHES "^[a-z0-9_]+$")
        message(FATAL_ERROR
            "sdl3mcp_app: alias \"${sdl3mcp_alias}\" must match [a-z0-9_]+")
    endif()
    if(sdl3mcp_alias MATCHES "^app_spawn_")
        message(FATAL_ERROR
            "sdl3mcp_app: alias \"${sdl3mcp_alias}\" must not use the reserved app_spawn_ prefix")
    endif()

    get_property(sdl3mcp_registered_aliases GLOBAL PROPERTY SDL3MCP_APP_ALIASES)
    if(sdl3mcp_alias IN_LIST sdl3mcp_registered_aliases)
        get_property(sdl3mcp_previous_target GLOBAL PROPERTY
            "SDL3MCP_APP_${sdl3mcp_alias}_TARGET")
        message(FATAL_ERROR
            "sdl3mcp_app: alias \"${sdl3mcp_alias}\" is already registered by target "
            "\"${sdl3mcp_previous_target}\"; cannot register \"${sdl3mcp_target}\"")
    endif()

    if(NOT TARGET "${sdl3mcp_target}")
        message(FATAL_ERROR
            "sdl3mcp_app: target \"${sdl3mcp_target}\" does not exist")
    endif()

    set(sdl3mcp_arguments)
    if(ARGC GREATER 2)
        if(NOT ARGV2 STREQUAL "ARGS")
            message(FATAL_ERROR
                "sdl3mcp_app: expected ARGS before \"${ARGV2}\"")
        endif()
        if(ARGC GREATER 3)
            math(EXPR sdl3mcp_last_argument "${ARGC} - 1")
            foreach(sdl3mcp_index RANGE 3 ${sdl3mcp_last_argument})
                list(APPEND sdl3mcp_arguments "${ARGV${sdl3mcp_index}}")
            endforeach()
        endif()
    endif()

    get_property(sdl3mcp_writer_scheduled GLOBAL PROPERTY SDL3MCP_APP_WRITER_SCHEDULED)
    set_property(GLOBAL APPEND PROPERTY SDL3MCP_APP_ALIASES "${sdl3mcp_alias}")
    set_property(GLOBAL PROPERTY "SDL3MCP_APP_${sdl3mcp_alias}_TARGET" "${sdl3mcp_target}")
    set_property(GLOBAL PROPERTY "SDL3MCP_APP_${sdl3mcp_alias}_ARGS" "${sdl3mcp_arguments}")

    if(NOT sdl3mcp_writer_scheduled)
        set_property(GLOBAL PROPERTY SDL3MCP_APP_WRITER_SCHEDULED TRUE)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL sdl3mcp_write_targets)
    endif()
endfunction()

function(sdl3mcp_write_targets)
    get_property(sdl3mcp_aliases GLOBAL PROPERTY SDL3MCP_APP_ALIASES)
    if(NOT sdl3mcp_aliases)
        return()
    endif()

    set(sdl3mcp_document "{")
    set(sdl3mcp_is_first TRUE)
    foreach(sdl3mcp_alias IN LISTS sdl3mcp_aliases)
        get_property(sdl3mcp_target GLOBAL PROPERTY
            "SDL3MCP_APP_${sdl3mcp_alias}_TARGET")
        get_property(sdl3mcp_arguments GLOBAL PROPERTY
            "SDL3MCP_APP_${sdl3mcp_alias}_ARGS")

        if(sdl3mcp_is_first)
            set(sdl3mcp_is_first FALSE)
        else()
            string(APPEND sdl3mcp_document ",")
        endif()
        string(APPEND sdl3mcp_document
            "\"${sdl3mcp_alias}\":[\"$<TARGET_FILE:${sdl3mcp_target}>\"")
        foreach(sdl3mcp_argument IN LISTS sdl3mcp_arguments)
            string(REPLACE "\\" "\\\\" sdl3mcp_argument "${sdl3mcp_argument}")
            string(REPLACE "\"" "\\\"" sdl3mcp_argument "${sdl3mcp_argument}")
            string(APPEND sdl3mcp_document ",\"${sdl3mcp_argument}\"")
        endforeach()
        string(APPEND sdl3mcp_document "]")
    endforeach()
    string(APPEND sdl3mcp_document "}")

    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/sdl3mcp-targets.json"
        CONTENT "${sdl3mcp_document}"
    )
endfunction()

# A parent project that vendors SDL through add_subdirectory already defines the
# target; reusing it keeps one SDL in the process and avoids the system install.
if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG REQUIRED)
endif()

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

# The translation units live in object libraries so unit tests that include
# the private src/ headers can link them directly: a shared library hides those
# symbols from its dynamic symbol table.
#
# They are split in two so the supervisor can link the wire half alone:
#   * wire: protocol, session, framing, transports and configuration; the relay
#     needs these and references no SDL symbol.
#   * app: everything that calls SDL (input, replay, capture plumbing, info).
# SDL3MCP keeps both halves, so the application sees no difference. The split is
# enforced by the link: if a wire unit starts calling SDL, sdl3mcp stops linking.
add_library(sdl3mcp_wire_objects OBJECT
    src/configuration.cpp
    src/frame.cpp
    src/capture.cpp
    src/json_rpc.cpp
    src/tools.cpp
    src/session.cpp
    src/server.cpp
    src/transport.cpp
    src/stdio_isolation.cpp
    src/socket_connect.cpp
)
add_library(sdl3mcp_app_objects OBJECT
    src/api.cpp
    src/capture_arguments.cpp
    src/keymap.cpp
    src/input.cpp
    src/input_json.cpp
    src/mouse.cpp
    src/replay.cpp
    src/app_input.cpp
    src/queue.cpp
    src/info.cpp
)
add_library(SDL3MCP ${sdl3mcp_library_type}
    $<TARGET_OBJECTS:sdl3mcp_wire_objects>
    $<TARGET_OBJECTS:sdl3mcp_app_objects>
)
add_library(SDL3MCP::SDL3MCP ALIAS SDL3MCP)

find_package(Threads REQUIRED)
set(SDL3MCP_OBJECT_TARGETS sdl3mcp_wire_objects sdl3mcp_app_objects)
foreach(sdl3mcp_object_target IN LISTS SDL3MCP_OBJECT_TARGETS)
    target_compile_features(${sdl3mcp_object_target} PRIVATE cxx_std_20)
    target_link_libraries(${sdl3mcp_object_target} PRIVATE Threads::Threads)
    set_target_properties(${sdl3mcp_object_target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        POSITION_INDEPENDENT_CODE ON
    )
    if(SDL3MCP_BUILD_SHARED)
        # SDL's SDL_DECLSPEC expands to __declspec(dllexport) when DLL_EXPORT is
        # defined, which is what DEFINE_SYMBOL sets for a shared library.
        target_compile_definitions(${sdl3mcp_object_target} PRIVATE DLL_EXPORT)
    else()
        target_compile_definitions(${sdl3mcp_object_target} PRIVATE SDL_STATIC_LIB)
    endif()
endforeach()

# The app half is the only one that needs SDL at link time.
target_link_libraries(sdl3mcp_app_objects PRIVATE SDL3::SDL3)
target_link_libraries(SDL3MCP
    PUBLIC SDL3::SDL3
    PRIVATE Threads::Threads
)

# The wire half includes the public header through configuration.hpp, which
# pulls in <SDL3/SDL.h> for its types and macros, but references no SDL symbol.
# It gets the include path only: linking SDL3::SDL3 here would put SDL back on
# the supervisor's link line and make it a build dependency of the relay.
target_include_directories(sdl3mcp_wire_objects PRIVATE
    $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>
)

set(SDL3MCP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SDL3MCP_GENERATED_DIR}/SDL3MCP")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP/SDL3MCP_version.h.in"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCP/SDL3MCP_version.h"
    @ONLY
)

foreach(sdl3mcp_object_target IN LISTS SDL3MCP_OBJECT_TARGETS)
    target_include_directories(${sdl3mcp_object_target}
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
        PRIVATE
            ${SDL3MCP_JSON_INCLUDE_DIRS}
    )
endforeach()
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

if(SDL3MCP_BUILD_CLI)
    # The supervisor is POSIX-only: the top-level WIN32 guard above already
    # rejects Windows, and the implementation uses fork/exec and private pipes.
    # The supervisor owns presentation: the library-to-supervisor link carries
    # raw pixels, so PNG and base64 are compiled here only. stb is a private
    # include of the supervisor objects, never of the library.
    add_library(sdl3mcp_supervisor_objects OBJECT
        supervisor/cli.cpp
        supervisor/child_process.cpp
        supervisor/child_peer.cpp
        supervisor/socket_peer.cpp
        supervisor/stdio_peer.cpp
        supervisor/targets.cpp
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
            sdl3mcp_wire_objects
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_executable(sdl3mcp supervisor/main.cpp)
    target_compile_features(sdl3mcp PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    # Object files from a linked object library do not propagate transitively, so
    # the wire objects are named here as well as in the supervisor objects. The
    # supervisor never calls SDL, so neither SDL3::SDL3 nor the app half is on
    # this link line; a wire unit that references SDL breaks the build here.
    target_link_libraries(sdl3mcp PRIVATE
        sdl3mcp_supervisor_objects
        sdl3mcp_wire_objects
    )
endif()

if(SDL3MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Packaging only happens when SDL3MCP is the top-level project: an embedded
# build's SDL3 target is not in any export set, so an export here would fail to
# generate.
if(PROJECT_IS_TOP_LEVEL)
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
endif()
