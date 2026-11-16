// SPDX-License-Identifier: GPL-3.0-only
#include "resource_limits.hpp"

#include <limits>
#include <stdexcept>

namespace rypath {
namespace {
constexpr std::size_t mib = std::size_t{1024} * 1024;
constexpr std::size_t gib = 1024 * mib;
constexpr std::size_t large_values = 128'000'000;
} // namespace

std::string_view resource_profile_name(ResourceProfile profile) {
    switch (profile) {
    case ResourceProfile::standard:
        return "standard";
    case ResourceProfile::large:
        return "large";
    }
    throw std::invalid_argument("Unknown resource profile");
}

std::optional<ResourceProfile> parse_resource_profile(std::string_view name) {
    if (name == "standard")
        return ResourceProfile::standard;
    if (name == "large")
        return ResourceProfile::large;
    return {};
}

ResourceLimits resource_limits(ResourceProfile profile) {
    ResourceLimits result{.pbrt = {},
                          .triangle_frame_entries = 2'000'000,
                          .preparation_bytes = std::numeric_limits<std::size_t>::max()};
    switch (profile) {
    case ResourceProfile::standard:
        return result;
    case ResourceProfile::large:
        result.pbrt.file_bytes = 128 * mib;
        result.pbrt.expanded_bytes = gib;
        result.pbrt.expansions = 1024;
        result.pbrt.total_values = large_values;
        result.pbrt.triangle_frame_entries = large_values;
        result.pbrt.shading_normal_entries = large_values;
        result.pbrt.image = {.max_dimension = image_dimension_ceiling,
                             .retained_bytes = image_retained_ceiling,
                             .scratch_bytes = image_scratch_ceiling,
                             .file_bytes = image_file_ceiling,
                             .png_allocation_bytes = image_scratch_ceiling};
        result.triangle_frame_entries = large_values;
        result.preparation_bytes = 16 * gib;
        result.pbrt.preparation_bytes = result.preparation_bytes;
        return result;
    }
    throw std::invalid_argument("Unknown resource profile");
}

std::string validate_resource_limits(const ResourceLimits &limits) {
    const auto &p = limits.pbrt;
    if (!valid_image_limits(p.image))
        return "Invalid image resource limits";
    if (p.file_bytes > 128 * mib || p.expanded_bytes > gib || p.expansions > 1024 ||
        p.total_values > large_values || p.triangle_frame_entries > large_values ||
        p.shading_normal_entries > large_values || limits.triangle_frame_entries > large_values)
        return "Resource limits exceed the supported large profile";
    // SIZE_MAX preserves the historical absence of a preparation-byte cap.
    if ((limits.preparation_bytes != std::numeric_limits<std::size_t>::max() &&
         limits.preparation_bytes > 16 * gib) ||
        (p.preparation_bytes != std::numeric_limits<std::size_t>::max() &&
         p.preparation_bytes > 16 * gib))
        return "Preparation byte limit exceeds the supported large profile";
    return {};
}

} // namespace rypath
