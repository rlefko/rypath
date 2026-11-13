// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "sobol_shared.h"

#include <cstdint>
#include <string>

namespace rypath {

const sobol::Tables &sobol_tables();

enum class SamplerKind : std::uint32_t { independent = 0, sobol = 1 };
enum class SobolRandomization : std::uint32_t {
    none = 0,
    permutedigits = 1,
    fastowen = 2,
    owen = 3
};
struct RenderSampler {
    SamplerKind kind = SamplerKind::independent;
    SobolRandomization randomization = SobolRandomization::fastowen;
};
inline constexpr std::uint32_t sobol_max_depth = 127;
inline constexpr std::uint32_t sobol_max_scale_exponent = sobol::max_scale_exponent;

// Validate a nonempty absolute sample range, without allocating render resources.
std::string validate_sampler_range(const RenderSampler &sampler, std::uint64_t seed,
                                   std::uint32_t width, std::uint32_t height,
                                   std::uint64_t sample_begin, std::uint64_t sample_count);
std::uint32_t sobol_scale_exponent(std::uint32_t width, std::uint32_t height);
std::string sampler_description(const RenderSampler &sampler);
// Decode the low word of a seed accepted by validate_sampler_range.
std::int32_t sobol_signed_seed(std::uint64_t seed);

} // namespace rypath
