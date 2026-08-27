# Keep every runner implementation header inside the runtime. Authored and
# generated game code must use the namespaced public SDK under
# `include/snesrecomp`; the private-header list is derived from the source tree
# so a newly added implementation header cannot silently bypass this check.

if(NOT DEFINED GAME_SOURCE_ROOT)
  message(FATAL_ERROR "GAME_SOURCE_ROOT is required")
endif()
if(NOT DEFINED SNESRECOMP_RUNNER_PRIVATE_ROOT)
  message(FATAL_ERROR "SNESRECOMP_RUNNER_PRIVATE_ROOT is required")
endif()

file(GLOB_RECURSE _private_headers
  LIST_DIRECTORIES false
  "${SNESRECOMP_RUNNER_PRIVATE_ROOT}/*.h")
set(_private_include_names "")
foreach(_header IN LISTS _private_headers)
  file(RELATIVE_PATH _relative
    "${SNESRECOMP_RUNNER_PRIVATE_ROOT}" "${_header}")
  get_filename_component(_basename "${_header}" NAME)
  list(APPEND _private_include_names "${_relative}" "${_basename}")
endforeach()
list(REMOVE_DUPLICATES _private_include_names)
list(APPEND _private_include_names
  "apu_sync.h"
  "common_cpu_infra.h"
  "common_rtl.h"
  "cpu_state.h"
  "cpu_trace.h"
  "runtime_constants.h"
  "snes_regs.h"
  "types.h")

file(GLOB_RECURSE _sources
  LIST_DIRECTORIES false
  "${GAME_SOURCE_ROOT}/*.c"
  "${GAME_SOURCE_ROOT}/*.h")

set(_violations "")
foreach(_source IN LISTS _sources)
  file(READ "${_source}" _contents)
  string(REGEX MATCHALL
    "#[ \t]*include[ \t]*[\"<][^\">]+[\">]" _includes "${_contents}")
  foreach(_directive IN LISTS _includes)
    string(REGEX REPLACE
      ".*[\"<]([^\">]+)[\">].*" "\\1" _include "${_directive}")
    if(_include MATCHES "^snesrecomp/")
      continue()
    endif()
    if(EXISTS "${GAME_SOURCE_ROOT}/${_include}")
      continue()
    endif()
    if(_include IN_LIST _private_include_names)
      file(RELATIVE_PATH _source_relative "${GAME_SOURCE_ROOT}" "${_source}")
      list(APPEND _violations "${_source_relative}: ${_include}")
    endif()
  endforeach()
endforeach()
list(SORT _violations)

if(_violations)
  message(FATAL_ERROR
    "Application code includes private runner headers: ${_violations}\n"
    "Use a namespaced header under snesrecomp/ or extend the public SDK.")
endif()

list(LENGTH _violations _count)
message(STATUS "Private runner boundary: ${_count} application violations")
