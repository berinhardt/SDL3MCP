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
target_link_libraries(sdl3mcp_test_configuration PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
add_test(NAME configuration_tests COMMAND sdl3mcp_test_configuration)

add_executable(sdl3mcp_test_keymap keymap_tests.cpp)
target_compile_features(sdl3mcp_test_keymap PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_keymap PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_keymap PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
add_test(NAME keymap_tests COMMAND sdl3mcp_test_keymap)

add_executable(sdl3mcp_test_input input_tests.cpp)
target_compile_features(sdl3mcp_test_input PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_input PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_input PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
add_test(NAME input_tests COMMAND sdl3mcp_test_input)

add_executable(sdl3mcp_test_replay replay_tests.cpp)
target_compile_features(sdl3mcp_test_replay PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_replay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_replay PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
add_test(NAME replay_tests COMMAND sdl3mcp_test_replay)

add_executable(sdl3mcp_test_mouse mouse_tests.cpp)
target_compile_features(sdl3mcp_test_mouse PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_mouse PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_mouse PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
add_test(NAME mouse_tests COMMAND sdl3mcp_test_mouse)

find_package(Threads REQUIRED)

add_executable(sdl3mcp_test_queue queue_tests.cpp)
target_compile_features(sdl3mcp_test_queue PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_queue PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_queue PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
    Threads::Threads
)
add_test(NAME queue_tests COMMAND sdl3mcp_test_queue)

add_executable(sdl3mcp_test_json_rpc json_rpc_tests.cpp)
target_compile_features(sdl3mcp_test_json_rpc PRIVATE cxx_std_20)
target_include_directories(sdl3mcp_test_json_rpc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sdl3mcp_test_json_rpc PRIVATE
    SDL3MCP::SDL3MCP
    SDL3::SDL3
    nlohmann_json::nlohmann_json
)
add_test(NAME json_rpc_tests COMMAND sdl3mcp_test_json_rpc)
