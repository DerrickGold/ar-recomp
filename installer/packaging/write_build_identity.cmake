# CMake install/CPack run this on the freshly staged release, never a live game
# folder. Generic archives lack the desktop host's verified payload manifest;
# give them a deterministic content identity too, without startup tree walks.
if(NOT DEFINED BUILDER_IDENTITY_ROOT)
    set(BUILDER_IDENTITY_ROOT "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/utils")
endif()
if(NOT EXISTS "${BUILDER_IDENTITY_ROOT}/snesbuild.ini")
    message(FATAL_ERROR "Builder identity requires a staged installer payload")
endif()
file(GLOB_RECURSE _inputs LIST_DIRECTORIES false RELATIVE "${BUILDER_IDENTITY_ROOT}"
    "${BUILDER_IDENTITY_ROOT}/*")
list(SORT _inputs)
set(_fingerprints "ActRaiserRecompBuilder release inputs v1\n")
foreach(_input IN LISTS _inputs)
    if(_input STREQUAL "build-inputs.sha256")
        continue()
    endif()
    file(SHA256 "${BUILDER_IDENTITY_ROOT}/${_input}" _hash)
    string(APPEND _fingerprints "${_input}\n${_hash}\n")
endforeach()
string(SHA256 _identity "${_fingerprints}")
file(WRITE "${BUILDER_IDENTITY_ROOT}/build-inputs.sha256" "${_identity}\n")
