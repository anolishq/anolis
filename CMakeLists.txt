# Anolis Runtime - CMake Build Configuration
cmake_minimum_required(VERSION 3.20)
project(anolis VERSION 0.1.7 LANGUAGES CXX)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Guard against unsupported ABI/toolchain combinations on Windows.
# The default vcpkg Windows triplets are MSVC-based and are incompatible
# with MinGW/GNU unless a MinGW triplet is explicitly selected.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_anolis_triplet "${VCPKG_TARGET_TRIPLET}")
    if(NOT _anolis_triplet OR _anolis_triplet MATCHES "windows")
        if(NOT _anolis_triplet MATCHES "mingw")
            message(FATAL_ERROR
                "Unsupported Windows toolchain/triplet combination:\n"
                "  compiler: ${CMAKE_CXX_COMPILER_ID}\n"
                "  VCPKG_TARGET_TRIPLET: '${_anolis_triplet}'\n"
                "Use MSVC presets (`dev-windows-release`, `dev-windows-debug`, `ci-windows-release`),\n"
                "or select an explicit MinGW triplet (e.g. x64-mingw-dynamic)."
            )
        endif()
    endif()
endif()

# Find protobuf via vcpkg
find_package(Protobuf CONFIG REQUIRED)

# Structural Analysis
option(ENABLE_CLANG_TIDY "Enable static analysis with clang-tidy" OFF)
option(ANOLIS_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Warnings.cmake)

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        # We use the inline integration for MSBuild/VS compatibility
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
        message(STATUS "clang-tidy configured for build-time analysis")
    else()
        message(WARNING "clang-tidy not found but ENABLE_CLANG_TIDY is ON")
    endif()
endif()

# Coverage Analysis (GCC/Clang)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
        # Multithreaded tests can corrupt gcov counters without atomic updates,
        # which causes lcov/geninfo failures with negative branch counts.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-fprofile-update=atomic)
        endif()
        add_link_options(--coverage)
        message(STATUS "Code coverage enabled (gcov/lcov compatible)")
    elseif(MSVC)
        message(WARNING "Coverage with MSVC requires Visual Studio profiler tools, not gcov/lcov")
    else()
        message(WARNING "Coverage requested but compiler not supported: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# ThreadSanitizer (TSAN) - Data Race Detection
option(ENABLE_TSAN "Enable ThreadSanitizer for data race detection" OFF)

if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(NOT WIN32)
            add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
            add_link_options(-fsanitize=thread)
            message(STATUS "ThreadSanitizer (TSAN) enabled for data race detection")
        else()
            message(WARNING "ThreadSanitizer not supported on Windows (Linux/macOS only)")
        endif()
    else()
        message(WARNING "ThreadSanitizer requires GCC or Clang, current compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# Enable testing
option(BUILD_TESTING "Build unit tests" ON)
option(ANOLIS_ENABLE_AUTOMATION "Enable automation subsystem (BehaviorTree runtime and BT endpoints)" ON)
option(ANOLIS_ENABLE_PYTHON_CTEST "Register selected Python integration tests with CTest" OFF)
set(ANOLIS_PROVIDER_SIM_EXE "" CACHE FILEPATH "Path to anolis-provider-sim executable for compatibility integration tests")

# Subdirectories
add_subdirectory(core)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests/unit)

    if(ANOLIS_ENABLE_PYTHON_CTEST)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(NOT Python3_Interpreter_FOUND)
            message(WARNING "ANOLIS_ENABLE_PYTHON_CTEST=ON but Python3 interpreter not found; skipping Python CTest registration")
        elseif(NOT ANOLIS_PROVIDER_SIM_EXE OR NOT EXISTS "${ANOLIS_PROVIDER_SIM_EXE}")
            message(STATUS "ANOLIS_ENABLE_PYTHON_CTEST=ON but ANOLIS_PROVIDER_SIM_EXE is unset or missing; skipping Python integration test registration")
        else()
            add_test(
                NAME integration.pytest
                COMMAND ${Python3_EXECUTABLE}
                        -m pytest
                        ${CMAKE_SOURCE_DIR}/tests/integration/test_integration.py
                        --runtime=$<TARGET_FILE:anolis-runtime>
                        --provider=${ANOLIS_PROVIDER_SIM_EXE}
                        -m
                        "not stress and not slow"
            )
            set_tests_properties(integration.pytest PROPERTIES
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                TIMEOUT 900
                LABELS "integration;python;compatibility"
            )

            add_test(
                NAME scenarios.pytest
                COMMAND ${Python3_EXECUTABLE}
                        -m pytest
                        ${CMAKE_SOURCE_DIR}/tests/scenarios/test_scenarios.py
                        --runtime=$<TARGET_FILE:anolis-runtime>
                        --provider=${ANOLIS_PROVIDER_SIM_EXE}
                        -m
                        "not stress and not slow"
            )
            set_tests_properties(scenarios.pytest PROPERTIES
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                TIMEOUT 1200
                LABELS "integration;python;compatibility;scenarios"
            )
        endif()
    endif()
endif()
