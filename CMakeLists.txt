cmake_minimum_required(VERSION 3.21)

project(SDL3MCP VERSION 1.0.0 LANGUAGES C CXX)

if(WIN32)
    message(FATAL_ERROR "SDL3MCP: Windows support is a TODO (binary stdio, Win32 transport wake-up); see README Platforms.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SDL3MCP_BUILD_SHARED "Build SDL3MCP as a shared library" OFF)
option(SDL3MCP_BUILD_TESTS "Build the SDL3MCP tests" ${PROJECT_IS_TOP_LEVEL})
option(SDL3MCP_BUILD_EXAMPLES "Build the SDL3MCP examples" OFF)
# The browser arm's own suite: an Emscripten doctest executable that dials a
# local WebSocket server from Node. It is off by default because it needs the
# Emscripten toolchain and Node, and it is the only suite an Emscripten
# configure builds.
option(SDL3MCP_BUILD_NODE_TESTS "Build the Emscripten WebSocket suite run under Node.js" OFF)
if(SDL3MCP_BUILD_NODE_TESTS AND NOT EMSCRIPTEN)
    message(WARNING
        "SDL3MCP_BUILD_NODE_TESTS is only meaningful for an Emscripten "
        "configure; the WebSocket suite is not built")
endif()
set(SDL3MCP_WANTS_DOCTEST
    ${SDL3MCP_BUILD_TESTS}
)
if(EMSCRIPTEN AND SDL3MCP_BUILD_NODE_TESTS)
    set(SDL3MCP_WANTS_DOCTEST ON)
endif()

# A parent project that vendors doctest through add_subdirectory already defines
# the target, so the check comes first and the package lookup only runs for a
# standalone configure. Without doctest the suite would silently register no
# tests, which reads like a passing run, so this stops instead.
if(SDL3MCP_WANTS_DOCTEST AND NOT TARGET doctest::doctest)
    find_package(doctest CONFIG QUIET)
endif()
if(SDL3MCP_WANTS_DOCTEST AND NOT TARGET doctest::doctest)
    message(FATAL_ERROR "SDL3MCP: a test option is ON but doctest was not found; install doctest (Arch: pacman -S doctest) or configure with -DSDL3MCP_BUILD_TESTS=OFF")
endif()

option(SDL3MCP_BUILD_CLI "Build the sdl3mcp Handler executable" OFF)
option(SDL3MCP_USE_SYSTEM_JSON "Resolve nlohmann_json from the system before vendoring" ON)

# Executable targets opt into app_spawn_<alias> by registering here:
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

set(SDL3MCP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SDL3MCP_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP_version.h.in"
    "${SDL3MCP_GENERATED_DIR}/SDL3MCP_version.h"
    @ONLY
)

# The translation units live in object libraries so the two public consumers and
# the unit tests can compose them without a second copy of the sources:
#   * common: JSON-RPC, tools, the session, the stream, configuration and the
#     dial. It names SDL types but references no SDL symbol, so the Handler can
#     link it without dragging in a window.
#   * app: the C API, the runtime, and the input/replay/capture/info half. This
#     is the only half that links SDL.
#   * server: the Handler. It links common and never SDL.
# A shared build hides every C++ symbol; only the C API carries SDL_DECLSPEC.
set(sdl3mcp_common_sources
    src/Configuration.cpp
    src/Configuration-Arguments.cpp
    src/Configuration-Endpoint.cpp
    src/Configuration-Usage.cpp
    src/JsonRpc.cpp
    src/Session.cpp
    src/Stream.cpp
    src/ToolCatalog.cpp
    src/ToolDefinitions.cpp
    src/ToolResult.cpp
)
if(EMSCRIPTEN)
    # A browser has no descriptor and no second thread: the WebSocket arm
    # replaces the native socket and delivers lines from the page's callbacks.
    list(APPEND sdl3mcp_common_sources src/WebSocketStream.cpp)
else()
    list(APPEND sdl3mcp_common_sources src/SocketStream.cpp)
endif()
add_library(sdl3mcp_common_objects OBJECT ${sdl3mcp_common_sources})

add_library(sdl3mcp_app_objects OBJECT
    src/Api.cpp
    src/AppInput.cpp
    src/Capture.cpp
    src/Frame.cpp
    src/Info.cpp
    src/Input.cpp
    src/Input-Actions.cpp
    src/Input-Arguments.cpp
    src/Input-Chord.cpp
    src/Input-Keyboard.cpp
    src/Input-Modifiers.cpp
    src/Input-Pointer.cpp
    src/Input-Properties.cpp
    src/KeyMap.cpp
    src/Mouse.cpp
    src/Pipeline.cpp
    src/Replay.cpp
    src/Runtime.cpp
)

set(SDL3MCP_OBJECT_TARGETS sdl3mcp_common_objects sdl3mcp_app_objects)
foreach(sdl3mcp_object_target IN LISTS SDL3MCP_OBJECT_TARGETS)
    target_compile_features(${sdl3mcp_object_target} PRIVATE cxx_std_20)
    target_include_directories(${sdl3mcp_object_target}
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
        PRIVATE
            ${SDL3MCP_JSON_INCLUDE_DIRS}
    )
    target_link_libraries(${sdl3mcp_object_target} PRIVATE nlohmann_json::nlohmann_json)
    set_target_properties(${sdl3mcp_object_target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        POSITION_INDEPENDENT_CODE ON
    )
    # SDL's SDL_DECLSPEC expands to __declspec(dllexport) when DLL_EXPORT is
    # defined, which is what DEFINE_SYMBOL sets for a shared library. A static
    # build says SDL_STATIC_LIB so a Windows consumer sees no dllimport.
    if(SDL3MCP_BUILD_SHARED)
        target_compile_definitions(${sdl3mcp_object_target} PRIVATE DLL_EXPORT)
    else()
        target_compile_definitions(${sdl3mcp_object_target} PRIVATE SDL_STATIC_LIB)
    endif()
endforeach()

# The common half takes SDL's include path but not its link line: it includes no
# SDL header today, because the configuration types are SDL-free. Keeping SDL
# off its link line is what makes the Handler build without it.
target_include_directories(sdl3mcp_common_objects PRIVATE
    $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>
)

# The app half is the only half that needs SDL at link time.
target_link_libraries(sdl3mcp_app_objects PUBLIC SDL3::SDL3)

add_library(SDL3MCP ${sdl3mcp_library_type}
    $<TARGET_OBJECTS:sdl3mcp_common_objects>
    $<TARGET_OBJECTS:sdl3mcp_app_objects>
)
add_library(SDL3MCP::SDL3MCP ALIAS SDL3MCP)
target_include_directories(SDL3MCP
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(SDL3MCP PUBLIC SDL3::SDL3)
if(NOT SDL3MCP_BUILD_SHARED)
    # Part of the exported interface: a static consumer must not see dllimport.
    target_compile_definitions(SDL3MCP PUBLIC SDL_STATIC_LIB)
endif()
if(EMSCRIPTEN)
    # emscripten_websocket_* lives in Emscripten's JS library, and the final link
    # needs it named. As INTERFACE it reaches the application's link line, which
    # is where the WebSocket arm is pulled in from the archive.
    target_link_libraries(SDL3MCP INTERFACE "-lwebsocket.js")
endif()

# The SDL-free core both the library and the Handler compose. It is a build-tree
# target: the installed package exports the library only, so a consumer never
# needs the internal object libraries.
add_library(SDL3MCP_COMMON INTERFACE)
add_library(SDL3MCP::COMMON ALIAS SDL3MCP_COMMON)
set_target_properties(SDL3MCP_COMMON PROPERTIES EXPORT_NAME COMMON)
target_link_libraries(SDL3MCP_COMMON INTERFACE sdl3mcp_common_objects)
target_include_directories(SDL3MCP_COMMON INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    # The include path travels with COMMON while SDL stays off its link line;
    # the configuration types are SDL-free, so no member includes SDL.
    $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>
)

# The Handler: one executable with a client-facing stream (stdio, or the
# upstream dial in Relay mode) and one application listener. It links the
# SDL-free common core and never SDL3::SDL3, so a wire unit that starts calling
# SDL breaks this link instead of dragging a window into the Handler.
#
# SDL3MCP::SERVER is defined exactly when SDL3MCP_BUILD_CLI is ON on a desktop
# configure: the executable and its object library are the Handler, and there is
# no Emscripten Handler. The client-facing stdio stream is compiled here, not in
# the common core, so the library archive holds no stdio transport.
#
# Presentation lives here: the library sends raw RGBA and this tree encodes PNG
# and base64. stb is compiled in one server unit, so the library archive holds
# no stb symbol.
if(SDL3MCP_BUILD_CLI AND NOT EMSCRIPTEN)
    add_library(sdl3mcp_server_objects OBJECT
        src/StdioStream.cpp
        src/handler/AppPeer.cpp
        src/handler/Handler.cpp
        src/handler/HandlerCli.cpp
        src/handler/Listener.cpp
        src/handler/Png.cpp
        src/handler/StbImageWrite.cpp
        src/handler/Targets.cpp
        src/handler/Web.cpp
    )
    target_compile_features(sdl3mcp_server_objects PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_server_objects
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src/handler"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
            # The common core carries SDL's include path; the Handler itself
            # includes no SDL header and links no SDL symbol.
            $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(sdl3mcp_server_objects
        PUBLIC
            sdl3mcp_common_objects
            nlohmann_json::nlohmann_json
    )
    set_target_properties(sdl3mcp_server_objects PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        POSITION_INDEPENDENT_CODE ON
    )

    # Main.cpp stays out of the object library: the test suites link the server
    # objects and must not pick up a second main.
    add_executable(sdl3mcp src/handler/Main.cpp $<TARGET_OBJECTS:sdl3mcp_server_objects>)
    add_executable(SDL3MCP::SERVER ALIAS sdl3mcp)
    target_compile_features(sdl3mcp PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/handler"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(sdl3mcp PRIVATE
        sdl3mcp_common_objects
        nlohmann_json::nlohmann_json
    )
endif()

if(SDL3MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SDL3MCP_BUILD_TESTS OR (EMSCRIPTEN AND SDL3MCP_BUILD_NODE_TESTS))
    enable_testing()
    add_subdirectory(tests)
endif()

# Packaging only happens when SDL3MCP is the top-level project: an embedded
# build's SDL3 target is not in any export set, so an export here would fail to
# generate.
#
# Only the library is packaged: the Handler and the examples are build-tree
# artifacts, and COMMON is the internal composition target. nlohmann/json and
# stb stay private, so a consumer needs neither of them.
if(PROJECT_IS_TOP_LEVEL)
    install(TARGETS SDL3MCP EXPORT SDL3MCPTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3MCP_types.h"
        "${SDL3MCP_GENERATED_DIR}/SDL3MCP_version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
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
