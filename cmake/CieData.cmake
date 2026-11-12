# SPDX-License-Identifier: GPL-3.0-only

function(rypath_generate_cie_data output)
  set(csv "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/cie/CIE_xyz_1931_2deg.csv")
  file(SHA256 "${csv}" digest)
  if(NOT digest STREQUAL "fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1")
    message(FATAL_ERROR "Unexpected CIE 1931 dataset SHA-256: ${digest}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${csv}")
  file(STRINGS "${csv}" rows)
  set(header [=[// CIE 2019, CIE 1931 standard colorimetric observer, DOI: 10.25039/CIE.DS.xvudnb9b
// CC BY-SA 4.0. See third_party/cie for attribution and license.
// Generated from the unmodified 1 nm table. Values are transcribed into C++.
#pragma once
#include <array>
namespace rypath::detail {
inline constexpr std::array<std::array<double, 3>, 471> cie_xyz{{
]=])
  foreach(row IN LISTS rows)
    string(REGEX REPLACE "^[0-9]+," "" xyz "${row}")
    string(APPEND header "    {${xyz}},\n")
  endforeach()
  string(APPEND header "}};\n}\n")
  file(GENERATE OUTPUT "${output}" CONTENT "${header}")
endfunction()
