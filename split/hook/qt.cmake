# Included by the patched src/yuzu/CMakeLists.txt when LICHII_SPLIT is on.
# Adds the split status panel to the Qt frontend.
set(_split_hook "${LICHII_SPLIT_DIR}/split/hook")
target_sources(yuzu PRIVATE
    "${_split_hook}/split_window.cpp"
)
target_include_directories(yuzu PRIVATE "${_split_hook}" "${LICHII_SPLIT_DIR}/split/include")
target_compile_definitions(yuzu PRIVATE LICHII_SPLIT)
