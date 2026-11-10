add_library(split STATIC
  src/settings.cpp
  src/version.cpp
  src/viewport.cpp
)

target_include_directories(split PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(split PRIVATE split_warnings)

# Read the version once at configure time. A shallow or archive checkout still
# gets a usable string.
execute_process(
  COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/.." describe --tags --always --dirty
  OUTPUT_VARIABLE SPLIT_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT SPLIT_VERSION)
  set(SPLIT_VERSION "dev")
endif()
target_compile_definitions(split PRIVATE SPLIT_VERSION="${SPLIT_VERSION}")
