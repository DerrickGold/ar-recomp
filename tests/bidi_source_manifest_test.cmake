# A pinned dependency update must not silently omit a translation unit from
# the shipped Go builder. CMake's desktop backend reads this same manifest.
file(STRINGS "${GAME_ROOT}/snesbuild.ini" _lines
    REGEX "^source = third_party/sheenbidi/.*[.]c$")
set(_listed)
foreach(_line IN LISTS _lines)
    string(REGEX REPLACE "^source = " "" _file "${_line}")
    list(APPEND _listed "${_file}")
endforeach()
file(GLOB_RECURSE _expected RELATIVE "${GAME_ROOT}"
    "${GAME_ROOT}/third_party/sheenbidi/Source/*.c")
list(REMOVE_ITEM _expected "third_party/sheenbidi/Source/SheenBidi.c")
list(SORT _expected)
list(SORT _listed)
if(NOT _expected OR NOT _listed STREQUAL _expected)
    message(FATAL_ERROR "SheenBidi sources differ from the shared game manifest")
endif()
file(STRINGS "${GAME_ROOT}/snesbuild.ini" _unity REGEX "^define = SB_CONFIG_UNITY")
if(_unity)
    message(FATAL_ERROR "SheenBidi must use separate tracked translation units")
endif()
