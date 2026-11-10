# Included by the patched src/core/CMakeLists.txt when LICHII_SPLIT is on.
# Adds the split hook to the emulator's core library. Changing what the hook
# builds needs no new patch.
set(_split_hook "${LICHII_SPLIT_DIR}/split/hook")
target_sources(core PRIVATE
    "${_split_hook}/split_hook.cpp"
)
target_include_directories(core PUBLIC "${_split_hook}")
target_compile_definitions(core PUBLIC LICHII_SPLIT)
target_link_libraries(core PRIVATE split)

# Which split-green-hat commit is built in, for the log.
execute_process(COMMAND git -C "${LICHII_SPLIT_DIR}" describe --tags --always --dirty
    OUTPUT_VARIABLE _split_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _split_version)
    set(_split_version "unknown")
endif()
set_property(SOURCE "${_split_hook}/split_hook.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "LICHII_SPLIT_VERSION=\"${_split_version}\"")
