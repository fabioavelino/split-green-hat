# Included by the patched src/core/CMakeLists.txt when LICHII_SPLIT is on.
# Adds the split hook to the emulator's core library. Changing what the hook
# builds needs no new patch.
set(_split_hook "${LICHII_SPLIT_DIR}/split/hook")
set(_coop_dir "${LICHII_SPLIT_DIR}/external/oot-remake-multiplayer")
set(_coop_hook "${_coop_dir}/lichii/hook")

target_sources(core PRIVATE
    "${_split_hook}/split_hook.cpp"
    # The co-op project's audited actor-creation path: the code cave, the
    # puppet director, pose writer and camera tracking. M1 uses the debug
    # puppet to prove the pipeline before the real SplitLink actor.
    "${_coop_hook}/coop_camera.cpp"
    "${_coop_hook}/coop_pose.cpp"
    "${_coop_hook}/coop_puppet.cpp"
    "${_coop_hook}/coop_svc.cpp"
)

# Fritchii turns -Wshadow into an error; the upstream co-op file has one
# harmless shadowed buffer. Suppress it for that file only.
set_source_files_properties("${_coop_hook}/coop_puppet.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-shadow>")

target_include_directories(core PUBLIC "${_split_hook}" "${LICHII_SPLIT_DIR}/split/include")
target_include_directories(core PRIVATE "${_coop_hook}")
target_compile_definitions(core PUBLIC LICHII_SPLIT)
target_link_libraries(core PRIVATE split coop coop_net coop_host coop_presence)

# Which split-green-hat commit is built in, for the log.
execute_process(COMMAND git -C "${LICHII_SPLIT_DIR}" describe --tags --always --dirty
    OUTPUT_VARIABLE _split_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _split_version)
    set(_split_version "unknown")
endif()
set_property(SOURCE "${_split_hook}/split_hook.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "LICHII_SPLIT_VERSION=\"${_split_version}\"")
