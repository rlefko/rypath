# SPDX-License-Identifier: GPL-3.0-only
# Verify the small, unmodified, separately licensed STL fixture.
set(_rypath_stl_root "${CMAKE_CURRENT_LIST_DIR}/../tests/fixtures/stl/external")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rypath_stl_root}/SHA256SUMS")
file(STRINGS "${_rypath_stl_root}/SHA256SUMS" _rypath_stl_hashes)
foreach(_rypath_stl_entry IN LISTS _rypath_stl_hashes)
    string(SUBSTRING "${_rypath_stl_entry}" 0 64 _rypath_stl_expected)
    string(SUBSTRING "${_rypath_stl_entry}" 66 -1 _rypath_stl_path)
    file(SHA256 "${_rypath_stl_root}/${_rypath_stl_path}" _rypath_stl_actual)
    if(NOT _rypath_stl_actual STREQUAL _rypath_stl_expected)
        message(FATAL_ERROR "STL fixture checksum mismatch: ${_rypath_stl_path}")
    endif()
endforeach()
