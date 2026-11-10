# Keeps build-machine folders out of the programs.
#
# A compiler writes each source file's full path into the program (__FILE__,
# which the emulator's log prints with every line), and the linker writes the
# full path of the .pdb. This file cuts those paths down to the part below the
# source root: `src\core\core.cpp`, `split\hook\split_hook.cpp`.
#
# Pass it to the emulator at configure time:
#   -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<this repo>/cmake/trim_paths.cmake
#
# MSVC: /d1trimfile:<root>/ and /PDBALTPATH:%_PDB%.
# GCC/Clang: -ffile-prefix-map=<root>/=.

if(SPLIT_TRIM_PATHS_DONE)
  return()
endif()
set(SPLIT_TRIM_PATHS_DONE ON)

set(_split_trim_roots "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
if(LICHII_SPLIT_DIR)
  list(APPEND _split_trim_roots "${LICHII_SPLIT_DIR}")
endif()
foreach(_split_home_var USERPROFILE HOME)
  if(DEFINED ENV{${_split_home_var}} AND NOT "$ENV{${_split_home_var}}" STREQUAL "")
    list(APPEND _split_trim_roots "$ENV{${_split_home_var}}")
  endif()
endforeach()

# Absolute, forward slashes, no doubles, longest first.
set(_split_trim_clean "")
foreach(_split_root IN LISTS _split_trim_roots)
  get_filename_component(_split_root "${_split_root}" ABSOLUTE)
  file(TO_CMAKE_PATH "${_split_root}" _split_root)
  string(REGEX REPLACE "/+$" "" _split_root "${_split_root}")
  if(_split_root AND NOT _split_root MATCHES "^([A-Za-z]:)?$")
    list(APPEND _split_trim_clean "${_split_root}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _split_trim_clean)
set(_split_trim_sorted "")
while(_split_trim_clean)
  set(_split_longest "")
  foreach(_split_root IN LISTS _split_trim_clean)
    string(LENGTH "${_split_root}" _split_len)
    string(LENGTH "${_split_longest}" _split_best)
    if(_split_len GREATER _split_best)
      set(_split_longest "${_split_root}")
    endif()
  endforeach()
  list(APPEND _split_trim_sorted "${_split_longest}")
  list(REMOVE_ITEM _split_trim_clean "${_split_longest}")
endwhile()

foreach(_split_root IN LISTS _split_trim_sorted)
  foreach(_split_lang C CXX)
    add_compile_options(
      "$<$<COMPILE_LANG_AND_ID:${_split_lang},MSVC>:/d1trimfile:${_split_root}/>"
      "$<$<COMPILE_LANG_AND_ID:${_split_lang},GNU,Clang,AppleClang>:-ffile-prefix-map=${_split_root}/=>")
  endforeach()
endforeach()
foreach(_split_lang C CXX)
  add_link_options("$<$<LINK_LANG_AND_ID:${_split_lang},MSVC>:/PDBALTPATH:%_PDB%>")
endforeach()
message(STATUS "Source paths trimmed below: ${_split_trim_sorted}")
