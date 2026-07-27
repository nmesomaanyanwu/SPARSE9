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

# Test suite (dependency-free) wired into CTest.
enable_testing()
add_executable(order_book_tests test/test_order_book.cpp)
target_link_libraries(order_book_tests PRIVATE order_book)
add_test(NAME order_book_tests COMMAND order_book_tests)
