cmake_minimum_required(VERSION 3.20)
project(ZephyrDB VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZEPHYR_BUILD_TESTS "Build ZephyrDB unit & integration tests" ON)
option(ZEPHYR_BUILD_BENCHMARKS "Build ZephyrDB benchmarks" ON)
option(ZEPHYR_BUILD_SERVER "Build ZephyrDB standalone server" ON)

set(ZEPHYR_CORE_SOURCES
    src/core/arena.cpp
    src/core/rcu.cpp
    src/core/augmented_btree.cpp
    src/storage/wal.cpp
    src/storage/mmap_store.cpp
    src/sql/lexer.cpp
    src/sql/parser.cpp
    src/sql/simd_eval.cpp
    src/sql/executor.cpp
    src/net/raft.cpp
    src/net/pgwire.cpp
    src/net/studio.cpp
    src/c_abi.cpp
)

function(zephyr_configure_target target_name)
    target_include_directories(${target_name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /std:c++latest
            /W4
            /wd4100
            /wd4201
            /wd4324
            /utf-8
            /permissive-
            $<$<CONFIG:Release>:/O2>
            $<$<AND:$<CONFIG:Release>,$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},AMD64>>:/arch:AVX2>
        )
        target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    elseif(EMSCRIPTEN)
        target_compile_options(${target_name} PRIVATE
            -std=c++26
            -Wall
            -Wextra
            -msimd128
            -mrelaxed-simd
            $<$<CONFIG:Release>:-O3>
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -std=c++26
            -Wall
            -Wextra
            -Wno-unused-parameter
            $<$<CONFIG:Release>:-O3>
            $<$<AND:$<CONFIG:Release>,$<OR:$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>,$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},AMD64>>>:-mavx2>
        )
    endif()
endfunction()

# Static core library
add_library(zephyrdb_core STATIC ${ZEPHYR_CORE_SOURCES})
zephyr_configure_target(zephyrdb_core)
target_compile_definitions(zephyrdb_core PUBLIC ZEPHYR_STATIC)

if(WIN32)
    target_link_libraries(zephyrdb_core PUBLIC ws2_32)
elseif(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(zephyrdb_core PUBLIC Threads::Threads)
endif()

# Shared library (C-ABI DLL / .so / .dylib)
if(NOT EMSCRIPTEN)
    add_library(zephyrdb SHARED ${ZEPHYR_CORE_SOURCES})
    zephyr_configure_target(zephyrdb)
    target_compile_definitions(zephyrdb PRIVATE ZEPHYR_BUILD_DLL)
    set_target_properties(zephyrdb PROPERTIES
        OUTPUT_NAME "zephyrdb"
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET hidden
    )
    if(WIN32)
        target_link_libraries(zephyrdb PRIVATE ws2_32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(zephyrdb PRIVATE Threads::Threads)
    endif()
endif()

if(ZEPHYR_BUILD_SERVER AND NOT EMSCRIPTEN)
    add_executable(zephyrdb_server src/server_main.cpp)
    zephyr_configure_target(zephyrdb_server)
    target_link_libraries(zephyrdb_server PRIVATE zephyrdb_core)
endif()

if(ZEPHYR_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_executable(zephyrdb_tests tests/test_zephyrdb.cpp)
    zephyr_configure_target(zephyrdb_tests)
    target_link_libraries(zephyrdb_tests PRIVATE zephyrdb_core)
    add_test(NAME zephyrdb_suite COMMAND zephyrdb_tests)
endif()

if(ZEPHYR_BUILD_BENCHMARKS AND NOT EMSCRIPTEN)
    add_executable(zephyrdb_bench benchmarks/bench_zephyrdb.cpp)
    zephyr_configure_target(zephyrdb_bench)
    target_link_libraries(zephyrdb_bench PRIVATE zephyrdb_core)
endif()
