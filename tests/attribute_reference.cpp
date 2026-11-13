// SPDX-License-Identifier: GPL-3.0-only

#include "attribute_reference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace attribute_reference {
namespace {

Moments at_receiver(std::array<double, 2> receiver, bool affine, int steps) {
    const auto [x, y] = receiver;
    double nx = 3, ny = 0, nz = -1;
    if (affine) {
        // World receiver triangle is (-4.5,-1), (-2.5,3), (3.5,-1).
        // The barycentric coordinates follow from its planar edges. Object
        // normals are (0,0,-1), (16,0,-4), (-2,1,-2) and are still unnormalized.
        const double b = (y + 1) / 4;
        const double c = (x + 4.5 - 2 * b) / 8;
        const double a = 1 - b - c;
        const double local_x = 16 * b - 2 * c;
        nx = local_x / 2;
        ny = c - local_x / 4;
        nz = (-a - 4 * b - 2 * c) / 3;
    }
    const double normal_length = std::hypot(nx, ny, nz);

    constexpr double width = light_xmax - light_xmin;
    constexpr double height = light_ymax - light_ymin;
    constexpr double area = width * height / 2;
    Moments result;
    double light_mean = 0;
    for (int i = 0; i < steps; ++i) {
        const double u = (i + 0.5) / steps;
        for (int j = 0; j < steps; ++j) {
            const double v = (j + 0.5) / steps;
            // Parameterize the horizontal chord at height v, with Jacobian
            // width * height * v. The camera always sees the receiver first.
            const double dx = light_xmin + u * v * width - x;
            const double dy = light_ymin + v * height - y;
            const double r = std::hypot(dx, dy, light_z);
            const double projection = (nx * dx + ny * dy + nz * light_z) / (r * normal_length);
            const double domega_darea = light_z / (r * r * r);
            const double bsdf_pdf = std::max(0.0, projection) / std::numbers::pi;
            const double weight = 2 * v / (static_cast<double>(steps) * steps);
            const double probability_ratio = area * bsdf_pdf * domega_darea;
            // In normalized emitter-area measure, the area density is 1.
            // Integrate the physical response directly and use generic MIS
            // densities only for the variance of its two independent estimates.
            const double w_light = 1 / (1 + probability_ratio * probability_ratio);
            const double w_bsdf = 1 - w_light;
            const double light_value = w_light * probability_ratio;
            result.mean += weight * probability_ratio;
            light_mean += weight * light_value;
            result.bsdf_mean += weight * probability_ratio * w_bsdf;
            result.second +=
                weight * (light_value * light_value + probability_ratio * w_bsdf * w_bsdf);
        }
    }
    result.second += 2 * light_mean * result.bsdf_mean;
    result.mean *= reflectance;
    result.bsdf_mean *= reflectance;
    result.second *= reflectance * reflectance;
    return result;
}

} // namespace

Moments furnace_moments() {
    // Uniform-sphere light sampling has cosine density 1/2 on [0,1].
    // The cosine-weighted BSDF has density 2c on the same interval.
    constexpr int steps = 4096;
    double light_mean = 0, bsdf_mean = 0, second = 0;
    for (int i = 0; i < steps; ++i) {
        const double c = (i + 0.5) / steps;
        const double ratio = 4 * c;
        const double light_value = ratio / (1 + ratio * ratio);
        const double bsdf_value = ratio * light_value;
        light_mean += 0.5 * light_value / steps;
        bsdf_mean += 2 * c * bsdf_value / steps;
        second += (0.5 * light_value * light_value + 2 * c * bsdf_value * bsdf_value) / steps;
    }
    second += 2 * light_mean * bsdf_mean;
    return {reflectance, reflectance * reflectance * second, reflectance * bsdf_mean};
}

Moments area_moments(bool affine, QuadratureSteps steps) {
    const auto [raster_steps, emitter_steps] = steps;
    Moments result;
    const double weight = 1 / (static_cast<double>(raster_steps) * raster_steps);
    for (int i = 0; i < raster_steps; ++i) {
        const double x = footprint * (2 * (i + 0.5) / raster_steps - 1);
        for (int j = 0; j < raster_steps; ++j) {
            const double y = footprint * (2 * (j + 0.5) / raster_steps - 1);
            const auto moments = at_receiver({x, y}, affine, emitter_steps);
            result.mean += weight * moments.mean;
            result.second += weight * moments.second;
            result.bsdf_mean += weight * moments.bsdf_mean;
        }
    }
    return result;
}

} // namespace attribute_reference
