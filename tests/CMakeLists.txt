# Unit tests that include the private src/ headers link the library objects:
# a shared library hides those symbols from its dynamic symbol table. Tests
# that only use the public C API keep linking the library target itself.
if(SDL3MCP_BUILD_SHARED)
    set(SDL3MCP_TEST_INTERNAL_LIBRARY sdl3mcp_objects)
else()
    set(SDL3MCP_TEST_INTERNAL_LIBRARY SDL3MCP::SDL3MCP)
endif()

add_executable(sdl3mcp_test_header_c header_c.c)
set_target_properties(sdl3mcp_test_header_c PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(sdl3mcp_test_header_c PRIVATE c_std_11)
target_link_libraries(sdl3mcp_test_header_c PRIVATE SDL3MCP::SDL3MCP)
add_test(NAME header_c COMMAND sdl3mcp_test_header_c)

add_executable(sdl3mcp_test_header_cpp header_cpp.cpp)
target_compile_features(sdl3mcp_test_header_cpp PRIVATE cxx_std_20)
target_link_libraries(sdl3mcp_test_header_cpp PRIVATE SDL3MCP::SDL3MCP)
add_test(NAME header_cpp COMMAND sdl3mcp_test_header_cpp)

add_executable(sdl3mcp_test_configuration configuration_tests.cpp)
target_compile_features(sdl3mcp_test_configuration PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_configuration PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_configuration PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
)
add_test(NAME configuration_tests COMMAND sdl3mcp_test_configuration)

add_executable(sdl3mcp_test_keymap keymap_tests.cpp)
target_compile_features(sdl3mcp_test_keymap PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_keymap PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_keymap PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
)
add_test(NAME keymap_tests COMMAND sdl3mcp_test_keymap)

add_executable(sdl3mcp_test_input input_tests.cpp)
target_compile_features(sdl3mcp_test_input PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_input PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_input PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
)
add_test(NAME input_tests COMMAND sdl3mcp_test_input)

add_executable(sdl3mcp_test_input_json input_json_tests.cpp)
target_compile_features(sdl3mcp_test_input_json PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_input_json PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_input_json PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
)
add_test(NAME input_json_tests COMMAND sdl3mcp_test_input_json)

add_executable(sdl3mcp_test_replay replay_tests.cpp)
target_compile_features(sdl3mcp_test_replay PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_replay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_replay PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
)
add_test(NAME replay_tests COMMAND sdl3mcp_test_replay)

add_executable(sdl3mcp_test_mouse mouse_tests.cpp)
target_compile_features(sdl3mcp_test_mouse PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_mouse PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_mouse PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
)
add_test(NAME mouse_tests COMMAND sdl3mcp_test_mouse)

find_package(Threads REQUIRED)

add_executable(sdl3mcp_test_queue queue_tests.cpp)
target_compile_features(sdl3mcp_test_queue PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_queue PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_queue PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    Threads::Threads
)
add_test(NAME queue_tests COMMAND sdl3mcp_test_queue)

add_executable(sdl3mcp_test_app_input app_input_tests.cpp)
target_compile_features(sdl3mcp_test_app_input PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_app_input PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_app_input PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME app_input_tests COMMAND sdl3mcp_test_app_input)

add_executable(sdl3mcp_test_input_tool input_tool_tests.cpp)
target_compile_features(sdl3mcp_test_input_tool PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_input_tool PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_input_tool PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME input_tool_tests COMMAND sdl3mcp_test_input_tool)
# SDL's dummy video driver keeps process-wide allocations the test cannot free,
# so leak detection is scoped off for the dummy-driver integration tests only.
# Every other test runs with ASan leak detection on.
set_tests_properties(input_tool_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

add_executable(sdl3mcp_test_capture capture_tests.cpp)
target_compile_features(sdl3mcp_test_capture PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_capture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_capture PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME capture_tests COMMAND sdl3mcp_test_capture)

add_executable(sdl3mcp_test_capture_tool capture_tool_tests.cpp)
target_compile_features(sdl3mcp_test_capture_tool PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_capture_tool PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_capture_tool PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME capture_tool_tests COMMAND sdl3mcp_test_capture_tool)
set_tests_properties(capture_tool_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

add_executable(sdl3mcp_test_terminate terminate_tests.cpp)
target_compile_features(sdl3mcp_test_terminate PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_terminate PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_terminate PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME terminate_tests COMMAND sdl3mcp_test_terminate)
set_tests_properties(terminate_tests PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

add_executable(sdl3mcp_test_json_rpc json_rpc_tests.cpp)
target_compile_features(sdl3mcp_test_json_rpc PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_json_rpc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_json_rpc PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
)
add_test(NAME json_rpc_tests COMMAND sdl3mcp_test_json_rpc)

add_executable(sdl3mcp_test_session session_tests.cpp)
target_compile_features(sdl3mcp_test_session PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_session PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_session PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
)
add_test(NAME session_tests COMMAND sdl3mcp_test_session)

add_executable(sdl3mcp_test_frame frame_tests.cpp)
target_compile_features(sdl3mcp_test_frame PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_frame PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_frame PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
)
add_test(NAME frame_tests COMMAND sdl3mcp_test_frame)

add_executable(sdl3mcp_test_info info_tests.cpp)
target_compile_features(sdl3mcp_test_info PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_info PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_info PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
)
add_test(NAME info_tests COMMAND sdl3mcp_test_info)

add_executable(sdl3mcp_test_server server_tests.cpp)
target_compile_features(sdl3mcp_test_server PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_server PRIVATE
    ${SDL3MCP_TEST_INTERNAL_LIBRARY}
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME server_tests COMMAND sdl3mcp_test_server)

add_executable(sdl3mcp_test_lifecycle lifecycle_tests.cpp)
target_compile_features(sdl3mcp_test_lifecycle PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_lifecycle PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_lifecycle PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
    Threads::Threads
    nlohmann_json::nlohmann_json
)
# One lifecycle per process, so every scenario is its own CTest.
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
if(SDL3MCP_BUILD_SUPERVISOR)
    add_executable(sdl3mcp_fake_child supervisor_fake_child.cpp)
    target_compile_features(sdl3mcp_fake_child PRIVATE cxx_std_20)
    target_link_libraries(sdl3mcp_fake_child PRIVATE nlohmann_json::nlohmann_json)

    add_executable(sdl3mcp_test_supervisor supervisor_tests.cpp)
    target_compile_features(sdl3mcp_test_supervisor PRIVATE cxx_std_20)
    target_include_directories(sdl3mcp_test_supervisor PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(sdl3mcp_test_supervisor PRIVATE
        sdl3mcp_supervisor_objects
        sdl3mcp_objects
        SDL3::SDL3
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    add_test(NAME supervisor_tests
             COMMAND sdl3mcp_test_supervisor
                     $<TARGET_FILE:sdl3mcp-supervisor>
                     $<TARGET_FILE:sdl3mcp_fake_child>)
endif()
