# Registration for the Handler's two app sources.
#
# This is a module rather than a block in CMakeLists.txt because the rules are
# configure-time behavior: a tiny project includes it and asserts every
# diagnostic, which a function defined in the middle of the main file cannot
# offer. See tests/web_registration_consumer and tests/check_web_registration.cmake.
#
# An executable target opts into app_spawn_<alias> with:
#   sdl3mcp_app(<alias> <target> [ARGS <arg>...])
# and an Emscripten page opts into the same tool with:
#   sdl3mcp_web(<alias> <target>)
#
# Registration only records into global properties. The write is deferred to the
# end of the top-level directory, so entries added from any subdirectory land in
# one file and nothing has to call the writer explicitly. The write is also what
# reads the target, so a registration may come before the target's artifact
# properties are set.

# Both functions share one alias namespace, so the grammar, the reserved prefix
# and the duplicate check live here. `registration` is the caller's name, which
# keeps the diagnostic pointing at the function the user called.
function(sdl3mcp_require_alias registration alias target)
    if(NOT alias MATCHES "^[a-z0-9_]+$")
        message(FATAL_ERROR
            "${registration}: alias \"${alias}\" must match [a-z0-9_]+")
    endif()
    if(alias MATCHES "^app_spawn_")
        message(FATAL_ERROR
            "${registration}: alias \"${alias}\" must not use the reserved app_spawn_ prefix")
    endif()

    get_property(sdl3mcp_aliases GLOBAL PROPERTY SDL3MCP_ALIASES)
    if(alias IN_LIST sdl3mcp_aliases)
        get_property(sdl3mcp_previous_target GLOBAL PROPERTY
            "SDL3MCP_ALIAS_${alias}_TARGET")
        message(FATAL_ERROR
            "${registration}: alias \"${alias}\" is already registered by target "
            "\"${sdl3mcp_previous_target}\"; cannot register \"${target}\"")
    endif()
endfunction()

# `kind` is APP or WEB: the shared properties answer "who took this alias", and
# the per-kind list is the write order.
function(sdl3mcp_record_alias alias target kind)
    set_property(GLOBAL APPEND PROPERTY SDL3MCP_ALIASES "${alias}")
    set_property(GLOBAL PROPERTY "SDL3MCP_ALIAS_${alias}_TARGET" "${target}")
    set_property(GLOBAL APPEND PROPERTY "SDL3MCP_${kind}_ALIASES" "${alias}")
endfunction()

function(sdl3mcp_schedule_writer)
    get_property(sdl3mcp_scheduled GLOBAL PROPERTY SDL3MCP_WRITER_SCHEDULED)
    if(sdl3mcp_scheduled)
        return()
    endif()
    set_property(GLOBAL PROPERTY SDL3MCP_WRITER_SCHEDULED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL sdl3mcp_write_registration)
endfunction()

function(sdl3mcp_app alias target)
    sdl3mcp_require_alias(sdl3mcp_app "${alias}" "${target}")

    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "sdl3mcp_app: target \"${target}\" does not exist")
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

    sdl3mcp_record_alias("${alias}" "${target}" APP)
    set_property(GLOBAL PROPERTY "SDL3MCP_APP_${alias}_ARGS"
        "${sdl3mcp_arguments}")
    sdl3mcp_schedule_writer()
endfunction()

function(sdl3mcp_web alias target)
    sdl3mcp_require_alias(sdl3mcp_web "${alias}" "${target}")

    # A desktop binary is spawned from --targets; only a browser page has a web
    # root, so this is a build mistake rather than a runtime one.
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "sdl3mcp_web: alias \"${alias}\" with target \"${target}\" needs an "
            "Emscripten configure; only an Emscripten page is served by --web")
    endif()

    sdl3mcp_record_alias("${alias}" "${target}" WEB)
    sdl3mcp_schedule_writer()
endfunction()

function(sdl3mcp_write_registration)
    sdl3mcp_write_targets()
    sdl3mcp_write_web()
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
            "SDL3MCP_ALIAS_${sdl3mcp_alias}_TARGET")
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

# What a web registration needs from its target. The call records only the
# alias; this runs in the deferred write, because a target's artifact properties
# may be set after it is registered and the end of the top-level directory is
# the first moment they are final.
function(sdl3mcp_require_web_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "sdl3mcp_web: target \"${target}\" does not exist")
    endif()

    get_target_property(sdl3mcp_target_type "${target}" TYPE)
    if(NOT sdl3mcp_target_type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR
            "sdl3mcp_web: target \"${target}\" is a ${sdl3mcp_target_type}, and "
            "a web app is an executable with an .html artifact")
    endif()

    # The page is the artifact a browser loads, so a target without one has no
    # root to serve. The Emscripten app shape sets SUFFIX; the default is a .js
    # for a Node environment, which is a program, not a page. A generator
    # expression has no configure-time value to compare, so it is left to the
    # generate step rather than rejected here.
    get_target_property(sdl3mcp_target_suffix "${target}" SUFFIX)
    if(NOT sdl3mcp_target_suffix MATCHES "\\$<" AND
       NOT sdl3mcp_target_suffix STREQUAL ".html")
        message(FATAL_ERROR
            "sdl3mcp_web: target \"${target}\" has SUFFIX "
            "\"${sdl3mcp_target_suffix}\", not \".html\"; give it an .html "
            "artifact with set_target_properties(${target} PROPERTIES SUFFIX \".html\")")
    endif()
endfunction()

# The web file maps an alias to the page root: the directory the Handler serves
# with --web, which is where the .html and the .js/.wasm it names were written.
function(sdl3mcp_write_web)
    get_property(sdl3mcp_aliases GLOBAL PROPERTY SDL3MCP_WEB_ALIASES)
    if(NOT sdl3mcp_aliases)
        return()
    endif()

    set(sdl3mcp_document "{")
    set(sdl3mcp_is_first TRUE)
    foreach(sdl3mcp_alias IN LISTS sdl3mcp_aliases)
        get_property(sdl3mcp_target GLOBAL PROPERTY
            "SDL3MCP_ALIAS_${sdl3mcp_alias}_TARGET")
        sdl3mcp_require_web_target("${sdl3mcp_target}")

        if(sdl3mcp_is_first)
            set(sdl3mcp_is_first FALSE)
        else()
            string(APPEND sdl3mcp_document ",")
        endif()
        string(APPEND sdl3mcp_document
            "\"${sdl3mcp_alias}\":\"$<TARGET_FILE_DIR:${sdl3mcp_target}>\"")
    endforeach()
    string(APPEND sdl3mcp_document "}")

    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/sdl3mcp-web.json"
        CONTENT "${sdl3mcp_document}"
    )
endfunction()
