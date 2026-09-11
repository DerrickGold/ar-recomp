include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../packaging/release_policy.cmake")

# Wrap one already-built desktop artifact and its BuilderData marker in a
# portable release archive. This function only copies and compresses; callers
# remain responsible for building and validating the source artifact once.
function(builder_create_portable_release kind platform artifact stage out_artifact out_filename)
    if(NOT kind MATCHES "^(macos|linux|windows)$")
        message(FATAL_ERROR "Unknown portable release kind: ${kind}")
    endif()
    if(IS_SYMLINK "${artifact}")
        message(FATAL_ERROR "Refusing portable release artifact symlink: ${artifact}")
    endif()
    if(kind STREQUAL "macos")
        if(NOT IS_DIRECTORY "${artifact}")
            message(FATAL_ERROR "Portable macOS release requires an app directory: ${artifact}")
        endif()
    elseif(NOT EXISTS "${artifact}" OR IS_DIRECTORY "${artifact}")
        message(FATAL_ERROR "Portable release requires a regular artifact: ${artifact}")
    endif()
    if(IS_SYMLINK "${stage}" OR NOT IS_DIRECTORY "${stage}")
        message(FATAL_ERROR "Portable release stage must be a real directory: ${stage}")
    endif()

    builder_portable_release_name("${platform}" _filename)
    set(_root_leaf "ActRaiserRecompBuilder-${platform}-portable")
    set(_root "${stage}/${_root_leaf}")
    set(_archive "${stage}/${_filename}")
    foreach(_path "${_root}" "${_archive}")
        if(EXISTS "${_path}" OR IS_SYMLINK "${_path}")
            message(FATAL_ERROR "Portable release staging collision: ${_path}")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${_root}")
    get_filename_component(_artifact_leaf "${artifact}" NAME)
    set(_copy "${_root}/${_artifact_leaf}")

    if(kind STREQUAL "macos")
        find_program(_ditto ditto REQUIRED)
        execute_process(COMMAND "${_ditto}" "${artifact}" "${_copy}"
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        file(COPY_FILE "${artifact}" "${_copy}")
        if(kind STREQUAL "linux")
            file(CHMOD "${_copy}" PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif()
    endif()
    file(WRITE "${_copy}.portable" "BuilderData\n")

    if(kind STREQUAL "macos")
        # Preserve resource forks and Finder metadata just like the direct app
        # ZIP; the portable marker remains outside the signed bundle.
        execute_process(COMMAND "${_ditto}" -c -k --keepParent --sequesterRsrc
            "${_root}" "${_archive}" COMMAND_ERROR_IS_FATAL ANY)
    elseif(_filename MATCHES "\\.zip$")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}"
            --format=zip -- "${_root_leaf}"
            WORKING_DIRECTORY "${stage}" COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cJf "${_archive}"
            -- "${_root_leaf}"
            WORKING_DIRECTORY "${stage}" COMMAND_ERROR_IS_FATAL ANY)
    endif()

    set(${out_artifact} "${_archive}" PARENT_SCOPE)
    set(${out_filename} "${_filename}" PARENT_SCOPE)
endfunction()
