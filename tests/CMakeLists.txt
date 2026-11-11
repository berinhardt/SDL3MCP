# Unit tests that include the private src/ headers link the library objects
# (wire and app halves): a shared library hides those symbols from its dynamic
# symbol table. Tests that only use the public C API keep linking the library
# target itself.
if(SDL3MCP_BUILD_SHARED)
    set(SDL3MCP_TEST_INTERNAL_LIBRARY sdl3mcp_wire_objects sdl3mcp_app_objects)
else()
    set(SDL3MCP_TEST_INTERNAL_LIBRARY SDL3MCP::SDL3MCP)
endif()

add_executable(sdl3mcp_test_header_c header_c.c)
set_target_properties(sdl3mcp_test_header_c PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(sdl3mcp_test_header_c PRIVATE c_std_11)
target_link_libraries(sdl3mcp_test_header_c PRIVATE SDL3MCP::SDL3MCP)
add_test(NAME header_c COMMAND sdl3mcp_test_header_c)

find_package(Threads REQUIRED)

# One doctest suite is one executable, so a failing case is reported by doctest
# and the remaining cases still run, unlike a hand-rolled main that aborts at
# the first assertion. The private src/ include and the object-library link let
# a suite reach the implementation the public header hides. A suite named
# <family>_tests builds the executable sdl3mcp_test_<family>; the name without
# the suffix, as for header_cpp, keeps the executable name as it is.
#
# Options:
#   LIBRARY <target>  library or object library to link instead of the internal
#                     one, for suites that only use the public C API.
#   LIBS <targets>    extra link libraries such as nlohmann_json or Threads.
#   INCLUDES <dirs>   extra include directories beyond ../src.
#   NO_TEST           create the executable without registering a test, for a
#                     suite that needs its own CTest command or registers
#                     several tests, as lifecycle_tests does.
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

sdl3mcp_add_test(header_cpp header_cpp.cpp)

sdl3mcp_add_test(configuration_tests configuration_tests.cpp)

sdl3mcp_add_test(keymap_tests keymap_tests.cpp)

sdl3mcp_add_test(input_tests input_tests.cpp)

sdl3mcp_add_test(input_json_tests input_json_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(replay_tests replay_tests.cpp)

sdl3mcp_add_test(mouse_tests mouse_tests.cpp)

sdl3mcp_add_test(connect_tests connect_tests.cpp
    LIBS Threads::Threads)

sdl3mcp_add_test(queue_tests queue_tests.cpp
    LIBS Threads::Threads)

sdl3mcp_add_test(app_input_tests app_input_tests.cpp
    LIBS nlohmann_json::nlohmann_json Threads::Threads)

sdl3mcp_add_test(input_tool_tests input_tool_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
# SDL's dummy video driver keeps process-wide allocations the test cannot free,
# so leak detection is scoped off for the dummy-driver integration tests only.
# Every other test runs with ASan leak detection on.
set_tests_properties(input_tool_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

sdl3mcp_add_test(text_input_tests text_input_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
set_tests_properties(text_input_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

sdl3mcp_add_test(capture_tests capture_tests.cpp
    LIBS nlohmann_json::nlohmann_json Threads::Threads)

sdl3mcp_add_test(capture_tool_tests capture_tool_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
set_tests_properties(capture_tool_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

sdl3mcp_add_test(frame_size_tests frame_size_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
set_tests_properties(frame_size_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

sdl3mcp_add_test(terminate_tests terminate_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS nlohmann_json::nlohmann_json Threads::Threads)
set_tests_properties(terminate_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

sdl3mcp_add_test(json_rpc_tests json_rpc_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(session_tests session_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(frame_tests frame_tests.cpp)

sdl3mcp_add_test(info_tests info_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

sdl3mcp_add_test(server_tests server_tests.cpp
    LIBS nlohmann_json::nlohmann_json Threads::Threads)

sdl3mcp_add_test(responder_tests responder_tests.cpp
    LIBS nlohmann_json::nlohmann_json)

# One lifecycle per process, so every scenario is its own CTest. The executable
# selects its case from the scenario argument.
sdl3mcp_add_test(lifecycle_tests lifecycle_tests.cpp
    LIBRARY SDL3MCP::SDL3MCP
    LIBS Threads::Threads nlohmann_json::nlohmann_json
    NO_TEST)
foreach(scenario none help version error uninitialized stdio_pipes)
    add_test(NAME lifecycle_${scenario}
             COMMAND sdl3mcp_test_lifecycle ${scenario})
endforeach()

# The README is the contract, so its code, schemas, and run command are checked.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME readme_check
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/check_readme.py"
                     --source "${PROJECT_SOURCE_DIR}"
                     --build "${PROJECT_BINARY_DIR}")
endif()

# The supervisor tests drive the real executable over pipes, with a fake child
# that speaks MCP without SDL, so no window or video driver is involved.
if(SDL3MCP_BUILD_CLI)
    sdl3mcp_add_test(supervisor_png_tests supervisor_png_tests.cpp
        LIBRARY sdl3mcp_supervisor_objects
        LIBS sdl3mcp_wire_objects sdl3mcp_app_objects SDL3::SDL3 Threads::Threads
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../supervisor")

    add_executable(sdl3mcp_fake_child supervisor_fake_child.cpp)
    target_compile_features(sdl3mcp_fake_child PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_fake_child PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(sdl3mcp_fake_child PRIVATE
        sdl3mcp_wire_objects
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    sdl3mcp_add_test(supervisor_tests supervisor_tests.cpp
        LIBRARY sdl3mcp_supervisor_objects
        LIBS sdl3mcp_wire_objects sdl3mcp_app_objects SDL3::SDL3
             nlohmann_json::nlohmann_json Threads::Threads
        NO_TEST)
    add_test(NAME supervisor_tests
             COMMAND sdl3mcp_test_supervisor
                     $<TARGET_FILE:sdl3mcp>
                     $<TARGET_FILE:sdl3mcp_fake_child>)

    # The relay drives the same executable with --listen and a fake application
    # that dials in over TCP, so no child process and no SDL are involved.
    sdl3mcp_add_test(relay_tests relay_tests.cpp
        LIBRARY sdl3mcp_supervisor_objects
        LIBS sdl3mcp_wire_objects sdl3mcp_app_objects SDL3::SDL3
             nlohmann_json::nlohmann_json Threads::Threads
        NO_TEST)
    add_test(NAME relay_tests
             COMMAND sdl3mcp_test_relay
                     $<TARGET_FILE:sdl3mcp>)

    # The spawner dials a relay the test itself listens on, then spawns the fake
    # child through its own per-spawn listener; the fake child dials back with
    # --mcp=<endpoint> and speaks MCP like the real app.
    sdl3mcp_add_test(spawner_tests spawner_tests.cpp
        LIBRARY sdl3mcp_supervisor_objects
        LIBS sdl3mcp_wire_objects sdl3mcp_app_objects SDL3::SDL3
             nlohmann_json::nlohmann_json Threads::Threads
        INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../supervisor"
        NO_TEST)
    add_test(NAME spawner_tests
             COMMAND sdl3mcp_test_spawner
                     $<TARGET_FILE:sdl3mcp>
                     $<TARGET_FILE:sdl3mcp_fake_child>)

    # The same binary again, this time driving the real example through the
    # proxy. The dummy video driver gives a real software renderer headless;
    # as with the other dummy-driver tests, SDL's process-wide allocations are
    # not leaks the example can free, so leak detection is scoped off here and
    # inherited by both the supervisor and the app it spawns.
    if(SDL3MCP_BUILD_EXAMPLES)
        add_test(NAME supervisor_real_app
                 COMMAND sdl3mcp_test_supervisor
                         $<TARGET_FILE:sdl3mcp>
                         $<TARGET_FILE:sdl3mcp_fake_child>
                         $<TARGET_FILE:minimal>)
        set_tests_properties(supervisor_real_app PROPERTIES ENVIRONMENT
            "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")
    endif()
endif()
