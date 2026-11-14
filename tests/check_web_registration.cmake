# Checks the web registration at its seam: configure tests/web_registration_consumer
# once per case and inspect the exit status, the diagnostic and the file the
# build writes. A FATAL_ERROR ends the configure that raises it, so every failure
# is its own configure and each one is started from here.
#
# Inputs, both passed by tests/CMakeLists.txt:
#   SDL3MCP_SOURCE_DIR  the tree whose cmake/SDL3MCPRegistration.cmake is checked
#   WORK_DIR            one build directory per case, emptied before it is used
cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED SDL3MCP_SOURCE_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR
        "check_web_registration: SDL3MCP_SOURCE_DIR and WORK_DIR are required")
endif()

set(sdl3mcp_consumer "${SDL3MCP_SOURCE_DIR}/tests/web_registration_consumer")

# Configures one case and returns its combined output. `emscripten` selects
# whether the case runs on an Emscripten configure, which is the only configure a
# web registration is valid in.
function(run_case case emscripten expect)
    set(build "${WORK_DIR}/${case}")
    file(REMOVE_RECURSE "${build}")

    set(command "${CMAKE_COMMAND}"
        -S "${sdl3mcp_consumer}"
        -B "${build}"
        "-DSDL3MCP_SOURCE_DIR=${SDL3MCP_SOURCE_DIR}"
        "-DSDL3MCP_WEB_CASE=${case}")
    if(emscripten)
        list(APPEND command -DEMSCRIPTEN=TRUE)
    endif()

    execute_process(COMMAND ${command}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    set(text "${output}\n${error}")

    if(expect STREQUAL "fail")
        if(result EQUAL 0)
            message(FATAL_ERROR
                "check_web_registration: case ${case} configured when it must fail")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR
            "check_web_registration: case ${case} failed to configure:\n${text}")
    endif()

    set(CASE_OUTPUT "${text}" PARENT_SCOPE)
    set(CASE_BUILD "${build}" PARENT_SCOPE)
endfunction()

# A diagnostic names the thing it rejects: the alias, the target or the reason.
function(expect_named text context)
    foreach(name IN LISTS ARGN)
        string(FIND "${text}" "${name}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR
                "check_web_registration: ${context} does not name \"${name}\":\n${text}")
        endif()
    endforeach()
endfunction()

# One entry of the written file: the alias maps to the target's output
# directory, which is the root --web serves.
function(expect_entry build alias)
    set(sdl3mcp_web_file "${build}/sdl3mcp-web.json")
    if(NOT EXISTS "${sdl3mcp_web_file}")
        message(FATAL_ERROR
            "check_web_registration: ${sdl3mcp_web_file} was not written")
    endif()
    file(READ "${sdl3mcp_web_file}" sdl3mcp_document)
    string(JSON sdl3mcp_root ERROR_VARIABLE sdl3mcp_error
        GET "${sdl3mcp_document}" ${alias})
    if(sdl3mcp_error)
        message(FATAL_ERROR
            "check_web_registration: sdl3mcp-web.json has no \"${alias}\" entry: ${sdl3mcp_error}")
    endif()
    if(NOT sdl3mcp_root STREQUAL "${build}")
        message(FATAL_ERROR
            "check_web_registration: \"${alias}\" is \"${sdl3mcp_root}\", not the target's output directory \"${build}\"")
    endif()
endfunction()

# The success case: one accumulated file, one entry per registration, each entry
# the target's output directory.
run_case(success TRUE pass)
foreach(sdl3mcp_alias demo other)
    expect_entry("${CASE_BUILD}" ${sdl3mcp_alias})
endforeach()

# The artifact properties are what the write reads, not the call: a target whose
# SUFFIX is set after it is registered is the normal shape, and a suffix only
# the generate step can evaluate has nothing the configure can judge.
run_case(suffix_after_registration TRUE pass)
expect_entry("${CASE_BUILD}" demo)

run_case(suffix_generator_expression TRUE pass)
expect_entry("${CASE_BUILD}" demo)

# A desktop configure has no page root to serve, so the call stops and says why.
run_case(non_emscripten FALSE fail)
expect_named("${CASE_OUTPUT}" "case non_emscripten"
    "sdl3mcp_web" "demo" "page" "Emscripten")

run_case(bad_alias TRUE fail)
expect_named("${CASE_OUTPUT}" "case bad_alias" "Bad-Alias" "[a-z0-9_]+")

run_case(reserved_prefix TRUE fail)
expect_named("${CASE_OUTPUT}" "case reserved_prefix"
    "app_spawn_demo" "reserved app_spawn_")

run_case(duplicate_web_alias TRUE fail)
expect_named("${CASE_OUTPUT}" "case duplicate_web_alias"
    "demo" "already registered by target" "page" "page2")

run_case(app_alias_taken TRUE fail)
expect_named("${CASE_OUTPUT}" "case app_alias_taken"
    "shared" "already registered by target" "desktop_app")

run_case(web_alias_taken TRUE fail)
expect_named("${CASE_OUTPUT}" "case web_alias_taken"
    "shared" "already registered by target" "page")

run_case(no_html_artifact TRUE fail)
expect_named("${CASE_OUTPUT}" "case no_html_artifact" "binary" ".html")

run_case(missing_target TRUE fail)
expect_named("${CASE_OUTPUT}" "case missing_target" "missing" "does not exist")

message(STATUS "check_web_registration: ok")
