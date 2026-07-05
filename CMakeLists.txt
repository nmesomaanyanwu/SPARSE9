cmake_minimum_required(VERSION 3.20)
project(matching_engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Werror)

add_executable(matching_engine src/main.cpp)
target_include_directories(matching_engine PRIVATE include)
