// SPDX-License-Identifier: GPL-3.0-only
// GENERATED modified-PBRT reference, not stock conformance.
// build_receipt_sha256: ec8dc53ff315faf37db13fedbda5aac898d2bf98d91e00571d47ff1d879a62c4
// stock_output_sha256: 34b958fa77d4dc288268004992051c21f498811af22d49d43f1ed487fcbe5969
// corrected_output_sha256: 0b737521b4c8efa20edb6411f16c7e99425d2e40de5816bc170f0cfdaa7f50c9
// stock_fixture_sha256: 559f1b7fd1226e8ed1b24fb0f8a2f4afc8b0a76250582fbaccac72a422cbb921
// corrected_header_sha256: f0f79357fa56f4b5e38e51aac3f5c376d2578c5a23f57de5b3e7929c272209ae
#pragma once
#include <array>
#include <cstdint>
namespace layered_schedule_corrected {
inline constexpr std::array<std::uint32_t, 28> directional_f_bits = {
    0x3e319569u, // rough diffuse
    0x3e255ea4u, // rough diffuse importance
    0x3e03737cu, // diffuse walk roulette
    0x3e03737du, // diffuse walk roulette importance
    0x3edb5131u, // rough conductor
    0x3edb5131u, // rough conductor importance
    0x3f16fd73u, // conductor walk roulette
    0x3f16fd73u, // conductor walk roulette importance
    0x3ca03493u, // entrance rejection
    0x3ca03493u, // entrance rejection importance
    0x3dc9d933u, // low beta eight samples
    0x3dc9d933u, // low beta eight samples importance
    0x3e1772cau, // one internal sample
    0x3e1772c9u, // one internal sample importance
    0x3d8aef92u, // tangent phase rejection
    0x3d8aef92u, // tangent phase rejection importance
    0x3e4391d1u, // concentric center
    0x3e4391d1u, // concentric y axis
    0x3e4391d1u, // concentric x axis
    0x3e4391d1u, // concentric lower corner
    0x3e4391d1u, // concentric upper left
    0x3e4391d1u, // concentric lower right
    0x3e4391d1u, // concentric upper corner
    0x3e4391d1u, // concentric diagonal
    0x3e1e9e54u, // walk roulette branch witness
    0x3e1e9e55u, // walk roulette branch witness importance
    0x3e178a00u, // negative material seed
    0x3e178a00u, // negative material seed importance
};
} // namespace layered_schedule_corrected
