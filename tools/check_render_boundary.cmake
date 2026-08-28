if(NOT DEFINED GAME_SOURCE_ROOT)
    message(FATAL_ERROR "GAME_SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE _portable_render_files
    "${GAME_SOURCE_ROOT}/render/*.c"
    "${GAME_SOURCE_ROOT}/render/*.h")
list(APPEND _portable_render_files
    "${GAME_SOURCE_ROOT}/presentation_upload_mirror.c"
    "${GAME_SOURCE_ROOT}/presentation_upload_mirror.h"
    "${GAME_SOURCE_ROOT}/hd_replacement_host.c"
    "${GAME_SOURCE_ROOT}/hd_replacement_host.h"
    "${GAME_SOURCE_ROOT}/hd_replacements.h")

set(_violations "")
foreach(_file IN LISTS _portable_render_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "#[ \t]*include[ \t]*[<\"]SDL" OR
       _contents MATCHES "SDL_[A-Za-z0-9_]+")
        list(APPEND _violations "${_file}")
    endif()
endforeach()

if(_violations)
    list(JOIN _violations "\n  " _formatted)
    message(FATAL_ERROR
        "Portable rendering code depends on SDL:\n  ${_formatted}\n"
        "Move native types and calls behind src/platform/sdl/.")
endif()
