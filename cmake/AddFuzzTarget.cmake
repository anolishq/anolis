# AddFuzzTarget.cmake — shared libFuzzer target helper for the anolishq C/C++ repos.
#
# Copy this file into a consumer repo (e.g. `cmake/AddFuzzTarget.cmake`) and
# `include()` it. Gate fuzzing behind `option(ENABLE_FUZZING ...)` so normal
# builds are unaffected:
#
#   option(ENABLE_FUZZING "Build libFuzzer fuzz targets (requires clang)" OFF)
#   if(ENABLE_FUZZING)
#     include(cmake/AddFuzzTarget.cmake)
#     anolis_add_fuzz_target(NAME fuzz_config SOURCES fuzz/fuzz_config.cpp LINK anolis-core)
#   endif()
#
# Each target is an executable built with `-fsanitize=fuzzer,address` (debug
# info, frame pointers). libFuzzer provides `main()`, so the harness only
# defines `LLVMFuzzerTestOneInput`. Run locally with:
#
#   ./fuzz_config -max_total_time=60 fuzz/corpus/fuzz_config
#
# The reusable `fuzz.yml` workflow builds and runs these in CI.

if(NOT ENABLE_FUZZING)
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "ENABLE_FUZZING requires a Clang toolchain (libFuzzer). "
    "Current compiler: ${CMAKE_CXX_COMPILER_ID}. "
    "Configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
endif()

# Sanitizer set shared by every fuzz target. fuzzer-no-link instruments code for
# coverage without pulling libFuzzer's main() into non-target objects.
#
# IMPORTANT: include() this BEFORE add_subdirectory() of the code-under-test so
# the instrumentation below applies to it, not just the harness.
set(ANOLIS_FUZZ_FLAGS
  -g -O1 -fno-omit-frame-pointer
  -fsanitize=fuzzer-no-link,address)

# Apply coverage+sanitizer instrumentation to the whole build so the
# code-under-test (not just the harness) is instrumented.
add_compile_options(${ANOLIS_FUZZ_FLAGS})
add_link_options(-fsanitize=address)

# anolis_add_fuzz_target(NAME <name> SOURCES <src>... [LINK <lib>...])
function(anolis_add_fuzz_target)
  cmake_parse_arguments(FZ "" "NAME" "SOURCES;LINK" ${ARGN})
  if(NOT FZ_NAME OR NOT FZ_SOURCES)
    message(FATAL_ERROR "anolis_add_fuzz_target: NAME and SOURCES are required")
  endif()
  add_executable(${FZ_NAME} ${FZ_SOURCES})
  # The target itself links libFuzzer (provides main()).
  target_compile_options(${FZ_NAME} PRIVATE -fsanitize=fuzzer)
  target_link_options(${FZ_NAME} PRIVATE -fsanitize=fuzzer)
  if(FZ_LINK)
    target_link_libraries(${FZ_NAME} PRIVATE ${FZ_LINK})
  endif()
endfunction()
