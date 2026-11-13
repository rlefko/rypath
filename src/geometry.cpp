// SPDX-License-Identifier: GPL-3.0-only

#include "geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rypath {
namespace {

bool finite(GeometryPoint point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

template <class T> bool size_fits(std::size_t count) {
    return count <= std::numeric_limits<std::uint32_t>::max() &&
           count <= std::numeric_limits<std::size_t>::max() / sizeof(T);
}

// Grow an expansion using error-free addition (Shewchuk, 1997).
// https://www.cs.cmu.edu/~quake/robust.html
// Each insertion adds at most one term. There are twelve insertions, the high
// and low parts of six signed products of three floats.
class DeterminantExpansion {
  public:
    void add(double term) {
        std::size_t output = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            const double sum = term + values_[i];
            const double rounded_value = sum - term;
            const double error = (term - (sum - rounded_value)) + (values_[i] - rounded_value);
            if (error != 0) {
                values_[output++] = error;
            }
            term = sum;
        }
        if (term != 0) {
            values_[output++] = term;
        }
        size_ = output;
    }

    bool is_zero() const { return size_ == 0; }

  private:
    std::array<double, 12> values_{};
    std::size_t size_ = 0;
};

bool determinant_is_zero(const std::array<float, 9> &m) {
    // A product of two binary32 values is exact in binary64. A third factor may
    // round, but the FMA residual recovers its low part exactly.
    const std::array<double, 6> pairs{
        static_cast<double>(m[0]) * m[4],  static_cast<double>(m[1]) * m[5],
        static_cast<double>(m[2]) * m[3],  -static_cast<double>(m[0]) * m[5],
        -static_cast<double>(m[1]) * m[3], -static_cast<double>(m[2]) * m[4],
    };
    const std::array<float, 6> factors{m[8], m[6], m[7], m[7], m[8], m[6]};
    std::array<double, 6> products{};
    double estimate = 0;
    double magnitude = 0;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        products[i] = pairs[i] * factors[i];
        estimate += products[i];
        magnitude += std::abs(products[i]);
    }
    // The estimate has one rounded multiply and at most five rounded additions
    // per term. This conservative bound is only a fast path for a nonzero sign.
    if (std::abs(estimate) > 8 * std::numeric_limits<double>::epsilon() * magnitude) {
        return false;
    }
    if (magnitude == 0) {
        return true;
    }
    DeterminantExpansion exact;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        exact.add(products[i]);
        exact.add(std::fma(pairs[i], factors[i], -products[i]));
    }
    return exact.is_zero();
}

bool zero_area(GeometryPoint a, GeometryPoint b, GeometryPoint c) {
    return determinant_is_zero({a.x, a.y, 1, b.x, b.y, 1, c.x, c.y, 1}) &&
           determinant_is_zero({a.y, a.z, 1, b.y, b.z, 1, c.y, c.z, 1}) &&
           determinant_is_zero({a.z, a.x, 1, b.z, b.x, 1, c.z, c.x, 1});
}

} // namespace

std::string validate_geometry(const GeometryData &geometry) {
    if (!size_fits<GeometryMesh>(geometry.meshes.size()) ||
        !size_fits<GeometryInstance>(geometry.instances.size())) {
        return "Geometry mesh or instance count exceeds supported size";
    }
    for (std::size_t mesh_index = 0; mesh_index < geometry.meshes.size(); ++mesh_index) {
        const auto &mesh = geometry.meshes[mesh_index];
        const std::string mesh_name = "Mesh " + std::to_string(mesh_index);
        if (!size_fits<GeometryPoint>(mesh.positions.size()) ||
            !size_fits<std::array<std::uint32_t, 3>>(mesh.triangles.size())) {
            return mesh_name + " exceeds supported vertex or triangle count";
        }
        if (!mesh.normals.empty() && mesh.normals.size() != mesh.positions.size()) {
            return mesh_name + " must have one normal per vertex";
        }
        if (!mesh.uvs.empty() && mesh.uvs.size() != mesh.positions.size()) {
            return mesh_name + " must have one UV per vertex";
        }
        for (std::size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
            if (!finite(mesh.positions[vertex])) {
                return mesh_name + " vertex " + std::to_string(vertex) + " is nonfinite";
            }
            if (!mesh.normals.empty() && !finite(mesh.normals[vertex])) {
                return mesh_name + " normal " + std::to_string(vertex) + " is nonfinite";
            }
            if (!mesh.uvs.empty() &&
                (!std::isfinite(mesh.uvs[vertex][0]) || !std::isfinite(mesh.uvs[vertex][1]))) {
                return mesh_name + " UV " + std::to_string(vertex) + " is nonfinite";
            }
        }
        for (std::size_t primitive = 0; primitive < mesh.triangles.size(); ++primitive) {
            const auto [a, b, c] = mesh.triangles[primitive];
            if (a >= mesh.positions.size() || b >= mesh.positions.size() ||
                c >= mesh.positions.size()) {
                return mesh_name + " triangle " + std::to_string(primitive) +
                       " has an out-of-range vertex index";
            }
            if (zero_area(mesh.positions[a], mesh.positions[b], mesh.positions[c])) {
                return mesh_name + " triangle " + std::to_string(primitive) + " has zero area";
            }
        }
    }
    for (std::size_t index = 0; index < geometry.instances.size(); ++index) {
        const auto &instance = geometry.instances[index];
        const std::string name = "Instance " + std::to_string(index);
        if (instance.mesh_index >= geometry.meshes.size()) {
            return name + " has an out-of-range mesh index";
        }
        const auto &m = instance.transform.values;
        if (!std::ranges::all_of(m, [](float value) { return std::isfinite(value); })) {
            return name + " has a nonfinite transform";
        }
        if (determinant_is_zero({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]})) {
            return name + " has a singular transform";
        }
    }
    return {};
}

std::string validate_geometry_rays(std::span<const GeometryRay> rays) {
    if (!size_fits<GeometryRay>(rays.size())) {
        return "Geometry ray count exceeds supported size";
    }
    for (std::size_t index = 0; index < rays.size(); ++index) {
        const auto &ray = rays[index];
        if (!finite(ray.origin) || !finite(ray.direction)) {
            return "Ray " + std::to_string(index) + " has a nonfinite origin or direction";
        }
        if (ray.direction.x == 0 && ray.direction.y == 0 && ray.direction.z == 0) {
            return "Ray " + std::to_string(index) + " has zero direction";
        }
        if (!std::isfinite(ray.t_min) || ray.t_min < 0 || !(ray.t_max > ray.t_min)) {
            return "Ray " + std::to_string(index) + " has an invalid interval";
        }
    }
    return {};
}

} // namespace rypath
