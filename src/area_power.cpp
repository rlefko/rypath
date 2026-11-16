// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// Bilinear area/rectangle adaptation: pbrt-v4 b0bdf461, shapes.{cpp,h}.
// Copyright (c) 1998-2020, Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Licensed under Apache-2.0; see third_party/pbrt/LICENSE.txt and README.md.
#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>

namespace rypath {
std::optional<double> area_image_mean(const ImagePyramid &image) {
    if (!validate_image_light(image).empty())
        return {};
    const auto &base = image.levels.front();
    double sum = 0;
    for (std::uint32_t y = 0; y < base.height; ++y)
        for (std::uint32_t x = 0; x < base.width; ++x) {
            const auto rgb = image_base_texel_linear_rgb(image, x, y);
            if (!rgb)
                return {};
            sum +=
                .212673499227 * (*rgb)[0] + .715151622536 * (*rgb)[1] + .072174878238 * (*rgb)[2];
        }
    const double mean = sum / (double(base.width) * base.height);
    return mean > 0 && std::isfinite(mean) ? std::optional<double>{mean} : std::nullopt;
}
namespace {
using V = std::array<double, 3>;
V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V cross(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(V a) { return std::sqrt(dot(a, a)); }
V lerp(double t, V a, V b) {
    return {(1 - t) * a[0] + t * b[0], (1 - t) * a[1] + t * b[1], (1 - t) * a[2] + t * b[2]};
}
// Pinned PBRT b0bdf461 BilinearPatch::IsRectangle / constructor Area rule.
// Accumulate in binary64 over the rounded world-space binary32 corners.
double patch_area(const std::array<V, 4> &p) {
    V n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
    const double nl = length(n), diagonal = length(sub(p[3], p[0]));
    bool rectangle =
        nl > 0 && diagonal > 0 && std::abs(dot(sub(p[3], p[0]), n) / (diagonal * nl)) <= 1e-5;
    V center{};
    for (const auto &v : p)
        for (unsigned k = 0; k < 3; ++k)
            center[k] += v[k] / 4;
    const double d = dot(sub(p[0], center), sub(p[0], center));
    constexpr std::array<unsigned, 4> next{2, 0, 3, 1};
    for (unsigned i = 0; i < 4; ++i) {
        const auto delta = sub(p[i], center);
        rectangle = rectangle && length(sub(p[i], p[next[i]])) > 0 && d > 0 &&
                    std::abs(dot(delta, delta) - d) / d <= 1e-4;
    }
    if (rectangle)
        return length(sub(p[2], p[0])) * length(sub(p[1], p[0]));
    V grid[4][4];
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
            grid[i][j] = lerp(double(i) / 3, lerp(double(j) / 3, p[0], p[2]),
                              lerp(double(j) / 3, p[1], p[3]));
    double area = 0;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            area += .5 * length(cross(sub(grid[i + 1][j + 1], grid[i][j]),
                                      sub(grid[i + 1][j], grid[i][j + 1])));
    return area;
}
bool normal_or_zero(float x) { return x == 0 || (std::isfinite(x) && std::isnormal(x)); }
std::string prepare_instance_power(const RenderScene &scene, std::size_t index, double mean,
                                   std::vector<float> &factors) {
    const auto &emission = scene.area_emissions[scene.instance_emissions[index]];
    const auto &instance = scene.geometry.instances[index];
    if (instance.mesh_index >= scene.geometry.meshes.size())
        return "Area power mesh index is out of range";
    const auto &mesh = scene.geometry.meshes[instance.mesh_index];
    auto world = [&](std::uint32_t vertex, V &p) {
        if (vertex >= mesh.positions.size())
            return false;
        const auto q = area_world_vertex(mesh.positions[vertex], instance.transform);
        if (!q)
            return false;
        p = {q->x, q->y, q->z};
        return true;
    };
    auto append = [&](double area) -> std::string {
        const double f =
            emission.power / (std::numbers::pi * area * (emission.two_sided ? 2 : 1) * mean);
        const float factor = static_cast<float>(f);
        if (!(area > 0 && std::isfinite(area) && factor > 0 && std::isnormal(factor)))
            return "Area power normalization must be positive normal binary32";
        if (emission.image_texture == UINT32_MAX) {
            for (float value : emission.radiance)
                if (!normal_or_zero(value) || !normal_or_zero(value * factor) ||
                    (value > 0 && value * factor == 0))
                    return "Area power radiance product is outside normal binary32";
        } else if (!normal_or_zero(emission.image_scale * factor) ||
                   (emission.image_scale > 0 && emission.image_scale * factor == 0))
            return "Area power image scale product is outside normal binary32";
        factors.push_back(factor);
        return {};
    };
    for (const auto &t : mesh.triangles) {
        std::array<V, 3> p;
        for (unsigned k = 0; k < 3; ++k)
            if (!world(t[k], p[k]))
                return "Invalid area power world vertex";
        if (auto error = append(.5 * length(cross(sub(p[1], p[0]), sub(p[2], p[0]))));
            !error.empty())
            return error;
    }
    for (const auto &patch : mesh.patches) {
        std::array<V, 4> p;
        for (unsigned k = 0; k < 4; ++k)
            if (!world(patch[k], p[k]))
                return "Invalid area power world vertex";
        if (auto error = append(patch_area(p)); !error.empty())
            return error;
    }
    return {};
}
} // namespace
std::string prepare_area_power(const RenderScene &scene, std::vector<std::vector<float>> &factors,
                               std::size_t &failed_instance, std::size_t max_factors) {
    factors.clear();
    failed_instance = SIZE_MAX;
    if (scene.instance_emissions.empty() && scene.area_emissions.empty())
        return {};
    if (!scene.instance_emissions.empty() &&
        scene.instance_emissions.size() != scene.geometry.instances.size())
        return "Area power bindings must supply one index per instance";
    std::size_t count = 0;
    std::vector<std::size_t> first_binding(scene.area_emissions.size(), SIZE_MAX);
    for (std::size_t i = 0; i < scene.instance_emissions.size(); ++i) {
        const auto binding = scene.instance_emissions[i];
        failed_instance = i;
        if (binding == no_area_emission)
            continue;
        if (binding >= scene.area_emissions.size())
            return "Area power emission index is out of range";
        if (first_binding[binding] == SIZE_MAX)
            first_binding[binding] = i;
        if (scene.area_emissions[binding].power <= 0)
            continue;
        const auto mesh_index = scene.geometry.instances[i].mesh_index;
        if (mesh_index >= scene.geometry.meshes.size())
            return "Area power mesh index is out of range";
        const auto &mesh = scene.geometry.meshes[mesh_index];
        if (mesh.triangles.size() > max_factors - count ||
            mesh.patches.size() > max_factors - count - mesh.triangles.size())
            return "Area power factor budget exceeded";
        count += mesh.triangles.size() + mesh.patches.size();
    }
    failed_instance = SIZE_MAX;
    // A shared immutable image may serve many declarations and placements.
    std::map<const ImagePyramid *, double> image_means;
    std::set<const ImagePyramid *> checked_normal_images;
    std::vector<double> means(scene.area_emissions.size(), 1);
    for (std::size_t j = 0; j < scene.area_emissions.size(); ++j) {
        const auto &emission = scene.area_emissions[j];
        failed_instance = first_binding[j];
        if (!std::isfinite(emission.power) || emission.power < 0)
            return "Area power must be finite and nonnegative";
        if (emission.power == 0)
            continue;
        if (emission.gltf_material != UINT32_MAX)
            return "glTF emission cannot specify area power";
        if (emission.image_texture == UINT32_MAX)
            continue;
        if (emission.image_texture >= scene.image_textures.size() ||
            !scene.image_textures[emission.image_texture].image)
            return "Area power image index is out of range";
        const auto *image = scene.image_textures[emission.image_texture].image.get();
        auto found = image_means.find(image);
        if (found == image_means.end()) {
            const auto mean = area_image_mean(*image);
            if (!mean)
                return "Positive area power requires a positive finite image mean";
            found = image_means.emplace(image, *mean).first;
        }
        means[j] = found->second;
        if (emission.image_scale > 0 && checked_normal_images.insert(image).second) {
            const auto &base = image->levels.front();
            for (std::uint32_t y = 0; y < base.height; ++y)
                for (std::uint32_t x = 0; x < base.width; ++x) {
                    const auto rgb = image_base_texel_linear_rgb(*image, x, y);
                    if (!rgb || !std::ranges::all_of(*rgb, normal_or_zero))
                        return "Positive area power image components must be normal binary32 or "
                               "zero";
                    const float amplitude = 2 * *std::ranges::max_element(*rgb);
                    if (amplitude > 0)
                        for (float component : *rgb) {
                            const float ratio = component / amplitude;
                            if (!normal_or_zero(ratio) || (component > 0 && ratio == 0))
                                return "Positive area power image reconstruction loses an RGB "
                                       "component";
                        }
                }
        }
    }
    factors.resize(scene.geometry.instances.size());
    for (std::size_t i = 0; i < scene.instance_emissions.size(); ++i) {
        const auto binding = scene.instance_emissions[i];
        if (binding == no_area_emission || scene.area_emissions[binding].power == 0)
            continue;
        failed_instance = i;
        if (auto error = prepare_instance_power(scene, i, means[binding], factors[i]);
            !error.empty())
            return error;
    }
    failed_instance = SIZE_MAX;
    return {};
}
} // namespace rypath
