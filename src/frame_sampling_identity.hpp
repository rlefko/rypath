// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "sha256.hpp"
#include <span>
#include <string>
namespace rypath::frame_benchmark {
// Bind executed sampling policies while retaining the physical scene/camera
// identity and every historical default/shared-coating-only identity.
inline std::string sampling_source_identity(std::string physical, bool shared_coating,
                                            bool coherent_film) {
    const auto bind = [&](const char *policy) {
        const auto binding = physical + policy;
        physical = digest_hex(sha256(
            std::span(reinterpret_cast<const std::uint8_t *>(binding.data()), binding.size())));
    };
    if (shared_coating)
        bind("|frame-coating:shared-diffuse-v1");
    if (coherent_film)
        bind("|frame-film:coherent-box-v1");
    return physical;
}
} // namespace rypath::frame_benchmark
