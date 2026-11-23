// SPDX-License-Identifier: GPL-3.0-only

#include "sampling.hpp"
#include "qmc_shared.h"

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
                                   std::uint64_t sample_begin, std::uint64_t sample_count,
                                   std::uint64_t total_samples) {
    if (sample_count == 0 ||
        sample_begin > std::numeric_limits<std::uint64_t>::max() - (sample_count - 1)) {
        return "Sampler range must be nonempty and fit the absolute sample indices";
    }
    if (sampler.kind == SamplerKind::independent) {
        return {};
    }
    if (sampler.kind != SamplerKind::sobol && sampler.kind != SamplerKind::halton &&
        sampler.kind != SamplerKind::zsobol) {
        return "Invalid render sampler kind";
    }
    if (randomization_name(sampler.randomization).empty()) {
        return "Invalid QMC randomization mode";
    }
    if (seed != static_cast<std::uint64_t>(sobol_signed_seed(seed))) {
        return "QMC seed must have a canonical signed 32-bit encoding";
    }
    if (sampler.kind == SamplerKind::halton) {
        if (sampler.randomization == SobolRandomization::fastowen)
            return "Halton does not support fastowen randomization";
        if (width == 0 || height == 0)
            return "Halton film dimensions must be positive";
        const auto stride = std::uint64_t{qmc::base_scale(width, 2)} * qmc::base_scale(height, 3);
        // The worst pixel offset is stride-1. Keep signed upstream index support,
        // while intentionally widening its overflowing sample*stride intermediate.
        const auto limit = (std::uint64_t{INT64_MAX} - (stride - 1)) / stride;
        if (sample_begin + sample_count - 1 > limit)
            return "Halton absolute sample range exceeds the signed 64-bit index capacity";
        return {};
    }
    const auto m = sobol_scale_exponent(width, height);
    if (width == 0 || height == 0 ||
        m > (sampler.kind == SamplerKind::zsobol ? 26u : sobol_max_scale_exponent)) {
        return sampler.kind == SamplerKind::zsobol
                   ? "ZSobol film dimensions must be positive and at most 2^26"
                   : "Sobol film dimensions must be positive and at most 2^25";
    }
    if (sampler.kind == SamplerKind::zsobol) {
        if (total_samples == 0 || !std::has_single_bit(total_samples))
            return "ZSobol requires a fixed power-of-two total sample budget";
        const auto log_spp = std::bit_width(total_samples) - 1;
        if (2 * m + log_spp > sobol::matrix_size)
            return "ZSobol resolution and total sample budget exceed the 52-bit index capacity";
        if (sample_begin >= total_samples || sample_count > total_samples - sample_begin)
            return "ZSobol absolute sample range exceeds its fixed total sample budget";
        return {};
    }
    const auto limit = std::uint64_t{1} << (sobol::matrix_size - 2 * m);
    if (sample_begin + (sample_count - 1) >= limit) {
        return "Sobol absolute sample range exceeds the 52-bit interval index capacity";
    }
    return {};
}

std::string validate_halton_light_range(std::uint32_t width, std::uint32_t height,
                                        std::uint64_t sample_begin, std::uint64_t sample_count) {
    if (width == 0 || height == 0)
        return "Halton light film dimensions must be positive";
    if (sample_count == 0 ||
        sample_begin > std::numeric_limits<std::uint64_t>::max() - (sample_count - 1))
        return "Halton light range must be nonempty and fit the absolute sample indices";
    const auto stride = std::uint64_t{qmc::base_scale(width, 2)} * qmc::base_scale(height, 3);
    const auto limit = (std::uint64_t{INT64_MAX} - (stride - 1)) / stride;
    const auto last_tile = qmc::halton_light_tile_count(width, height) - 1;
    const auto slots = qmc::halton_light_tile_slots(width, height);
    // Bound before multiplying: both the tile packing and final CRT index must
    // remain within the signed upstream Halton capacity for every film owner.
    if (limit < last_tile || sample_begin + sample_count - 1 > (limit - last_tile) / slots)
        return "Halton light absolute sample range exceeds the signed 64-bit index capacity";
    return {};
}

// NOLINTEND(bugprone-easily-swappable-parameters)

std::uint32_t sampler_max_depth(const RenderSampler &sampler, bool dielectrics) {
    if (sampler.kind == SamplerKind::halton)
        return dielectrics ? 110u : 124u;
    if (sampler.kind == SamplerKind::sobol || sampler.kind == SamplerKind::zsobol)
        return dielectrics ? sobol_dielectric_max_depth : sobol_max_depth;
    return 1024;
}

std::string sampler_description(const RenderSampler &sampler, bool dielectrics,
                                bool perspective_lens) {
    const std::string lens = perspective_lens ? "+lens-v1" : "";
    switch (sampler.kind) {
    case SamplerKind::independent:
        return std::string(dielectrics ? "philox4x32-10-v1+dielectric-v1" : "philox4x32-10-v1") +
               lens + " (sequence differs from PBRT)";
    case SamplerKind::halton:
        return "halton-v1" + std::string(dielectrics ? "+dielectric-v1" : "") + lens + " / " +
               std::string(randomization_name(sampler.randomization)) +
               " (PBRT camera variates; rypath transport dimensions; none/owen ignore seed)";
    case SamplerKind::zsobol:
        return "zsobol-v1" + std::string(dielectrics ? "+dielectric-v1" : "") + lens + " / " +
               std::string(randomization_name(sampler.randomization)) +
               " (PBRT primary ordering; rypath transport dimensions)";
    case SamplerKind::sobol: {
        const auto mode = randomization_name(sampler.randomization);
        if (mode.empty()) {
            return "invalid Sobol randomization mode";
        }
        return std::string(dielectrics ? "sobol-v1+dielectric-v1" : "sobol-v1") + lens + " / " +
               std::string(mode) + " (PBRT camera variates; rypath transport dimensions)";
    }
    }
    return "invalid render sampler kind";
}

} // namespace rypath
