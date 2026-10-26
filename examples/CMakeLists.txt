add_executable(minimal minimal/main.c)
set_target_properties(minimal PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
target_link_libraries(minimal PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
