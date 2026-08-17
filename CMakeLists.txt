cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Werror)

# The engine itself, as a small static library reused by the demo and tests.
add_library(order_book STATIC src/orderBook.cpp)
target_include_directories(order_book PUBLIC include)

# Hand-driven demo executable.
add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE order_book)

# Benchmarks (Release-optimised recommended: -DCMAKE_BUILD_TYPE=Release).
add_executable(bench_core bench/bench_core.cpp)
target_link_libraries(bench_core PRIVATE order_book)
target_include_directories(bench_core PRIVATE bench)

add_executable(bench_cancel bench/bench_cancel.cpp)
target_link_libraries(bench_cancel PRIVATE order_book)
target_include_directories(bench_cancel PRIVATE bench)

find_package(Threads REQUIRED)
add_executable(bench_pipe bench/bench_pipe.cpp)
target_link_libraries(bench_pipe PRIVATE order_book Threads::Threads)
target_include_directories(bench_pipe PRIVATE bench)

add_executable(bench_tcp bench/bench_tcp.cpp)
target_link_libraries(bench_tcp PRIVATE order_book Threads::Threads)
target_include_directories(bench_tcp PRIVATE bench)

# Standalone TCP order-entry gateway.
add_executable(matching_server src/server_main.cpp)
target_link_libraries(matching_server PRIVATE order_book Threads::Threads)

# Test suite (dependency-free) wired into CTest.
enable_testing()
add_executable(order_book_tests test/test_order_book.cpp)
target_link_libraries(order_book_tests PRIVATE order_book)
add_test(NAME order_book_tests COMMAND order_book_tests)
