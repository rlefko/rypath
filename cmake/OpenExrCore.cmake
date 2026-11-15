# SPDX-License-Identifier: GPL-3.0-only
# Standalone static C11 subset; deliberately does not discover host OpenEXR/Imath.
enable_language(C)
find_package(Threads REQUIRED)
set(_rypath_exr_root "${CMAKE_CURRENT_LIST_DIR}/../third_party/openexr")
set(_rypath_exr_core "${_rypath_exr_root}/src/lib/OpenEXRCore")
set(_rypath_exr_sources
  attributes base bytes channel_list chunk coding color_metadata compression
  context debug decoding encoding float_vector internal_b44 internal_b44_table
  internal_b44_table_init internal_dwa internal_dwa_table internal_dwa_table_init
  internal_huf internal_piz internal_pxr24 internal_rle internal_structs internal_zip
  memory opaque pack parse_header part part_attr preview std_attr string string_vector
  unpack validation write_header)
list(TRANSFORM _rypath_exr_sources PREPEND "${_rypath_exr_core}/")
list(TRANSFORM _rypath_exr_sources APPEND ".c")
add_library(rypath_openexr_core STATIC ${_rypath_exr_sources})
set_target_properties(rypath_openexr_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO POSITION_INDEPENDENT_CODE YES)
target_compile_definitions(rypath_openexr_core PRIVATE _FILE_OFFSET_BITS=64)
target_include_directories(rypath_openexr_core SYSTEM PUBLIC "${_rypath_exr_core}")
target_link_libraries(rypath_openexr_core PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(rypath_openexr_core PUBLIC m)
endif()
if(RYPATH_ENABLE_SANITIZERS)
  target_compile_options(rypath_openexr_core PRIVATE -fsanitize=address,undefined
    -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(rypath_openexr_core PRIVATE -fsanitize=address,undefined)
endif()
# Pin the shipped adaptation as well as the upstream source revision.
file(STRINGS "${_rypath_exr_root}/SHA256SUMS" _rypath_exr_hashes)
foreach(_rypath_exr_entry IN LISTS _rypath_exr_hashes)
  string(SUBSTRING "${_rypath_exr_entry}" 0 64 _rypath_exr_expected)
  string(SUBSTRING "${_rypath_exr_entry}" 66 -1 _rypath_exr_path)
  file(SHA256 "${_rypath_exr_root}/${_rypath_exr_path}" _rypath_exr_actual)
  if(NOT _rypath_exr_actual STREQUAL _rypath_exr_expected)
    message(FATAL_ERROR "Vendored OpenEXR checksum mismatch: ${_rypath_exr_path}")
  endif()
endforeach()
