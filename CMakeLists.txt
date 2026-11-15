cmake_minimum_required(VERSION 3.21)

project(SDL3MCP VERSION 1.0.0 LANGUAGES C CXX)

if(WIN32)
    message(FATAL_ERROR "SDL3MCP: Windows support is a TODO (binary stdio, Win32 transport wake-up); see README Platforms.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The library and the SDL app half. This is the only half that compiles SDL, so
# OFF leaves the Handler alone — the SDL-free common core, the Handler, and the
# Handler tests — and a machine without an SDL package can build and test it.
option(SDL3MCP_BUILD_LIBRARY
    "Build the SDL3MCP library and its SDL app half" ON)

# Tests are two groups by dependency, not by directory:
#   HANDLER_TESTS  handler_tests, web_tests, protocol_core_tests: every suite
#                  that links no SDL.
#   LIBRARY_TESTS  every suite that needs SDL to configure, build or run: the
#                  suites that link the library or the app half, plus the
#                  package checks (targets_check, readme_check), including the
#                  Emscripten Node suites.
# Neither group is an option: each is derived from the half it exercises, so a
# suite group exists exactly when that half is built. SDL3MCP_BUILD_TESTS is
# the single switch; a Handler-only configure gets HANDLER_TESTS and no SDL.
option(SDL3MCP_BUILD_TESTS
    "Build the test suites derived from the half each group exercises"
    ${PROJECT_IS_TOP_LEVEL})

option(SDL3MCP_BUILD_CLI "Build the sdl3mcp Handler executable" OFF)

set(SDL3MCP_HANDLER_TESTS FALSE)
set(SDL3MCP_LIBRARY_TESTS FALSE)
if(SDL3MCP_BUILD_TESTS)
    if(SDL3MCP_BUILD_CLI)
        set(SDL3MCP_HANDLER_TESTS TRUE)
    endif()
    if(SDL3MCP_BUILD_LIBRARY)
        set(SDL3MCP_LIBRARY_TESTS TRUE)
    endif()
endif()

if(SDL3MCP_HANDLER_TESTS OR SDL3MCP_LIBRARY_TESTS)
    set(SDL3MCP_TESTS_ENABLED TRUE)
else()
    set(SDL3MCP_TESTS_ENABLED FALSE)
endif()

# doctest is shared by both groups and resolves in three steps, like
# nlohmann/json: a target the parent project already defined, a package
# installed in the environment, then a pinned download. The download is what
# makes the gate reproducible on a machine with no doctest package: without a
# resolution step at all the suites would silently register no tests, which
# reads like a passing run, so this stops instead.
if(SDL3MCP_TESTS_ENABLED AND NOT TARGET doctest::doctest)
    find_package(doctest CONFIG QUIET)
endif()
if(SDL3MCP_TESTS_ENABLED AND NOT TARGET doctest::doctest)
    include(FetchContent)
    # The release tag pins the version this tree was built against; the hash
    # pins the archive.
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH
            SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(doctest)
endif()
if(SDL3MCP_TESTS_ENABLED AND NOT TARGET doctest::doctest)
    message(FATAL_ERROR
        "SDL3MCP: a test option is ON but doctest could not be resolved: "
        "define doctest::doctest, install its package, or allow the pinned "
        "v2.5.3 download")
endif()

# Executable targets opt into app_spawn_<alias> by registering here:
#   sdl3mcp_app(<alias> <target> [ARGS <arg>...])
# and Emscripten pages opt into the same tools with:
#   sdl3mcp_web(<alias> <target>)
# The functions live in a module so a configure-level test includes them without
# this build graph; see tests/web_registration_consumer.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SDL3MCPRegistration.cmake")

# A parent project that vendors SDL through add_subdirectory already defines the
# target; reusing it keeps one SDL in the process and avoids the system install.
#
# The lookup is conditional: the Handler and its suites compile no SDL, so a
# Handler-only configure must not require the package. It runs when the
# library half is on, and tests/handler_only_check.cmake proves the absence with
# CMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE.
#
# Reading that variable here is also what keeps a Handler-only configure from
# reporting it as unused, and the check names the one combination that cannot
# work: the library half asked for with its only provider disabled.
if(CMAKE_DISABLE_FIND_PACKAGE_SDL3
   AND NOT TARGET SDL3::SDL3
   AND SDL3MCP_BUILD_LIBRARY)
    message(FATAL_ERROR
        "SDL3MCP: the library half needs SDL, but "
        "CMAKE_DISABLE_FIND_PACKAGE_SDL3 is set and no SDL3::SDL3 target "
        "exists")
endif()
if(SDL3MCP_BUILD_LIBRARY)
    if(NOT TARGET SDL3::SDL3)
        find_package(SDL3 CONFIG REQUIRED)
    endif()
endif()

# nlohmann/json is header-only and private to the build. It resolves in three
# steps: a target the parent project already defined, then a package installed
# in the environment, then a pinned download. Only the include path is taken, so
# the exported target never asks a consumer to have nlohmann installed.
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    # The release tag pins the version the deleted vendored header carried.
    # Tests and install rules stay off: this is an include path, not a build.
    set(JSON_BuildTests OFF CACHE BOOL "")
    set(JSON_Install OFF CACHE BOOL "")
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
        URL_HASH
            SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR
        "SDL3MCP: nlohmann_json could not be resolved: define the "
        "nlohmann_json::nlohmann_json target, install its package, or allow "
        "the pinned FetchContent download")
endif()

get_target_property(SDL3MCP_JSON_INCLUDE_DIRS
    nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES
)
if(NOT SDL3MCP_JSON_INCLUDE_DIRS)
    set(SDL3MCP_JSON_INCLUDE_DIRS "")
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
#     dial. It compiles no SDL, so the Handler can link it without SDL.
#   * lib: the C API, the runtime, and the input/replay/capture/info half. This
#     is the only half that needs SDL.
#   * handler: the Handler. It links common and never SDL.
#
# Each directory is a bare include root for the targets that need it; there is
# no src/ catch-all, so a unit that reaches across halves names the directory it
# reaches into.
set(sdl3mcp_common_sources
    src/common/Configuration.cpp
    src/common/Configuration-Arguments.cpp
    src/common/Configuration-Endpoint.cpp
    src/common/Configuration-Usage.cpp
    src/common/JsonRpc.cpp
    src/common/Session.cpp
    src/common/Stream.cpp
    src/common/ToolCatalog.cpp
    src/common/ToolDefinitions.cpp
    src/common/ToolResult.cpp
)
if(EMSCRIPTEN)
    # A browser has no descriptor and no second thread: the WebSocket arm
    # replaces the native socket and delivers lines from the page's callbacks.
    # The arm is two units: the dial, writes and pump, plus the callbacks and
    # the byte queue they fill.
    list(APPEND sdl3mcp_common_sources
        src/common/SocketStream-Websockets.cpp
        src/common/SocketStream-WSEvents.cpp
    )
else()
    list(APPEND sdl3mcp_common_sources src/common/SocketStream.cpp)
endif()
add_library(sdl3mcp_common_objects OBJECT ${sdl3mcp_common_sources})

# The common and lib object libraries share a compile surface: the public
# headers, json's include path and hidden visibility. The lib library adds SDL
# afterwards, because it is the only half that needs it.
function(sdl3mcp_configure_object_target sdl3mcp_object_target)
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
endfunction()

# The common half is compiled with no SDL include path: no file in it may
# include SDL, and the configuration types are SDL-free. The Handler inherits
# that, so a stray SDL include in a common file fails the Handler build.
sdl3mcp_configure_object_target(sdl3mcp_common_objects)

# The lib half is the library's other half and the only code that names SDL. A
# Handler-only build leaves it out, so no SDL include path or target exists
# anywhere in that build.
if(SDL3MCP_BUILD_LIBRARY)
    add_library(sdl3mcp_lib_objects OBJECT
        src/lib/Api.cpp
        src/lib/AppInput.cpp
        src/lib/Capture.cpp
        src/lib/Frame.cpp
        src/lib/Info.cpp
        src/lib/Input.cpp
        src/lib/InputActions.cpp
        src/lib/InputArguments.cpp
        src/lib/Input-Chord.cpp
        src/lib/InputKeyboard.cpp
        src/lib/Input-Modifiers.cpp
        src/lib/InputPointer.cpp
        src/lib/InputProperties.cpp
        src/lib/KeyMap.cpp
        src/lib/Mouse.cpp
        src/lib/Pipeline.cpp
        src/lib/Replay.cpp
        src/lib/Runtime.cpp
        src/lib/TextInputScope.cpp
    )
    sdl3mcp_configure_object_target(sdl3mcp_lib_objects)
    # The lib half reaches the common headers by bare name; its own units are in
    # the same directory, so quoted includes resolve without an include path.
    target_include_directories(sdl3mcp_lib_objects PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    )
    # SDL is a compile-time dependency only: the final link is the app's job,
    # because the library publishes no SDL target.
    target_link_libraries(sdl3mcp_lib_objects PRIVATE SDL3::SDL3)

    add_library(SDL3MCP STATIC
        $<TARGET_OBJECTS:sdl3mcp_common_objects>
        $<TARGET_OBJECTS:sdl3mcp_lib_objects>
    )
    add_library(SDL3MCP::SDL3MCP ALIAS SDL3MCP)
    target_include_directories(SDL3MCP
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    # No target_link_libraries(SDL3MCP SDL3::SDL3): on a static library a PRIVATE
    # dependency is exported as $<LINK_ONLY:...>, so it would still publish SDL and
    # force find_dependency(SDL3) on every consumer. The app names SDL itself.
endif()

# The SDL-free core both the library and the Handler compose: a real static
# library built from exactly the common object library, so the Handler links one
# target instead of a bag of objects. It is a build-tree target: the installed
# package exports the library only, so a consumer never needs it.
add_library(SDL3MCP_COMMON STATIC $<TARGET_OBJECTS:sdl3mcp_common_objects>)
add_library(SDL3MCP::COMMON ALIAS SDL3MCP_COMMON)
set_target_properties(SDL3MCP_COMMON PROPERTIES EXPORT_NAME COMMON)
target_include_directories(SDL3MCP_COMMON INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SDL3MCP_GENERATED_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
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
# and base64. stb is compiled in one Handler unit, so the library archive holds
# no stb symbol.
if(SDL3MCP_BUILD_CLI AND NOT EMSCRIPTEN)
    add_library(sdl3mcp_handler_objects OBJECT
        src/handler/StdioStream.cpp
        src/handler/AppPeer.cpp
        src/handler/AppPeer-Protocol.cpp
        src/handler/Handler-Attach.cpp
        src/handler/Handler-Catalog.cpp
        src/handler/Handler-Deadline.cpp
        src/handler/Handler-DryRun.cpp
        src/handler/Handler-Forward.cpp
        src/handler/Handler-Lifecycle.cpp
        src/handler/Handler-Loop.cpp
        src/handler/Handler-Merge.cpp
        src/handler/Handler-Present.cpp
        src/handler/Handler-Reaper.cpp
        src/handler/Handler-Spawn.cpp
        src/handler/Handler-Wait.cpp
        src/handler/Handler-Web.cpp
        src/handler/HandlerCli-Endpoint.cpp
        src/handler/HandlerCli-Usage.cpp
        src/handler/HandlerCli.cpp
        src/handler/Listener.cpp
        src/handler/Png.cpp
        src/handler/StbImageWrite.cpp
        src/handler/Targets.cpp
        src/handler/Web-Apps.cpp
        src/handler/Web-Plan.cpp
        src/handler/Web-Plan-Preflight.cpp
        src/handler/Web-Plan-Profile.cpp
        src/handler/Web-Plan-Sweep.cpp
        src/handler/Web-Run.cpp
        src/handler/Web-Run-Child.cpp
        src/handler/Web-Run-Service.cpp
    )
    target_compile_features(sdl3mcp_handler_objects PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_handler_objects
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src/handler"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${SDL3MCP_GENERATED_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    )
    # SDL's include path is deliberately absent: no Handler unit includes SDL,
    # and a stray include must fail this build rather than pass review. COMMON
    # is the executable's link entry below, not this library's: the objects only
    # need the common headers to compile.
    target_link_libraries(sdl3mcp_handler_objects
        PRIVATE
            nlohmann_json::nlohmann_json
    )
    set_target_properties(sdl3mcp_handler_objects PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        POSITION_INDEPENDENT_CODE ON
    )

    # Main.cpp stays out of the object library: the test suites link the handler
    # objects and must not pick up a second main. The link block is the whole
    # Handler composition in one place: its objects, the SDL-free core, json.
    add_executable(sdl3mcp src/handler/Main.cpp)
    add_executable(SDL3MCP::SERVER ALIAS sdl3mcp)
    target_compile_features(sdl3mcp PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/handler"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    )
    target_link_libraries(sdl3mcp PRIVATE
        $<TARGET_OBJECTS:sdl3mcp_handler_objects>
        SDL3MCP_COMMON
        nlohmann_json::nlohmann_json
    )
endif()

if(SDL3MCP_TESTS_ENABLED)
    enable_testing()
    add_subdirectory(tests)
endif()

# Packaging only happens when SDL3MCP is the top-level project: an embedded
# build's SDL3 target is not in any export set, so an export here would fail to
# generate. A Handler-only build has no library to export, so it packages
# nothing.
#
# Only the library is packaged: the Handler is a build-tree artifact, and
# COMMON is the internal composition target. nlohmann/json and
# stb stay private, so a consumer needs neither of them.
if(PROJECT_IS_TOP_LEVEL AND SDL3MCP_BUILD_LIBRARY)
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
