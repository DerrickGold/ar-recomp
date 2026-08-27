set(_prefix "${RUNTIME_BUILD_DIR}/installed-consumer-prefix")
set(_consumer_build "${RUNTIME_BUILD_DIR}/installed-consumer-build")
file(REMOVE_RECURSE "${_prefix}" "${_consumer_build}")

set(_install_command
    "${CMAKE_COMMAND}" --install "${RUNTIME_BUILD_DIR}" --prefix "${_prefix}")
if(NOT "${RUNTIME_TEST_CONFIG}" STREQUAL "")
    list(APPEND _install_command --config "${RUNTIME_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${_install_command}
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "runtime install failed:\n${_install_output}\n${_install_error}")
endif()

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${RUNTIME_SOURCE_DIR}/tests/consumer"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}")
if(NOT "${RUNTIME_TEST_CONFIG}" STREQUAL "")
    list(APPEND _configure_command
        "-DCMAKE_BUILD_TYPE=${RUNTIME_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "installed consumer configure failed:\n${_configure_output}\n${_configure_error}")
endif()

set(_build_command "${CMAKE_COMMAND}" --build "${_consumer_build}")
if(NOT "${RUNTIME_TEST_CONFIG}" STREQUAL "")
    list(APPEND _build_command --config "${RUNTIME_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${_build_command}
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "installed consumer build failed:\n${_build_output}\n${_build_error}")
endif()
