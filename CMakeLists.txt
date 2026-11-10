cmake_minimum_required(VERSION 3.24)

project(split_green_hat VERSION 0.1.0 LANGUAGES C CXX)

option(SPLIT_BUILD_TESTS "Build the unit tests" ON)
option(SPLIT_BUILD_PROGRAMS "Build helper programs" OFF)

include(cmake/trim_paths.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(split_warnings INTERFACE)
if(MSVC)
  target_compile_options(split_warnings INTERFACE /W4 /permissive- /utf-8)
else()
  target_compile_options(split_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(split)

if(SPLIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
