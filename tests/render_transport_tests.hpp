// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "metal_render.hpp"

#include <cmath>
#include <stdexcept>

namespace rypath::test {

inline void check_render_transport() {
    // Square plates spanning [-1, 1] in x/z, one unit apart in y. For these Philox
    // samples, the nearest crossing of each opposite plane is well inside or
    // outside its square. The camera looks down between the plates.
    RenderScene scene;
    scene.camera = {{0, 0.5f, 0}, {0, 0, 0}, {0, 0, 1}, 10};
    scene.geometry.meshes = {
        {{{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}}, {{0, 1, 2}, {0, 2, 3}}}};
    scene.geometry.instances = {{0, {}}, {0, {{1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0}}}};
    scene.materials.resize(1);
    scene.materials.front().reflectance.fill(0.75f);
    scene.instance_materials = {0, 0};
    auto built = build_metal_renderer(scene);
    if (built.status != GeometryStatus::success || !built.scene) {
        throw std::runtime_error("Two-plate transport fixture failed to build: " +
                                 built.diagnostic);
    }

    // The ratios reduce the solid-angle PDFs analytically. At cos=1/4 the
    // PDFs are equal. At cos=1 the light power weight is 1/17.
    constexpr auto light = [](double cosine) {
        const double ratio = 4 * cosine;
        return ratio / (1 + ratio * ratio);
    };
    constexpr auto escape = [](double cosine) {
        const double ratio_squared = 16 * cosine * cosine;
        return ratio_squared / (1 + ratio_squared);
    };
    static_assert(light(0.25) == 0.5 && escape(0.25) == 0.5);
    static_assert(light(1) == 4.0 / 17 && escape(1) == 16.0 / 17);
    constexpr double rho = 0.75;

    struct Probe {
        const char *label;
        std::uint64_t sample;
        std::uint32_t scatterings;
        double expected;
    };
    // A separate double-precision plane calculation selected these fixed
    // seed-1 paths before running Metal. Crossing margins in x/z exceed 0.08.
    // The first two probes escape after one scatter, including a value above
    // one that must remain unclipped. The third retains three unoccluded light
    // terms on roulette termination. In the last probe the first
    // two light terms are zero. Both roulette events survive, so post-roulette
    // throughput remains rho^2 and the fourth bounce contributes before escape.
    constexpr std::array probes{
        Probe{"complementary one-bounce MIS", 16, 1,
              rho * (light(0.2605856298185942) + escape(0.39052519476934366))},
        Probe{"unclipped raw radiance above one", 42, 1,
              rho * (light(0.2866041691639817) + escape(0.5992369846717766))},
        Probe{"third-scatter roulette retains NEE", 7, 3,
              rho * light(0.4436034472185657) + rho * rho * light(0.7026132031747483) +
                  rho * rho * rho * light(0.34453408825428117)},
        Probe{"surviving roulette compensates continuation", 65, 4,
              rho * rho * rho * (light(0.49632220285984685) + light(0.2762963058330404)) +
                  rho * rho * escape(0.34637618413176924)},
    };
    for (const auto &probe : probes) {
        const auto result = built.scene->sample({.width = 1,
                                                 .height = 1,
                                                 .seed = 1,
                                                 .pixel_count = 1,
                                                 .sample_begin = probe.sample,
                                                 .sample_count = 1});
        if (result.status != GeometryStatus::success || result.samples.size() != 1) {
            throw std::runtime_error(std::string{probe.label} + ": " + result.diagnostic);
        }
        const auto &sample = result.samples.front();
        if (sample.scatterings != probe.scatterings ||
            !(std::abs(sample.radiance - probe.expected) <= 3e-6)) {
            throw std::runtime_error(
                std::string{probe.label} + " expected " + std::to_string(probe.expected) + " at " +
                std::to_string(probe.scatterings) + " scatterings, got " +
                std::to_string(sample.radiance) + " at " + std::to_string(sample.scatterings));
        }
    }
}

} // namespace rypath::test
