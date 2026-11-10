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

# The co-op research project supplies the memory readers, actor creation,
# camera tracking and code cave used by the M1 second-Link work. GPL-3.0.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/oot-remake-multiplayer/CMakeLists.txt")
  set(COOP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(COOP_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/oot-remake-multiplayer
                   "${CMAKE_BINARY_DIR}/oot-coop" EXCLUDE_FROM_ALL)
endif()

if(SPLIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
