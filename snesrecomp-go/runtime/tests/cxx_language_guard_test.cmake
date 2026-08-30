if(NOT DEFINED TEST_ROOT OR NOT DEFINED RUNNER_CMAKE)
    message(FATAL_ERROR "cxx language guard test needs TEST_ROOT and RUNNER_CMAKE")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
file(WRITE "${TEST_ROOT}/source/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.21)\n"
    "project(snesrecomp_c_only_guard LANGUAGES C)\n"
    "include(\"${RUNNER_CMAKE}\")\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}/source" -B "${TEST_ROOT}/build"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)

if(configure_result EQUAL 0)
    message(FATAL_ERROR
        "C-only consumer unexpectedly configured the C++20 runner")
endif()

set(configure_log "${configure_output}\n${configure_error}")
if(NOT configure_log MATCHES "enable_language\\(CXX\\)")
    message(FATAL_ERROR
        "C-only failure did not explain how to enable C++:\n${configure_log}")
endif()
