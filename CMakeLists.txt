cmake_minimum_required(VERSION 3.20)
project(L3OrderBook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizer options
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

find_package(ZLIB REQUIRED)

# Core order book library
add_library(l3_orderbook
    src/order_book.cpp
    src/allocation_tracker.cpp
)

target_include_directories(l3_orderbook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(l3_orderbook PUBLIC
    ZLIB::ZLIB
)

target_compile_options(l3_orderbook PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# Reference runner executable
add_executable(reference_runner
    src/reference_runner.cpp
)

target_link_libraries(reference_runner PRIVATE
    l3_orderbook
)

target_include_directories(reference_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_compile_options(reference_runner PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# Differential parity runner executable
add_executable(differential_runner
    src/differential_runner.cpp
)

target_link_libraries(differential_runner PRIVATE
    l3_orderbook
)

target_include_directories(differential_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_compile_options(differential_runner PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# Arena runner executable (standalone engine benchmark + zero-allocation proof)
add_executable(arena_runner
    src/arena_runner.cpp
)

target_link_libraries(arena_runner PRIVATE
    l3_orderbook
)

target_include_directories(arena_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_compile_options(arena_runner PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# Stage 3 Queue Position and Wire Latency Simulator
add_executable(queue_simulator
    src/queue_simulator.cpp
)

target_link_libraries(queue_simulator PRIVATE
    l3_orderbook
)

target_compile_options(queue_simulator PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# Stage 4 Fill Simulator & Adverse Selection
add_executable(fill_simulator
    src/fill_simulator.cpp
)

target_link_libraries(fill_simulator PRIVATE
    l3_orderbook
)

target_compile_options(fill_simulator PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# External dependencies via FetchContent
include(FetchContent)

# GoogleTest
if(BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(l3_tests
        tests/test_main.cpp
        tests/test_parser_and_reference.cpp
        tests/test_order_book.cpp
        tests/test_stage3_queue.cpp
        tests/test_stage4_fill_simulator.cpp
    )

    target_link_libraries(l3_tests PRIVATE
        l3_orderbook
        GTest::gtest_main
    )

    target_include_directories(l3_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_compile_options(l3_tests PRIVATE
        -Wall -Wextra -Werror -pedantic
    )

    include(GoogleTest)
    gtest_discover_tests(l3_tests DISCOVERY_TIMEOUT 30)
endif()

# Google Benchmark
if(BUILD_BENCHMARKS)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(l3_benchmarks
        benchmarks/bench_main.cpp
    )

    target_link_libraries(l3_benchmarks PRIVATE
        l3_orderbook
        benchmark::benchmark
    )

    target_compile_options(l3_benchmarks PRIVATE
        -Wall -Wextra -Werror -pedantic
    )

    # Stage 2 isolated MMAP throughput, batch distribution, and tail attribution benchmark
    add_executable(bench_stage2
        benchmarks/bench_stage2.cpp
    )

    target_link_libraries(bench_stage2 PRIVATE
        l3_orderbook
    )

    target_include_directories(bench_stage2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_compile_options(bench_stage2 PRIVATE
        -Wall -Wextra -Werror -pedantic
    )
endif()
