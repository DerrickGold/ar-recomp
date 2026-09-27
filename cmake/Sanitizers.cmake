# Instrument the entire configured build, including test support libraries.
# A tests-only build must not silently lose checks behind the game target.
option(AR_SANITIZE "Build with ASan + UBSan (debug corruption bugs)" OFF)
option(AR_TSAN "Build with ThreadSanitizer" OFF)

if(AR_SANITIZE AND AR_TSAN)
    message(FATAL_ERROR "AR_SANITIZE and AR_TSAN cannot both be ON")
endif()

if(AR_SANITIZE OR AR_TSAN)
    if(MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizer builds require a GCC or Clang toolchain")
    endif()
    if(AR_SANITIZE)
        set(_AR_SANITIZER address,undefined)
        # UBSan diagnostics must fail the test, rather than report and continue.
        add_compile_options(-fno-sanitize-recover=all)
    else()
        set(_AR_SANITIZER thread)
    endif()
    add_compile_options(-fsanitize=${_AR_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_AR_SANITIZER})
    message(STATUS "ActRaiserRecomp: ${_AR_SANITIZER} instrumentation enabled")
    unset(_AR_SANITIZER)
endif()
