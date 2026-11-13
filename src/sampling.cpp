// SPDX-License-Identifier: GPL-3.0-only

#include "sampling.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string_view>

namespace rypath {
namespace {

constexpr sobol::Tables tables{
#include "../third_party/pbrt/sobol_tables.inc"
};

static_assert(static_cast<sobol::Word>(SobolRandomization::none) == sobol::no_randomization);
static_assert(static_cast<sobol::Word>(SobolRandomization::permutedigits) == sobol::permutedigits);
static_assert(static_cast<sobol::Word>(SobolRandomization::fastowen) == sobol::fastowen);
static_assert(static_cast<sobol::Word>(SobolRandomization::owen) == sobol::owen);

std::string_view randomization_name(SobolRandomization randomization) {
    switch (randomization) {
    case SobolRandomization::none:
        return "none";
    case SobolRandomization::permutedigits:
        return "permutedigits";
    case SobolRandomization::fastowen:
        return "fastowen";
    case SobolRandomization::owen:
        return "owen";
    }
    return {};
}

} // namespace

const sobol::Tables &sobol_tables() { return tables; }

std::uint32_t sobol_scale_exponent(std::uint32_t width, std::uint32_t height) {
    return std::bit_width(std::max({width, height, 1u}) - 1);
}

std::int32_t sobol_signed_seed(std::uint64_t seed) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(seed));
}

// The scalar argument order follows the render request and absolute range.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::string validate_sampler_range(const RenderSampler &sampler, std::uint64_t seed,
                                   std::uint32_t width, std::uint32_t height,
                                   std::uint64_t sample_begin, std::uint64_t sample_count) {
    if (sample_count == 0 ||
        sample_begin > std::numeric_limits<std::uint64_t>::max() - (sample_count - 1)) {
        return "Sampler range must be nonempty and fit the absolute sample indices";
    }
    if (sampler.kind == SamplerKind::independent) {
        return {};
    }
    if (sampler.kind != SamplerKind::sobol) {
        return "Invalid render sampler kind";
    }
    if (randomization_name(sampler.randomization).empty()) {
        return "Invalid Sobol randomization mode";
    }
    if (seed != static_cast<std::uint64_t>(sobol_signed_seed(seed))) {
        return "Sobol seed must have a canonical signed 32-bit encoding";
    }
    const auto m = sobol_scale_exponent(width, height);
    if (width == 0 || height == 0 || m > sobol_max_scale_exponent) {
        return "Sobol film dimensions must be positive and at most 2^25";
    }
    const auto limit = std::uint64_t{1} << (sobol::matrix_size - 2 * m);
    if (sample_begin + (sample_count - 1) >= limit) {
        return "Sobol absolute sample range exceeds the 52-bit interval index capacity";
    }
    return {};
}

// NOLINTEND(bugprone-easily-swappable-parameters)

std::string sampler_description(const RenderSampler &sampler) {
    switch (sampler.kind) {
    case SamplerKind::independent:
        return "philox4x32-10-v1 (sequence differs from PBRT)";
    case SamplerKind::sobol: {
        const auto mode = randomization_name(sampler.randomization);
        if (mode.empty()) {
            return "invalid Sobol randomization mode";
        }
        return "sobol-v1 / " + std::string(mode) +
               " (PBRT camera variates; rypath transport dimensions)";
    }
    }
    return "invalid render sampler kind";
}

} // namespace rypath
