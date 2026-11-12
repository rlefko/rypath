# SPDX-License-Identifier: GPL-3.0-only

function(rypath_verify_spectral_data path expected)
  file(SHA256 "${path}" digest)
  if(NOT digest STREQUAL expected)
    message(FATAL_ERROR "Unexpected SHA-256 for ${path}: ${digest}")
  endif()
endfunction()

function(rypath_embed_rgb_spectrum_data output table)
  rypath_verify_spectral_data("${table}"
    "6adffc41eefcf064b6524606cb8734941aa69ac962ac268fd9276f440cef61d5")
  # Bound the length of each string literal and keep every binary32 word within one chunk.
  set(chunk_size 4096)
  set(generated "${output}.tmp")
  file(WRITE "${generated}" "// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Generated from the canonical sRGB table. See third_party/pbrt/README.md.
#include <cstddef>
namespace rypath::detail {
namespace {
constexpr char chunks[][${chunk_size} + 1] = {
")
  file(SIZE "${table}" length)
  math(EXPR last_offset "${length} - 1")
  foreach(offset RANGE 0 ${last_offset} ${chunk_size})
    file(READ "${table}" chunk OFFSET ${offset} LIMIT ${chunk_size} HEX)
    string(REGEX REPLACE "(..)" "\\\\x\\1" escaped "${chunk}")
    file(APPEND "${generated}" "    \"${escaped}\",\n")
  endforeach()
  file(APPEND "${generated}" "};
} // namespace

const char *srgb_table_bytes(std::size_t offset) {
    return &chunks[offset / ${chunk_size}][offset % ${chunk_size}];
}
} // namespace rypath::detail
")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${generated}" "${output}"
    COMMAND_ERROR_IS_FATAL ANY)
  file(REMOVE "${generated}")
endfunction()

function(rypath_generate_rgb_spectrum_data output)
  set(data_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/pbrt")
  set(table "${data_dir}/srgb.bin")
  set(d65 "${data_dir}/d65.inc")
  rypath_verify_spectral_data("${d65}"
    "5a9a9c41d16cd61a936704cd5194ec0d2ade4f5f64c728419e2befaac566006a")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${table}" "${d65}")

  # Also generate at configure time for compilation database consumers such as clang-tidy.
  rypath_embed_rgb_spectrum_data("${output}" "${table}")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" "-Doutput=${output}" "-Dtable=${table}"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    DEPENDS "${table}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    COMMENT "Embedding pinned PBRT sRGB data"
    VERBATIM)
endfunction()

if(DEFINED CMAKE_SCRIPT_MODE_FILE)
  rypath_embed_rgb_spectrum_data("${output}" "${table}")
endif()
