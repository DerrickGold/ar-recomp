# Optional native games have private runtime globals. Only the versioned host
# API is exported; generated AR1 and AR2 symbols never share a link namespace.
set(AR2_CONTENT_PACK "${CMAKE_SOURCE_DIR}/game-assets/imports/ar2-world.argame"
    CACHE FILEPATH "Optional Workshop AR2 world pack")
if(EXISTS "${AR2_CONTENT_PACK}")
  if(ANDROID)
    message(FATAL_ERROR "The AR2 prototype currently supports desktop coroutine backends only")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(ar2_output "${CMAKE_BINARY_DIR}/imported-ar2")
  file(GLOB ar2_cfg CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/recomp/ar2/*.cfg")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${ar2_cfg} "${AR2_CONTENT_PACK}" "${CMAKE_SOURCE_DIR}/tools/prepare_ar2.py"
    "${CMAKE_SOURCE_DIR}/recomp/ar2/module.ini")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_ar2.py"
    --pack "${AR2_CONTENT_PACK}" --output "${ar2_output}"
    RESULT_VARIABLE ar2_result OUTPUT_FILE "${CMAKE_BINARY_DIR}/ar2-prepare.log"
    ERROR_FILE "${CMAKE_BINARY_DIR}/ar2-prepare-errors.log")
  if(NOT ar2_result EQUAL 0)
    message(FATAL_ERROR "AR2 preparation failed; see ar2-prepare-errors.log")
  endif()
  file(GLOB ar2_generated "${ar2_output}/generated/*.c")
  file(STRINGS "${CMAKE_SOURCE_DIR}/recomp/ar2/module.ini" ar2_source_lines REGEX "^source = ")
  set(ar2_sources)
  foreach(line IN LISTS ar2_source_lines)
    string(REGEX REPLACE "^source = " "" source "${line}")
    list(APPEND ar2_sources "${CMAKE_SOURCE_DIR}/${source}")
  endforeach()
  add_library(actraiser2-objects OBJECT ${ar2_sources} ${ar2_generated} ${SNESRECOMP_RUNNER_SOURCES})
  snesrecomp_configure_runtime_target(actraiser2-objects)
  target_include_directories(actraiser2-objects PUBLIC "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(actraiser2-objects PRIVATE abort=sr_native_module_abort)
  set_target_properties(actraiser2-objects PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  add_library(actraiser2-native SHARED)
  target_link_libraries(actraiser2-native PRIVATE actraiser2-objects)
  set_target_properties(actraiser2-native PROPERTIES PREFIX ""
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(UNIX AND NOT APPLE)
    target_link_options(actraiser2-native PRIVATE "-Wl,-Bsymbolic")
  endif()
  add_dependencies(ActRaiserRecomp actraiser2-native)
  if(BUILD_TESTING)
    # Link the same private objects into a developer-only harness. RAM probes
    # and controlled fixtures never become part of the shipping module ABI.
    add_executable(actraiser_ar2_native_probe tests/ar2_native_probe.c)
    target_link_libraries(actraiser_ar2_native_probe PRIVATE actraiser2-objects)
    add_test(NAME actraiser_ar2_native_progression
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ar2_native_progression_test.py"
        $<TARGET_FILE:actraiser_ar2_native_probe> "${ar2_output}/ar2.sfc")
    set_tests_properties(actraiser_ar2_native_progression PROPERTIES TIMEOUT 180)
    add_executable(actraiser_imported_game_module_test tests/imported_game_module_test.c)
    target_include_directories(actraiser_imported_game_module_test PRIVATE src
      snesrecomp-go/runtime/include)
    target_link_libraries(actraiser_imported_game_module_test PRIVATE SDL3::SDL3)
    add_dependencies(actraiser_imported_game_module_test actraiser2-native)
    add_test(NAME actraiser_imported_game_module
      COMMAND actraiser_imported_game_module_test $<TARGET_FILE:actraiser2-native>)
    add_executable(actraiser_imported_game_session_test tests/imported_game_session_test.c
      src/host/imported_game_session.c)
    target_include_directories(actraiser_imported_game_session_test PRIVATE src
      snesrecomp-go/runtime/include)
    target_link_libraries(actraiser_imported_game_session_test PRIVATE SDL3::SDL3)
    add_dependencies(actraiser_imported_game_session_test actraiser2-native)
    add_test(NAME actraiser_imported_game_session
      COMMAND actraiser_imported_game_session_test "${ar2_output}/ar2.sfc")
    set_tests_properties(actraiser_imported_game_session PROPERTIES TIMEOUT 90)
  endif()
endif()
