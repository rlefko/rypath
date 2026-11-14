// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_spectra.hpp"

#include <algorithm>
#include <cstddef>

namespace rypath {
namespace {

using DenseSpectrum = std::array<float, 471>;

#include "../third_party/pbrt/named_spectra.inc"

static_assert(names.size() == 88 && names.size() == spectra.size());
static_assert(std::ranges::is_sorted(names) && std::ranges::adjacent_find(names) == names.end());
static_assert(std::ranges::binary_search(names, "stdillum-D65"));

} // namespace

std::span<const std::string_view> pbrt_named_spectrum_names() { return names; }

const std::array<float, 471> *pbrt_named_spectrum(std::string_view name) {
    const auto position = std::ranges::lower_bound(names, name);
    if (position == names.end() || *position != name) {
        return nullptr;
    }
    return &spectra[static_cast<std::size_t>(position - names.begin())];
}

} // namespace rypath
