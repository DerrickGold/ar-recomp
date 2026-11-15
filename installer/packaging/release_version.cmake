include_guard(GLOBAL)

# Keep release tags verbatim: compact tags such as v0900 and dotted tags such
# as v1.0 are identifiers, not numbers to reformat. Resolve once per release
# and pass the same stamp to every platform and Git-free remote snapshot.
function(builder_resolve_release_version out_name)
    set(_version "${SNESBUILD_VERSION}")
    if(_version STREQUAL "")
        find_package(Git QUIET)
        if(GIT_FOUND)
            get_filename_component(_repo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
                WORKING_DIRECTORY "${_repo}"
                RESULT_VARIABLE _git_result
                OUTPUT_VARIABLE _version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(NOT _git_result EQUAL 0)
                set(_version "")
            endif()
        endif()
        if(_version STREQUAL "")
            set(_version "0.0.0-dev")
        endif()
    endif()
    if(NOT _version MATCHES "^[A-Za-z0-9][A-Za-z0-9._+-]*$")
        message(FATAL_ERROR "Release version must use only letters, digits, '.', '_', '+', or '-' and start with a letter or digit: ${_version}")
    endif()
    set(${out_name} "${_version}" PARENT_SCOPE)
endfunction()
