# Unit tests.
#
# A suite that includes the private src/ headers links the common archive and
# the app object library: the sources are compiled once for the library, and
# the suite reaches internal symbols the C ABI does not name. A suite that uses
# only the public C API links SDL3MCP::SDL3MCP, so the public surface is
# exercised too. A suite that links an object library names SDL itself, because
# the library publishes no SDL.
#
# A browser configure has no POSIX test suite. Its suites are the WebSocket
# transport arm and the whole app path, both run under Node against a local
# server. They are registered whenever tests are on: an Emscripten configure has
# no other suite, so there is no separate option to remember.
if(EMSCRIPTEN)
    find_program(NODE_EXECUTABLE node)
    if(NOT NODE_EXECUTABLE)
        message(WARNING
            "node was not found; the Emscripten suites run under Node and are "
            "not registered")
        return()
    endif()

    # Both suites link the real archive, so the WebSocket arm is pulled from the
    # object the application links. The library publishes no SDL, so each suite
    # adds SDL and -lwebsocket.js itself, exactly like a consumer's Emscripten
    # link line.
    add_executable(sdl3mcp_test_websocket_node
        websocket_stream_node_tests.cpp)
    target_compile_features(sdl3mcp_test_websocket_node PRIVATE
        cxx_std_20)
    target_include_directories(sdl3mcp_test_websocket_node PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(sdl3mcp_test_websocket_node PRIVATE
        SDL3MCP::SDL3MCP
        SDL3::SDL3
        "-lwebsocket.js"
        doctest::doctest
    )
    # ASYNCIFY is what lets the browser event loop run between pumps, and
    # EXIT_RUNTIME is what makes main's verdict end the Node process.
    target_link_options(sdl3mcp_test_websocket_node PRIVATE
        "-sASYNCIFY"
        "-sEXIT_RUNTIME=1"
        "-sENVIRONMENT=node"
    )
    add_test(NAME node_websocket_tests
             COMMAND "${NODE_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/run_node_websocket_test.js"
                     "$<TARGET_FILE:sdl3mcp_test_websocket_node>")

    # The app half of the browser path: the dummy video driver supplies a real
    # window, the archive supplies the protocol, and a test-only post-js file
    # supplies the two page globals.
    add_executable(sdl3mcp_test_web_app_node web_app_node_tests.cpp)
    target_compile_features(sdl3mcp_test_web_app_node PRIVATE
        cxx_std_20)
    target_link_libraries(sdl3mcp_test_web_app_node PRIVATE
        SDL3MCP::SDL3MCP
        SDL3::SDL3
        "-lwebsocket.js"
        doctest::doctest
    )
    target_link_options(sdl3mcp_test_web_app_node PRIVATE
        "-sASYNCIFY"
        "-sEXIT_RUNTIME=1"
        "-sENVIRONMENT=node"
        # Post-js, never pre-js: a window that exists at module load flips
        # ENVIRONMENT_IS_WEB and trips the -sENVIRONMENT=node assert.
        "--post-js=${CMAKE_CURRENT_SOURCE_DIR}/node_page_globals.js"
    )
    add_test(NAME node_web_app_tests
             COMMAND "${NODE_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/run_node_web_app_test.js"
                     "$<TARGET_FILE:sdl3mcp_test_web_app_node>")
    return()
endif()

find_package(Threads REQUIRED)

# The private half of the library: protocol, session, tools, configuration and
# the app-side pipeline. The common archive and the app object library keep the
# suite reachable to private symbols.
set(SDL3MCP_TEST_INTERNAL_LIBRARY SDL3MCP_COMMON sdl3mcp_app_objects)

# One doctest suite is one executable, so a failing case is reported by doctest
# and the remaining cases still run. A suite named <family>_tests builds the
# executable sdl3mcp_test_<family>; a name without the suffix keeps the
# executable name as it is.
#
# Options:
#   LIBRARY <targets>  library or object libraries to link instead of the
#                      internal one, for suites that use only the public C API.
#   LIBS <targets>     extra link libraries such as nlohmann_json or Threads.
#   INCLUDES <dirs>    extra include directories beyond ../src.
#   NO_TEST            create the executable without registering a test, for a
#                      suite that needs its own CTest command or registers
#                      several tests, as library_lifecycle_tests does.
function(sdl3mcp_add_test sdl3mcp_test_name sdl3mcp_test_source)
    cmake_parse_arguments(sdl3mcp_test "NO_TEST" "LIBRARY" "LIBS;INCLUDES" ${ARGN})

    string(REGEX REPLACE "_tests$" "" sdl3mcp_test_family "${sdl3mcp_test_name}")
    set(sdl3mcp_test_target "sdl3mcp_test_${sdl3mcp_test_family}")

    if(NOT sdl3mcp_test_LIBRARY)
        set(sdl3mcp_test_LIBRARY ${SDL3MCP_TEST_INTERNAL_LIBRARY})
    endif()

    add_executable(${sdl3mcp_test_target} ${sdl3mcp_test_source})
    target_compile_features(${sdl3mcp_test_target} PRIVATE cxx_std_20)
    target_include_directories(${sdl3mcp_test_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        ${sdl3mcp_test_INCLUDES}
    )
    target_link_libraries(${sdl3mcp_test_target} PRIVATE
        ${sdl3mcp_test_LIBRARY}
        SDL3::SDL3
        doctest::doctest
        ${sdl3mcp_test_LIBS}
    )
    if(NOT sdl3mcp_test_NO_TEST)
        add_test(NAME ${sdl3mcp_test_name} COMMAND ${sdl3mcp_test_target})
    endif()
endfunction()

# The public header compiles as C11 and as C++20, and the release is 1.0.0.
# The C test is a C-compiled consumer, so it names SDL itself: the library
# publishes no SDL include path.
add_executable(sdl3mcp_test_library_header_c library_header_c.c)
set_target_properties(sdl3mcp_test_library_header_c PROPERTIES
    LINKER_LANGUAGE CXX)
target_compile_features(sdl3mcp_test_library_header_c PRIVATE c_std_11)
target_link_libraries(sdl3mcp_test_library_header_c PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
)
add_test(NAME library_header_c COMMAND sdl3mcp_test_library_header_c)

sdl3mcp_add_test(header_cpp header_cpp.cpp
    LIBRARY SDL3MCP::SDL3MCP)

# The protocol core is SDL-free, so its suite links the common library alone: a
# wire unit that starts calling SDL breaks this build instead of dragging a
# window and a GPU into a protocol test.
add_executable(sdl3mcp_test_protocol_core protocol_core_tests.cpp)
target_compile_features(sdl3mcp_test_protocol_core PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_protocol_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${SDL3MCP_GENERATED_DIR}"
    ${SDL3MCP_JSON_INCLUDE_DIRS}
)
target_link_libraries(sdl3mcp_test_protocol_core PRIVATE
    SDL3MCP_COMMON
    nlohmann_json::nlohmann_json
    doctest::doctest
)
add_test(NAME protocol_core_tests COMMAND sdl3mcp_test_protocol_core)

# The Handler end to end: the real executable over pipes and sockets, driven by
# a test MCP client and a fake application that speaks the library's wire
# protocol. No SDL is on this link line, so a server unit that starts calling
# SDL breaks the test build instead of dragging a window into the Handler.
if(TARGET sdl3mcp)
    add_executable(sdl3mcp_test_handler handler_tests.cpp)
    target_compile_features(sdl3mcp_test_handler PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_test_handler PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/handler"
    )
    target_link_libraries(sdl3mcp_test_handler PRIVATE
        sdl3mcp_server_objects
        SDL3MCP_COMMON
        nlohmann_json::nlohmann_json
        doctest::doctest
        Threads::Threads
    )

    # The fake target a --targets entry spawns: it dials the appended
    # --mcp=HOST:PORT and speaks the library's wire protocol, so the spawn path
    # is exercised end to end without SDL.
    add_executable(sdl3mcp_handler_fake_app handler_fake_app.cpp)
    target_compile_features(sdl3mcp_handler_fake_app PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_handler_fake_app PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(sdl3mcp_handler_fake_app PRIVATE
        SDL3MCP_COMMON
        nlohmann_json::nlohmann_json
    )

    # The controllable page server and WebSocket bridge a web spawn starts in
    # place of python3 and websocat: no Python, no websocat, no browser.
    add_executable(sdl3mcp_test_web_stack web_stack_helper.cpp)
    target_compile_features(sdl3mcp_test_web_stack PRIVATE cxx_std_20)
    target_link_libraries(sdl3mcp_test_web_stack PRIVATE Threads::Threads)

    add_test(NAME handler_tests
             COMMAND sdl3mcp_test_handler $<TARGET_FILE:sdl3mcp>
                     $<TARGET_FILE:sdl3mcp_handler_fake_app>
                     $<TARGET_FILE:sdl3mcp_test_web_stack>)

    # The web source and the throwaway profile are pure value/filesystem
    # operations, so their suite links the server objects without a Handler
    # process or a browser.
    sdl3mcp_add_test(web_tests web_tests.cpp
        LIBRARY sdl3mcp_server_objects
        LIBS Threads::Threads
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../src/handler")
endif()

# The pure value halves: key names, input planning, the replay state, mouse
# normalization, and frame validation/crop.
sdl3mcp_add_test(keymap_tests keymap_tests.cpp)

sdl3mcp_add_test(input_tests input_tests.cpp)

sdl3mcp_add_test(replay_tests replay_tests.cpp)

sdl3mcp_add_test(mouse_tests mouse_tests.cpp)

sdl3mcp_add_test(app_input_tests app_input_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(frame_tests frame_tests.cpp)

# The app-thread pipeline with a recording completion. SDL's dummy video driver
# supplies the window a bound input batch needs.
sdl3mcp_add_test(pipeline_tests pipeline_tests.cpp
    LIBS nlohmann_json::nlohmann_json)
set_tests_properties(pipeline_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

# The 1.0 configuration grammar and the two argument parsers are pure: a value
# in, a value and a message out, with no window or socket.
sdl3mcp_add_test(library_configuration_tests library_configuration_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(library_info_tests library_info_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(library_arguments_tests library_arguments_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

# One lifecycle per process, so every scenario is its own CTest and no scenario
# can leak the claimed lifecycle into another.
sdl3mcp_add_test(library_lifecycle_tests library_lifecycle_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS Threads::Threads
    NO_TEST)
foreach(sdl3mcp_lifecycle_scenario
        off no_endpoint off_clears_endpoint refused_endpoint second_init
        uninitialized help version bad_argument)
    add_test(NAME library_lifecycle_${sdl3mcp_lifecycle_scenario}
             COMMAND sdl3mcp_test_library_lifecycle
                     ${sdl3mcp_lifecycle_scenario})
endforeach()

# The test app dials a test MCP server on loopback, with the server in its own
# thread. SDL's dummy video driver keeps process-wide allocations the test
# cannot free, so leak detection is scoped off for these integration tests only.
sdl3mcp_add_test(library_socket_tests library_socket_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
set_tests_properties(library_socket_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

# The full input/capture pipeline: a capture is a no-op input, several requests
# registered in one pump share the next frame, a region crops it, and a rejected
# WriteFrame leaves the request pending.
sdl3mcp_add_test(library_pipeline_tests library_pipeline_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
set_tests_properties(library_pipeline_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

# The public CMake surface: a parent project must see the three public target
# names. The consumer embeds SDL3MCP as a subdirectory and asserts them, so the
# check fails if a name disappears or is renamed. It configures a tree of its
# own, so it only runs when SDL3 came from find_package and a standalone
# configure can find it again; a parent that vendors SDL through
# add_subdirectory leaves SDL3_DIR unset and skips the check rather than
# failing it for an environment it does not control.
if(SDL3_DIR)
    get_filename_component(sdl3mcp_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_test(NAME targets_check
             COMMAND "${CMAKE_COMMAND}"
                     -S "${CMAKE_CURRENT_SOURCE_DIR}/targets_consumer"
                     -B "${CMAKE_CURRENT_BINARY_DIR}/targets_consumer"
                     "-DSDL3MCP_SOURCE_DIR=${sdl3mcp_source_dir}"
                     "-DSDL3_DIR=${SDL3_DIR}")
endif()

# The web registration is configure-time behavior, so it is checked by
# configuring a tiny project rather than by a compiled suite. The script runs one
# configure per case and asserts the file on success and the diagnostic on each
# failure, which a FATAL_ERROR makes impossible to observe from inside.
add_test(NAME web_registration_check
         COMMAND "${CMAKE_COMMAND}"
                 "-DSDL3MCP_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/.."
                 "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/web_registration"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/check_web_registration.cmake")

# The README is the contract, so its code, schemas, and run command are checked.
# The Handler's --dump-tools supplies the catalog the check compares against, so
# the guard reads the same definitions tools/list does.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(sdl3mcp_readme_arguments
        --source "${PROJECT_SOURCE_DIR}"
        --build "${PROJECT_BINARY_DIR}"
    )
    if(TARGET sdl3mcp)
        list(APPEND sdl3mcp_readme_arguments --tools $<TARGET_FILE:sdl3mcp>)
    endif()
    add_test(NAME readme_check
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/check_readme.py"
                     ${sdl3mcp_readme_arguments})
endif()
