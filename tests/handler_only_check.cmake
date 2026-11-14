# Proves the Handler half is SDL-free by configuring a tree of its own: the
# library and its suites are off, SDL discovery is disabled outright, and the
# Handler and the Handler suites must configure, build and pass.
#
# CMAKE_DISABLE_FIND_PACKAGE_SDL3 turns any find_package(SDL3) into a configure
# error, so the configure itself is the check: an unconditional SDL lookup, or
# an SDL target on a Handler link line, fails here instead of passing review.
#
# Inputs, all passed by tests/CMakeLists.txt:
#   SDL3MCP_SOURCE_DIR  the tree to configure
#   WORK_DIR            the throwaway tree; emptied before every run
#   CTEST_COMMAND       ctest beside the CMake that runs this script
#   GENERATOR           the parent's generator, so the nested build uses the
#                       same toolchain
#   BUILD_TYPE          the parent's CMAKE_BUILD_TYPE
#   DOCTEST_DIR         the package the parent found, when it found one
#   DOCTEST_SOURCE_DIR  the doctest the parent fetched, when it downloaded one
#   NLOHMANN_JSON_DIR / NLOHMANN_JSON_SOURCE_DIR  the same for nlohmann/json
#
# Forwarding the dependencies is what keeps the fixture off the network: on a
# machine whose parent configure already resolved them, the nested configure
# must not download them again.
#
# The nested tree registers this fixture too, so the configure passes
# -DSDL3MCP_HANDLER_ONLY_FIXTURE=OFF and the recursion stops there.
cmake_minimum_required(VERSION 3.21)

foreach(sdl3mcp_required SDL3MCP_SOURCE_DIR WORK_DIR CTEST_COMMAND)
    if(NOT DEFINED ${sdl3mcp_required})
        message(FATAL_ERROR "handler_only_check: ${sdl3mcp_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
set(sdl3mcp_build "${WORK_DIR}/build")

set(sdl3mcp_configure
    "${CMAKE_COMMAND}"
    -S "${SDL3MCP_SOURCE_DIR}"
    -B "${sdl3mcp_build}"
    "-DSDL3MCP_BUILD_LIBRARY=OFF"
    "-DSDL3MCP_BUILD_CLI=ON"
    "-DSDL3MCP_BUILD_TESTS=OFF"
    "-DSDL3MCP_BUILD_HANDLER_TESTS=ON"
    "-DSDL3MCP_BUILD_LIBRARY_TESTS=OFF"
    "-DSDL3MCP_HANDLER_ONLY_FIXTURE=OFF"
    "-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE"
)
if(GENERATOR)
    list(APPEND sdl3mcp_configure -G "${GENERATOR}")
endif()
if(BUILD_TYPE)
    list(APPEND sdl3mcp_configure "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
if(DOCTEST_DIR AND EXISTS "${DOCTEST_DIR}/doctestConfig.cmake")
    list(APPEND sdl3mcp_configure "-Ddoctest_DIR=${DOCTEST_DIR}")
elseif(DOCTEST_SOURCE_DIR)
    list(APPEND sdl3mcp_configure
        "-DFETCHCONTENT_SOURCE_DIR_DOCTEST=${DOCTEST_SOURCE_DIR}")
endif()
if(NLOHMANN_JSON_DIR AND EXISTS "${NLOHMANN_JSON_DIR}/nlohmann_jsonConfig.cmake")
    list(APPEND sdl3mcp_configure "-Dnlohmann_json_DIR=${NLOHMANN_JSON_DIR}")
elseif(NLOHMANN_JSON_SOURCE_DIR)
    list(APPEND sdl3mcp_configure
        "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}")
endif()

# Run one step and report every stream on failure: a nested configure that
# stops on a CMake error writes the diagnostic to stderr, and a test that hides
# it reads like a broken toolchain.
function(sdl3mcp_step description)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE sdl3mcp_result
        OUTPUT_VARIABLE sdl3mcp_output
        ERROR_VARIABLE sdl3mcp_error)
    if(NOT sdl3mcp_result EQUAL 0)
        message(FATAL_ERROR
            "handler_only_check: ${description} failed (${sdl3mcp_result}):\n"
            "${sdl3mcp_output}\n${sdl3mcp_error}")
    endif()
endfunction()

sdl3mcp_step(configure ${sdl3mcp_configure})
sdl3mcp_step(build "${CMAKE_COMMAND}" --build "${sdl3mcp_build}" --parallel)
sdl3mcp_step(ctest "${CTEST_COMMAND}" --test-dir "${sdl3mcp_build}"
    --output-on-failure)

message(STATUS "handler_only_check: ok")
