// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace rypath {
struct ImagePyramid;
struct LightSamplingInput {
    double power = 0;         // SampleLe/Phi-based alias weight.
    double bounds_power = -1; // PBRT Bounds().phi; -1 uses power for direct-API callers.
    std::array<float, 3> low{}, high{}, direction{0, 0, 1};
    float cos_outer = -1, cos_emission = 0;
    bool two_sided = true, infinite = false;
};
struct LightDirectionSampling {
    std::uint32_t width = 0, height = 0;
    std::vector<float> cdf;
};
std::optional<std::size_t> light_direction_sampling_bytes(std::uint32_t width,
                                                          std::uint32_t height);
std::optional<LightDirectionSampling>
build_light_direction_sampling(const ImagePyramid &image, std::size_t budget,
                               std::string &diagnostic, float inverse_tangent = 0,
                               float half_width = 1, float half_height = 1);
struct LightSamplingTables {
    std::vector<render_wire::LightAlias> alias;
    std::vector<render_wire::LightRecord> records;
    std::vector<render_wire::LightBounds> nodes;
    std::vector<std::uint32_t> infinite;
};
double light_spectrum_power(const std::array<float, 471> &spectrum);
std::optional<double> light_rgb_power(const std::array<float, 3> &rgb);
std::optional<double> light_image_power(const ImagePyramid &image,
                                        std::array<float, 3> tint = {1, 1, 1});
std::optional<double> light_image_bounds_power(const ImagePyramid &image,
                                               std::array<float, 3> tint = {1, 1, 1},
                                               bool maximum_channel = false);
// PBRT's rectangle classification and 3x3 quadrilateral area estimate.
bool light_patch_rectangle(const std::array<std::array<double, 3>, 4> &corners);
double light_patch_area(const std::array<std::array<double, 3>, 4> &corners);
// Tight transformed angular cones are used only when a similarity preserves angles.
void light_positional_cone(LightSamplingInput &input, const std::array<float, 9> &local_from_world,
                           float cos_outer, float cos_emission);
void light_normal_cone(LightSamplingInput &input, std::span<const std::array<double, 3>> normals);
std::optional<double> light_projection_power(const ImagePyramid &image, float inverse_tangent,
                                             float half_width, float half_height);
// Conservative maximum including host builder scratch; caller also charges copies.
std::optional<std::size_t> light_sampling_bytes(std::size_t lights);
std::optional<LightSamplingTables> build_light_sampling(std::span<const LightSamplingInput> input,
                                                        std::size_t byte_budget,
                                                        std::string &diagnostic);
} // namespace rypath
