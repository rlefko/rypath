// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pbrt.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace rypath {

enum class ResourceProfile { standard, large };

struct ResourceLimits {
    // Includes separate decoded scalars, expanded frame entries, and expanded
    // shading-normal entries; their units must not alias one another.
    pbrt::Limits pbrt;
    // Native validation counts frames, rather than their six scalar coordinates.
    std::size_t triangle_frame_entries;
    std::size_t preparation_bytes;
};

std::string_view resource_profile_name(ResourceProfile profile);
std::optional<ResourceProfile> parse_resource_profile(std::string_view name);
ResourceLimits resource_limits(ResourceProfile profile);
// Reduced budgets are useful for embedding and tests. Reject requests beyond
// the supported large profile before parsing or allocation.
std::string validate_resource_limits(const ResourceLimits &limits);

} // namespace rypath
