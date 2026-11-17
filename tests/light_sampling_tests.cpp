// SPDX-License-Identifier: GPL-3.0-only
#include "image_texture.hpp"
#include "light_sampling.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
using namespace rypath;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
LightSamplingTables build(const std::vector<LightSamplingInput> &input) {
    std::string error;
    auto table = build_light_sampling(input, std::size_t{16} * 1024 * 1024, error);
    if (!table)
        throw std::runtime_error(error);
    return *table;
}
void alias_mass() {
    std::vector<LightSamplingInput> input(5);
    input[0].power = 1;
    input[1].power = 2;
    input[2].power = 1000;
    input[3].power = 0;
    input[4].power = .001;
    auto table = build(input);
    std::vector<double> mass(input.size());
    double total = 1003.001;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto &a = table.alias[i];
        require(a.alias < input.size() && a.threshold >= 0 && a.threshold <= 1, "Alias bounds");
        mass[i] += double(a.threshold) / static_cast<double>(input.size());
        mass[a.alias] += (1 - double(a.threshold)) / static_cast<double>(input.size());
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        require(std::abs(mass[i] - input[i].power / total) < 1e-7,
                "Actual alias mass differs from target power");
        require(std::abs(mass[i] - table.alias[i].pmf) < 1e-7,
                "Alias evaluation differs from actual selection mass");
    }
    require(mass[3] == 0, "Black light retained positive power mass");
    auto black = build(std::vector<LightSamplingInput>(3));
    for (auto &a : black.alias)
        require(std::abs(a.pmf - 1. / 3) < 1e-7, "All black power fallback");
    require(black.nodes.empty(), "Black finite lights entered BVH");
}
void separate_power_proxies() {
    std::vector<LightSamplingInput> input(3);
    input[0].power = 1;
    input[0].bounds_power = 9;
    input[1].power = 3;
    input[1].bounds_power = 1;
    input[2].power = 0;
    input[2].bounds_power = 0;
    auto table = build(input);
    require(std::abs(table.alias[0].pmf - .25) < 1e-7 && std::abs(table.alias[1].pmf - .75) < 1e-7,
            "Alias must use Phi, independently of bounds phi");
    require(std::abs(table.nodes[table.records[0].leaf].power - .9) < 1e-7 &&
                std::abs(table.nodes[table.records[1].leaf].power - .1) < 1e-7,
            "BVH leaves must use bounds phi, independently of Phi");
    require(table.records[2].leaf == UINT32_MAX && table.alias[2].pmf == 0,
            "Black light excluded from both nonempty proposals");
    input[0].bounds_power = 0;
    input[1].bounds_power = 0;
    table = build(input);
    require(std::abs(table.nodes[table.records[0].leaf].power - .25) < 1e-7,
            "Positive Phi restores support when a bounds proxy is zero");
    input[0].power = 0;
    input[0].bounds_power = 5;
    input[1].power = 2;
    input[1].bounds_power = 0;
    table = build(input);
    require(std::abs(table.alias[0].pmf - 5. / 7) < 1e-7 && table.records[0].leaf != UINT32_MAX,
            "Positive bounds restore support when fixed-wavelength Phi is zero");
    for (auto &entry : input) {
        entry.power = 0;
        entry.bounds_power = 0;
    }
    table = build(input);
    require(table.nodes.empty(), "All-black explicit bounds do not create finite leaves");
    for (auto &alias : table.alias)
        require(std::abs(alias.pmf - 1. / 3) < 1e-7, "All-black Phi uniform fallback");
}
void bounds_models() {
    const std::array<std::array<double, 3>, 4> rectangle{
        {{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {2, 3, 0}}};
    require(light_patch_rectangle(rectangle) && light_patch_area(rectangle) == 6,
            "Pinned rectangle area");
    const std::array<std::array<double, 3>, 4> trapezoid{
        {{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {1, 3, 0}}};
    require(!light_patch_rectangle(trapezoid) &&
                std::abs(light_patch_area(trapezoid) - 4.5) < 1e-12,
            "Pinned nonrectangle 3x3 patch estimate integrates planar trapezoid");
    const std::array<std::array<double, 3>, 4> warped{{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {2, 3, 2}}};
    // Independent Python transcription of pinned PBRT's 3x3 area construction.
    require(std::abs(light_patch_area(warped) - 7.2314719134551639) < 1e-12,
            "Pinned nonplanar 3x3 patch area");
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1},
        mirror{1, 0, 0, 0, 1, 0, 0, 0, -1}, shear{1, .25f, 0, 0, 1, 0, 0, 0, 1};
    LightSamplingInput cone;
    light_positional_cone(cone, identity, .9f, .98f);
    require(cone.cos_outer > .899f && cone.cos_outer <= .9f && cone.cos_emission == .98f &&
                cone.direction[2] == 1 && !cone.two_sided,
            "Spot/projection similarity cone");
    light_positional_cone(cone, mirror, 1, .7f);
    require(cone.direction[2] == -1 && cone.cos_outer > .999f, "Reflected projection cone axis");
    light_positional_cone(cone, shear, .9f, .98f);
    require(cone.cos_outer == -1 && cone.cos_emission == 0,
            "Affine direction distortion uses conservative support");
    const std::array<std::array<double, 3>, 4> normals{
        {{0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}}};
    light_normal_cone(cone, normals);
    for (const auto &n : normals) {
        double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        double cosine =
            (n[0] * cone.direction[0] + n[1] * cone.direction[1] + n[2] * cone.direction[2]) /
            length;
        require(cosine + 1e-7 >= cone.cos_outer, "Normal cone must bound each patch corner");
    }
}
void topology() {
    std::vector<LightSamplingInput> input(129);
    for (unsigned i = 0; i < 128; ++i) {
        input[i].power = i + 1;
        input[i].low = {float(i), 0, 0};
        input[i].high = {float(i) + .25f, 1, 1};
        input[i].direction = {0, 0, 1};
        input[i].cos_outer = 1;
        input[i].two_sided = false;
    }
    input[128].infinite = true;
    input[128].power = 100;
    auto a = build(input), b = build(input);
    require(a.nodes.size() == 255, "BVH node count");
    require(a.infinite == std::vector<std::uint32_t>{128}, "Infinite inventory");
    std::vector<bool> seen(input.size());
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        const auto &n = a.nodes[i];
        require(n.power > 0 && std::isfinite(n.power), "Finite node importance");
        require(n.cos_outer >= -1 && n.cos_outer <= 1, "Cone range");
        require(n.child == b.nodes[i].child && n.light == b.nodes[i].light,
                "Nondeterministic build");
        if (n.light != UINT32_MAX) {
            require(n.light < 128 && !seen[n.light], "Repeated or invalid finite leaf");
            seen[n.light] = true;
            require(a.records[n.light].leaf == i, "Reverse leaf map");
        } else {
            require(i + 1 < a.nodes.size() && n.child < a.nodes.size(), "Child indices");
            for (auto child : {i + 1, std::size_t(n.child)}) {
                const auto &c = a.nodes[child];
                require(c.parent == i, "Parent map");
                require(c.low.x >= n.low.x && c.low.y >= n.low.y && c.low.z >= n.low.z &&
                            c.high.x <= n.high.x && c.high.y <= n.high.y && c.high.z <= n.high.z,
                        "Nonconservative parent bounds");
            }
        }
    }
    for (unsigned i = 0; i < 128; ++i)
        require(seen[i], "Missing light leaf");
    require(a.records[128].flags == 1, "Infinite reverse map");
}
void direction_maps() {
    ImagePyramid image{.encoding = ImageEncoding::linear,
                       .wrap = ImageWrap::clamp,
                       .levels = {{2, 3, 0}},
                       .storage = ImageStorage::linear_float,
                       .native_base = true};
    const std::array<float, 6> values{1, 2, 0, 4, 8, 16};
    for (float value : values)
        image.texels.insert(image.texels.end(), {std::bit_cast<std::uint32_t>(value),
                                                 std::bit_cast<std::uint32_t>(value),
                                                 std::bit_cast<std::uint32_t>(value), 0});
    std::string error;
    auto table = build_light_direction_sampling(image, 4096, error);
    require(bool(table), "Rectangular direction CDF build");
    require(table->width == 2 && table->height == 3 && table->cdf.size() == 13,
            "Rectangular CDF layout");
    double mass = 0;
    for (unsigned y = 0; y < 3; ++y) {
        auto row = 4 + y * 3;
        for (unsigned x = 0; x < 2; ++x) {
            double p = (table->cdf[y + 1] - table->cdf[y]) *
                       (table->cdf[row + x + 1] - table->cdf[row + x]);
            require(std::abs(p - values[y * 2 + x] / 31.) < 1e-7, "Native map mass");
            mass += p;
        }
    }
    require(std::abs(mass - 1) < 1e-7, "Map normalization");
    auto projection = build_light_direction_sampling(image, 4096, error, 2, 1, 1.5);
    require(bool(projection), "Projection CDF build");
    std::array<double, 6> expected{};
    double total = 0;
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 2; ++x) {
            double sx = (2 * (x + .5) / 2 - 1) / 2, sy = (2 * (y + .5) / 3 - 1) * 1.5 / 2;
            double c = 1 / std::sqrt(1 + sx * sx + sy * sy);
            total += (expected[y * 2 + x] = values[y * 2 + x] * c * c * c);
        }
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 2; ++x) {
            auto row = 4 + y * 3;
            double p = (projection->cdf[y + 1] - projection->cdf[y]) *
                       (projection->cdf[row + x + 1] - projection->cdf[row + x]);
            require(std::abs(p - expected[y * 2 + x] / total) < 1e-7,
                    "Projection solid-angle weighting");
        }
    require(!build_light_direction_sampling(image, 0, error), "Direction table budget");
    require(!light_direction_sampling_bytes(0, 1), "Zero direction width");
}
void gltf_power() {
    ImagePyramid image{.encoding = ImageEncoding::srgb,
                       .wrap = ImageWrap::repeat,
                       .levels = {{2, 1, 0}, {1, 1, 8}},
                       .storage = ImageStorage::linear_float,
                       .source_channels = 4,
                       .gltf = true};
    for (const auto rgb : std::array{std::array{.2f, .4f, .6f}, std::array{.8f, .6f, .4f},
                                     std::array{.5f, .5f, .5f}}) {
        for (float value : rgb)
            image.texels.push_back(std::bit_cast<std::uint32_t>(value));
        image.texels.push_back(std::bit_cast<std::uint32_t>(1.f));
    }
    const auto value = light_image_power(image, {2, 1, .5f});
    const auto a = light_rgb_power({.4f, .4f, .3f}), b = light_rgb_power({1.6f, .6f, .2f});
    require(value && a && b && std::abs(*value - (*a + *b) / 2) < 1e-10,
            "glTF emission base RGB proxy");
    const auto bounds = light_image_bounds_power(image, {2, 1, .5f});
    require(bounds && std::abs(*bounds - ((.4 + .4 + .3) / 3 + (1.6 + .6 + .2) / 3) / 2) < 2e-8,
            "Area-image bounds use mean RGB, independently of spectral Phi");
    const auto projection_bounds = light_image_bounds_power(image, {1, 1, 1}, true);
    require(projection_bounds && std::abs(*projection_bounds - .7) < 2e-8,
            "Projection bounds use mean maximum RGB channel");
    image.texels.resize(7);
    require(!light_image_power(image), "Truncated glTF base must fail");
}
void rejection() {
    std::string error;
    std::vector<LightSamplingInput> input(1);
    input[0].power = -1;
    require(!build_light_sampling(input, 1024, error), "Negative power accepted");
    input[0].power = NAN;
    require(!build_light_sampling(input, 1024, error), "NaN power accepted");
    input[0].power = 1;
    input[0].high[0] = -1;
    require(!build_light_sampling(input, 1024, error), "Inverted bounds accepted");
    input[0].high[0] = 0;
    require(!build_light_sampling(input, 0, error), "Allocation budget ignored");
    require(!light_sampling_bytes(SIZE_MAX), "Size overflow accepted");
    input.resize(2);
    input[0].power = 1e100;
    input[1].power = 1e-100;
    require(!build_light_sampling(input, 4096, error), "Positive PMF underflow silently accepted");
}
} // namespace
int main() {
    try {
        alias_mass();
        separate_power_proxies();
        bounds_models();
        topology();
        direction_maps();
        gltf_power();
        rejection();
        std::cout << "Light selection aliases, BVH topology, and numeric budgets passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
