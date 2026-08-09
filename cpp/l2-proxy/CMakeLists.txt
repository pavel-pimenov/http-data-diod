cmake_minimum_required(VERSION 3.16)
project(l2-proxy)

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------- ccache integration -------------------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "ccache found: ${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
else()
    message(WARNING "ccache не найден. Для ускорения компиляции рекомендуется установить ccache (sudo apt install ccache).")
endif()
# ----------------------------------------------------------

# Unity Build: combine multiple .cpp files into fewer compilation units
# This reduces header parsing overhead and speeds up clean builds by 20-40%
# Allow explicit -DCMAKE_UNITY_BUILD=OFF (used by the clang-tidy lint stage,
# which needs per-file compile_commands.json entries).
if(NOT DEFINED CMAKE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
endif()
set(CMAKE_UNITY_BUILD_BATCH_SIZE 10)
message(STATUS "Unity Build enabled (batch size: 10)")

# Keep version.cpp, main.cpp and httplib.cc out of the Unity batches:
#  - version.cpp is the only TU that includes l2-proxy-version.h; the version
#    is regenerated on every commit, so batching it would recompile the whole
#    batch on each version bump. As its own tiny TU a bump recompiles just it.
#  - httplib.cc is by far the largest TU; as its own TU it is never rebuilt
#    just because a batch-mate changed, and it adds one more parallel job.
# GROUP mode (CMake >= 3.18): sources without a UNITY_GROUP compile
# individually, the rest are grouped by their UNITY_GROUP value.

set(BUILD_SHARED_LIBS OFF)

# Position independent code for static linking
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Suppress compiler notes and warnings for cleaner build output
set(L2_PROXY_COMMON_COMPILE_OPTIONS
    -Wno-ignored-attributes
    -Wno-deprecated
    -Wno-deprecated-declarations
    -Wno-attributes
)

# Options for tracing backends
option(BUILD_TESTS "Build unit tests with Catch2" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer + LeakSanitizer" OFF)
option(ENABLE_PROFILER "Enable gperftools heap profiling (links tcmalloc + profiler)" OFF)

# PVS-Studio static analysis
option(ENABLE_PVS_STUDIO "Enable PVS-Studio static analysis target" OFF)

# clang-tidy static analysis
option(ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)

# cppcheck static analysis
option(ENABLE_CPPCHECK "Enable cppcheck analysis" OFF)

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer + LeakSanitizer + UBSan enabled")

    # More readable sanitizer reports and stack traces
    set(CMAKE_UNITY_BUILD OFF)
    message(STATUS "Unity Build disabled for sanitizer build")

    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g
        -O1
    )
    add_link_options(
        -fsanitize=address,undefined
    )
endif()

# Bake build mode into a macro so the app can log it at startup
set(L2_PROXY_BUILD_MODE "release")
if(ENABLE_ASAN)
    set(L2_PROXY_BUILD_MODE "debug+asan+lsan+ubsan")
elseif(ENABLE_PROFILER)
    set(L2_PROXY_BUILD_MODE "release+profiler")
endif()
message(STATUS "Build mode: ${L2_PROXY_BUILD_MODE}")

if(ENABLE_PROFILER)
    message(STATUS "gperftools heap profiling enabled")

    set(CMAKE_UNITY_BUILD OFF)
    message(STATUS "Unity Build disabled for profiler build (frame pointers needed)")

    find_library(TCMALLOC_LIBRARY tcmalloc_minimal
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib/aarch64-linux-gnu
        /usr/lib
        REQUIRED)
    find_library(PROFILER_LIBRARY profiler
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib/aarch64-linux-gnu
        /usr/lib
        REQUIRED)
    message(STATUS "Found tcmalloc_minimal: ${TCMALLOC_LIBRARY}")
    message(STATUS "Found profiler: ${PROFILER_LIBRARY}")

    add_compile_options(-g -fno-omit-frame-pointer)

    add_link_options(-no-pie)
endif()

# Release optimizations: -O3 + LTO for latency-sensitive proxy
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT ENABLE_ASAN AND NOT ENABLE_PROFILER)
    add_compile_options(-O3 -flto)
    add_link_options(-O3 -flto)
    message(STATUS "Release optimizations enabled: -O3 -flto")
endif()

# Find required packages
# Force dynamic linking for OpenSSL to avoid static library dependencies (zstd, libjent)
set(OpenSSL_USE_STATIC_LIBS OFF)
find_package(OpenSSL REQUIRED)

find_package(prometheus-cpp REQUIRED)
find_package(ZLIB REQUIRED)

# Find spdlog (fast C++ logging library)
find_package(spdlog CONFIG REQUIRED)
message(STATUS "Found spdlog: ${spdlog_VERSION}")

# Find Catch2 (unit testing framework) - optional
if(BUILD_TESTS)
    find_package(Catch2 REQUIRED)
    include(Catch)
endif()

# Find CLI11 (command line parsing) - optional
if(BUILD_TESTS)
    find_package(CLI11 CONFIG QUIET)
    if(NOT CLI11_FOUND)
        find_package(CLI11 REQUIRED)
    endif()
endif()

# No find_package needed


# Find NATS C client library
message(STATUS "NATS support enabled")
find_path(NATS_C_INCLUDE_DIR nats/nats.h
    /usr/local/include
    /usr/include
    REQUIRED)
find_library(NATS_C_LIBRARY nats
    /usr/local/lib
    /usr/lib/x86_64-linux-gnu
    /usr/lib
    REQUIRED)
message(STATUS "Found NATS C client: ${NATS_C_LIBRARY}")



# Generate l2-proxy-version.h only if it doesn't exist
#if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/l2-proxy-version.h)
#  add_custom_command(
#    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/l2-proxy-version.h
#    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/generate_version.sh > ${CMAKE_CURRENT_BINARY_DIR}/l2-proxy-version.h
#    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
#    COMMENT "Generating l2-proxy-version.h"
#  )
#endif()

#add_custom_target(generate_version DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/l2-proxy-version.h)


# Add executable
add_executable(l2-proxy
 main.cpp
 version.cpp
 app_context.cpp
 common_utils.cpp
 config.cpp
 request_handler.cpp
 request_id_generator.cpp
 trace_context_extractor.cpp
 request_data_preparer.cpp
 response_builder.cpp
 server_handler.cpp
 duplicate_detector.cpp
 l2_worker.cpp
 l2_worker_nats.cpp
 trace_logger.cpp
 nats_client.cpp
 nats_push_service.cpp
 nats_poll_service.cpp
  metrics_manager.cpp
 http_client.cpp
 http_client_pool.cpp
 stats_logger.cpp
 labeled_counter_collector.cpp
 labeled_histogram_collector.cpp
 httplib/httplib.cc
)

# Common compile options and definitions
target_compile_options(l2-proxy PRIVATE
    ${L2_PROXY_COMMON_COMPILE_OPTIONS}
)

target_compile_definitions(l2-proxy PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
    CPPHTTPLIB_ZLIB_SUPPORT
    CPPHTTPLIB_THREAD_POOL_COUNT=128
    CPPHTTPLIB_THREAD_POOL_MAX_COUNT=256
    ENABLE_NATS
    L2_PROXY_BUILD_MODE="${L2_PROXY_BUILD_MODE}"
)

# Suppress deprecation warnings for httplib
target_compile_options(l2-proxy PRIVATE -Wno-deprecated-declarations)


# add_dependencies(l2-proxy generate_version)

# GROUP-mode unity: main.cpp and httplib.cc have no UNITY_GROUP and are
# compiled individually; the rest are grouped into two batches.
set_target_properties(l2-proxy PROPERTIES UNITY_BUILD_MODE GROUP)

set_source_files_properties(
    app_context.cpp common_utils.cpp config.cpp request_handler.cpp
    request_id_generator.cpp trace_context_extractor.cpp
    request_data_preparer.cpp response_builder.cpp server_handler.cpp
    duplicate_detector.cpp
    PROPERTIES UNITY_GROUP "proxy-core")

set_source_files_properties(
    l2_worker.cpp l2_worker_nats.cpp trace_logger.cpp nats_client.cpp
    nats_push_service.cpp nats_poll_service.cpp metrics_manager.cpp
    http_client.cpp http_client_pool.cpp stats_logger.cpp
    labeled_counter_collector.cpp labeled_histogram_collector.cpp
    PROPERTIES UNITY_GROUP "proxy-nats")

# Include directories
target_include_directories(l2-proxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/base64
)

target_include_directories(l2-proxy PRIVATE ${NATS_C_INCLUDE_DIR})

target_link_libraries(l2-proxy PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    prometheus-cpp::pull
    prometheus-cpp::core
    pthread
    ZLIB::ZLIB
    spdlog::spdlog_header_only
)


if(ENABLE_PROFILER)
    target_link_libraries(l2-proxy PRIVATE
        ${TCMALLOC_LIBRARY}
        ${PROFILER_LIBRARY}
    )
endif()
target_link_libraries(l2-proxy PRIVATE ${NATS_C_LIBRARY})

# Use static libstdc++/libgcc only for non-sanitized builds.
# Sanitizer runtimes work more reliably with dynamic runtime linkage.
if(NOT ENABLE_ASAN AND NOT ENABLE_PROFILER)
    target_link_libraries(l2-proxy PRIVATE
        -static-libgcc
        -static-libstdc++
    )
endif()

# Export symbols for backtrace() symbol resolution in crash dumps
target_link_options(l2-proxy PRIVATE -rdynamic)

if(ENABLE_PROFILER)
    target_link_options(l2-proxy PRIVATE -no-pie)
endif()

# Add test subdirectory (only when building locally)
# add_subdirectory(test)

# Unit tests (optional, enabled with -DBUILD_TESTS=ON)
if(BUILD_TESTS)
    message(STATUS "Building unit tests with Catch2 and CLI11")
    
    # Component tests
    add_executable(test_components
        test_components.cpp
        config.cpp
        duplicate_detector.cpp
    )
    target_include_directories(test_components PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_options(test_components PRIVATE
        ${L2_PROXY_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(test_components PRIVATE 
        Catch2::Catch2WithMain 
        CLI11::CLI11
        pthread
        spdlog::spdlog_header_only
    )
    if(ENABLE_ASAN)
        target_compile_options(test_components PRIVATE
            -fsanitize=address
            -fno-omit-frame-pointer
            -g
            -O1
        )
        target_link_options(test_components PRIVATE
            -fsanitize=address
        )
    endif()
    
    # Enable Catch2 test discovery
    catch_discover_tests(test_components)

    # Enable testing
    enable_testing()
endif()

# ------------------- PVS-Studio static analysis -------------------
if(ENABLE_PVS_STUDIO)
    message(STATUS "PVS-Studio analysis enabled")

    include(FetchContent)
    FetchContent_Declare(
        PVS_CMakeModule
        GIT_REPOSITORY "https://github.com/viva64/pvs-studio-cmake-module.git"
        GIT_TAG        "master"
    )
    FetchContent_MakeAvailable(PVS_CMakeModule)
    include("${pvs_cmakemodule_SOURCE_DIR}/PVS-Studio.cmake")

    # Exclude third-party libraries from analysis via ARGS -e
    set(PVS_EXCLUDE_PATHS
        "${CMAKE_SOURCE_DIR}/httplib"
        "${CMAKE_SOURCE_DIR}/nats"
        "${CMAKE_SOURCE_DIR}/base64"
    )

    set(PVS_EXCLUDE_ARGS "")
    foreach(_path IN LISTS PVS_EXCLUDE_PATHS)
        list(APPEND PVS_EXCLUDE_ARGS -e "${_path}")
    endforeach()

    pvs_studio_add_target(
        TARGET l2-proxy.pvs
        OUTPUT FORMAT json
        ANALYZE l2-proxy
        MODE GA:1,2,3
        LOG "${CMAKE_BINARY_DIR}/pvs-studio.log"
        ARGS ${PVS_EXCLUDE_ARGS}
    )

    message(STATUS "PVS-Studio target: l2-proxy.pvs (run via: cmake --build . --target l2-proxy.pvs)")
endif()

# ------------------- clang-tidy static analysis -------------------
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE clang-tidy)
    if(CLANG_TIDY_EXE)
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-p;${CMAKE_BINARY_DIR}")
    else()
        message(WARNING "clang-tidy not found, skipping")
    endif()
endif()

# ------------------- cppcheck static analysis -------------------
if(ENABLE_CPPCHECK)
    find_program(CPPCHECK_EXE cppcheck)
    if(CPPCHECK_EXE)
        message(STATUS "cppcheck enabled: ${CPPCHECK_EXE}")
        add_custom_target(cppcheck
            COMMAND ${CPPCHECK_EXE}
                --enable=all
                --std=c++23
                --suppress=missingIncludeSystem
                --suppress=unusedFunction
                --suppress=unmatchedSuppression
                --suppress=functionStatic
                --suppress=knownConditionTrueFalse
                --suppress=operatorEqVarError
                --suppress=useStlAlgorithm
                --inline-suppr
                --quiet
                --check-level=exhaustive
                -I ${CMAKE_SOURCE_DIR}
                -I ${CMAKE_SOURCE_DIR}/httplib
                -I ${CMAKE_SOURCE_DIR}/base64
                ${CMAKE_SOURCE_DIR}/*.cpp
                ${CMAKE_SOURCE_DIR}/*.hpp
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running cppcheck static analysis"
        )
        message(STATUS "cppcheck target: cppcheck (run via: cmake --build . --target cppcheck)")
    else()
        message(WARNING "cppcheck not found, skipping")
    endif()
endif()
