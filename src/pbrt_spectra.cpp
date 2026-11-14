// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_spectra.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>

namespace rypath {
namespace {

using DenseSpectrum = std::array<float, 471>;

#include "../third_party/pbrt/named_spectra.inc"

struct KnotRange {
    std::string_view name;
    std::size_t offset;
    std::size_t count;
};

#include "../third_party/pbrt/named_spectrum_knots.inc"

static_assert(names.size() == 88 && names.size() == spectra.size());
static_assert(std::ranges::is_sorted(names) && std::ranges::adjacent_find(names) == names.end());
static_assert(std::ranges::binary_search(names, "stdillum-D65"));
static_assert(std::ranges::equal(names, knot_ranges, {}, std::identity{}, &KnotRange::name));
static_assert([] {
    std::size_t offset = 0;
    for (const auto &range : knot_ranges) {
        if (range.offset != offset || range.count < 2) {
            return false;
        }
        offset += range.count;
    }
    return offset == spectrum_knots.size();
}());

std::size_t spectrum_index(std::string_view name) {
    const auto position = std::ranges::lower_bound(names, name);
    if (position == names.end() || *position != name) {
        return names.size();
    }
    return static_cast<std::size_t>(position - names.begin());
}

} // namespace

std::span<const std::string_view> pbrt_named_spectrum_names() { return names; }

const std::array<float, 471> *pbrt_named_spectrum(std::string_view name) {
    const auto index = spectrum_index(name);
    return index == names.size() ? nullptr : &spectra[index];
}

std::span<const SpectralKnot> pbrt_named_spectrum_knots(std::string_view name) {
    const auto index = spectrum_index(name);
    if (index == names.size()) {
        return {};
    }
    const auto &range = knot_ranges[index];
    return std::span{spectrum_knots}.subspan(range.offset, range.count);
}

} // namespace rypath
