# The public-target consumer: it embeds SDL3MCP the way a parent project does
# and asserts the three public target names are visible. The Handler is gated on
# SDL3MCP_BUILD_CLI, so the consumer turns it on to see all three.
#
# It also states the build graph the README documents: the library is static
# only and publishes no SDL (no target, include path, compile definition or web
# link flag), COMMON is a real static library built from the common object
# library, and the Handler's build carries no SDL include path.
cmake_minimum_required(VERSION 3.21)

project(sdl3mcp_targets_consumer LANGUAGES C CXX)

# The consumer owns SDL's discovery: SDL3MCP publishes no SDL dependency, so the
# app names the SDL target it links on its own link line.
find_package(SDL3 CONFIG REQUIRED)

set(SDL3MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SDL3MCP_BUILD_CLI ON CACHE BOOL "" FORCE)
add_subdirectory("${SDL3MCP_SOURCE_DIR}" sdl3mcp)

foreach(sdl3mcp_public_target SDL3MCP::SDL3MCP SDL3MCP::SERVER SDL3MCP::COMMON)
    if(NOT TARGET "${sdl3mcp_public_target}")
        message(FATAL_ERROR
            "SDL3MCP: public target ${sdl3mcp_public_target} is missing")
    endif()
endforeach()

if(NOT TARGET sdl3mcp)
    message(FATAL_ERROR "SDL3MCP: the Handler executable target sdl3mcp is missing")
endif()

# The library is static only.
get_target_property(sdl3mcp_library_type SDL3MCP::SDL3MCP TYPE)
if(NOT sdl3mcp_library_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
        "SDL3MCP: the library must be static only, found ${sdl3mcp_library_type}")
endif()

# COMMON is a library with sources, not an interface shim over an object
# library.
get_target_property(sdl3mcp_common_type SDL3MCP::COMMON TYPE)
if(NOT sdl3mcp_common_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
        "SDL3MCP: COMMON must be a real static library, found ${sdl3mcp_common_type}")
endif()
get_target_property(sdl3mcp_common_sources SDL3MCP::COMMON SOURCES)
if(NOT sdl3mcp_common_sources)
    message(FATAL_ERROR "SDL3MCP: COMMON has no sources")
endif()

# SDL carries its include dirs on SDL3::Headers (linked from the library
# target), so collect every SDL target's include dirs rather than only the
# alias's.
set(sdl3_include_dirs)
foreach(sdl3_include_target SDL3::Headers SDL3::SDL3 SDL3::SDL3-static)
    if(TARGET "${sdl3_include_target}")
        get_target_property(sdl3_target_include_dirs "${sdl3_include_target}"
            INTERFACE_INCLUDE_DIRECTORIES)
        if(sdl3_target_include_dirs)
            list(APPEND sdl3_include_dirs ${sdl3_target_include_dirs})
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES sdl3_include_dirs)
if(NOT sdl3_include_dirs)
    message(FATAL_ERROR
        "SDL3MCP: SDL publishes no include directory to compare against")
endif()

# The library's interface names no SDL target and no Emscripten web link flag.
get_target_property(sdl3mcp_library_link_libraries SDL3MCP::SDL3MCP
    INTERFACE_LINK_LIBRARIES)
if(sdl3mcp_library_link_libraries)
    foreach(sdl3mcp_entry IN LISTS sdl3mcp_library_link_libraries)
        if(sdl3mcp_entry MATCHES "^SDL3::" OR sdl3mcp_entry MATCHES "websocket\\.js")
            message(FATAL_ERROR
                "SDL3MCP: the library publishes ${sdl3mcp_entry}; "
                "the consumer links SDL itself")
        endif()
    endforeach()
endif()

# No linkage macro travels with the library: it is static only.
get_target_property(sdl3mcp_library_compile_definitions SDL3MCP::SDL3MCP
    INTERFACE_COMPILE_DEFINITIONS)
if(sdl3mcp_library_compile_definitions)
    foreach(sdl3mcp_definition IN LISTS sdl3mcp_library_compile_definitions)
        if(sdl3mcp_definition STREQUAL "DLL_EXPORT" OR
           sdl3mcp_definition STREQUAL "SDL_STATIC_LIB")
            message(FATAL_ERROR
                "SDL3MCP: the library publishes the linkage macro "
                "${sdl3mcp_definition}")
        endif()
    endforeach()
endif()

function(sdl3mcp_assert_no_sdl_include_dirs target)
    get_target_property(sdl3mcp_target_include_dirs "${target}" INCLUDE_DIRECTORIES)
    get_target_property(sdl3mcp_target_interface_include_dirs "${target}"
        INTERFACE_INCLUDE_DIRECTORIES)
    foreach(sdl3mcp_include_dir IN LISTS sdl3mcp_target_include_dirs
            sdl3mcp_target_interface_include_dirs)
        # A generator expression naming SDL publishes the path even though the
        # literal directory cannot be compared here.
        if(sdl3mcp_include_dir MATCHES "SDL3::")
            message(FATAL_ERROR
                "SDL3MCP: ${target} carries SDL's include path "
                "${sdl3mcp_include_dir}")
        endif()
    endforeach()
    foreach(sdl3_include_dir IN LISTS sdl3_include_dirs)
        if(sdl3_include_dir IN_LIST sdl3mcp_target_include_dirs)
            message(FATAL_ERROR
                "SDL3MCP: ${target} compiles with SDL's include path "
                "${sdl3_include_dir}")
        endif()
        if(sdl3_include_dir IN_LIST sdl3mcp_target_interface_include_dirs)
            message(FATAL_ERROR
                "SDL3MCP: ${target} publishes SDL's include path "
                "${sdl3_include_dir}")
        endif()
    endforeach()
endfunction()

# The library, COMMON and the Handler's objects carry no SDL include path, so a
# stray SDL include in the SDL-free half fails that build instead of passing
# review.
sdl3mcp_assert_no_sdl_include_dirs(SDL3MCP::SDL3MCP)
sdl3mcp_assert_no_sdl_include_dirs(SDL3MCP::COMMON)
sdl3mcp_assert_no_sdl_include_dirs(sdl3mcp_handler_objects)

# The Handler executable links COMMON, never SDL. The object library carries the
# sources, so the link block is where the two halves meet.
get_target_property(sdl3mcp_handler_link_libraries sdl3mcp LINK_LIBRARIES)
set(sdl3mcp_links_common FALSE)
if(sdl3mcp_handler_link_libraries)
    foreach(sdl3mcp_entry IN LISTS sdl3mcp_handler_link_libraries)
        if(sdl3mcp_entry MATCHES "^SDL3::")
            message(FATAL_ERROR
                "SDL3MCP: the Handler links ${sdl3mcp_entry}; it compiles no SDL")
        endif()
        if(sdl3mcp_entry STREQUAL "SDL3MCP_COMMON")
            set(sdl3mcp_links_common TRUE)
        endif()
    endforeach()
endif()
if(NOT sdl3mcp_links_common)
    message(FATAL_ERROR
        "SDL3MCP: the Handler does not link SDL3MCP_COMMON")
endif()
