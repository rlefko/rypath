// SPDX-License-Identifier: GPL-3.0-only
#include "triangle_attributes.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

GeometryMesh mesh() {
    GeometryMesh result;
    result.positions = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    result.triangles = {{0, 1, 2}};
    return result;
}

void transforms() {
    auto source = mesh();
    source.normals = {{3, 0, 1}, {0, 2, 1}, {0, 0, 0}};
    GeometryTransform transform{{2, 1, 0, 7, 0, 3, .5f, -9, 0, 0, -4, 11}};
    std::vector<GeometryPoint> packed;
    require(prepare_shading_normals(source, transform, true, packed).empty(),
            "Reflected/sheared normals should prepare");
    // Independently inverted triangular matrix, not the production cofactors.
    const std::array<std::array<double, 3>, 3> expected{
        {{1.5, -.5, -.3125}, {0, 2. / 3, -1. / 6}, {0, 0, 0}}};
    const double common = packed[0].x / expected[0][0];
    require(common > 0, "Reflection must not reverse authored normals");
    for (std::size_t i = 0; i < packed.size(); ++i) {
        const std::array<double, 3> actual{packed[i].x, packed[i].y, packed[i].z};
        for (std::size_t c = 0; c < 3; ++c) {
            require(std::abs(actual[c] - common * expected[i][c]) < 2e-7,
                    "Inverse transpose must preserve relative normal lengths");
        }
    }
    const auto original = packed;
    transform.values[3] = -13;
    transform.values[7] = 27;
    transform.values[11] = 0;
    require(prepare_shading_normals(source, transform, true, packed).empty(),
            "Translation must not affect preparation");
    for (std::size_t i = 0; i < packed.size(); ++i) {
        require(packed[i].x == original[i].x && packed[i].y == original[i].y &&
                    packed[i].z == original[i].z,
                "Normal bytes must be translation independent");
    }
    require(prepare_shading_normals(source, transform, false, packed).empty(),
            "Authored reversal should prepare");
    for (std::size_t i = 0; i < packed.size(); ++i) {
        require(packed[i].x == -original[i].x && packed[i].y == -original[i].y &&
                    packed[i].z == -original[i].z,
                "Authored reversal must flip every normal exactly once");
    }
}

void numeric_domain() {
    auto source = mesh();
    std::vector<GeometryPoint> packed{{1, 2, 3}};
    require(prepare_shading_normals(source, {}, false, packed).empty() && packed.empty(),
            "Missing attributes must clear the prepared table");
    source.normals = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    require(prepare_shading_normals(source, {}, false, packed).empty() && packed.size() == 3,
            "Authored zero normals require geometric fallback");
    source.normals = {{0, 0, 1}, {0, 0, -1}, {0, 0, 0}};
    require(prepare_shading_normals(source, {}, false, packed).empty(),
            "Cancelling normal fields are supported");
    require(packed[0].z == -packed[1].z && packed[2].z == 0,
            "Normal cancellation must not be repaired per vertex");
    const float high = std::numeric_limits<float>::max();
    const float low = std::numeric_limits<float>::min();
    GeometryTransform extreme{{high, 0, 0, 0, 0, high, 0, 0, 0, 0, low, 0}};
    source.normals = {{0, 0, 1}, {0, 0, 2}, {0, 0, 3}};
    require(prepare_shading_normals(source, extreme, false, packed).empty(),
            "Common scaling must avoid determinant overflow");
    require(packed[0].z > 0 && packed[2].z < 1 && std::abs(packed[2].z / packed[0].z - 3) < 1e-6,
            "Extreme transformed normal lengths must retain their ratios");
    source.normals = {{0, 0, high}, {0, 0, low}, {0, 0, 0}};
    require(!prepare_shading_normals(source, {}, false, packed).empty() && packed.empty(),
            "Nonzero vectors lost to GPU subnormal flushing must fail visibly");
    source.positions.push_back({3, 3, 0});
    source.normals = {{1, 0, -1e-30f}, {1, 0, -1e-30f}, {1, 0, -1e-30f}, {1e18f, 0, 0}};
    require(!prepare_shading_normals(source, {}, false, packed).empty() && packed.empty(),
            "An unused large normal must not erase the component deciding emitter sidedness");
    source.positions.pop_back();
    source.normals.pop_back();
    source.normals[0].z = std::numeric_limits<float>::infinity();
    require(!prepare_shading_normals(source, {}, false, packed).empty(),
            "Nonfinite input normals must fail");
    source.normals.pop_back();
    require(!prepare_shading_normals(source, {}, false, packed).empty(),
            "Malformed programmatic attribute counts must fail");
}

void emitter_ties() {
    auto source = mesh();
    std::vector<GeometryPoint> packed;
    for (const GeometryPoint n : {GeometryPoint{}, GeometryPoint{1, 0, 0}}) {
        source.normals.assign(3, n);
        require(prepare_shading_normals(source, {}, true, packed).empty(),
                "Degenerate emission attributes should prepare");
        require(!validate_normal_emission(source, {}, true, false, packed).empty(),
                "Conflicting one-sided PBRT orientation must be diagnosed");
        require(validate_normal_emission(source, {}, false, false, packed).empty(),
                "An unflipped tie has no PBRT hit/sample conflict");
        require(validate_normal_emission(source, {}, true, true, packed).empty(),
                "Two-sided emission is unaffected by tied orientation");
        GeometryTransform reflected{{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
        require(!validate_normal_emission(source, reflected, true, false, packed).empty(),
                "Mirrored raw and fallback winding can also disagree");
    }
    source.normals = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}};
    require(prepare_shading_normals(source, {}, true, packed).empty(),
            "Mixed emitting orientations should prepare");
    require(validate_normal_emission(source, {}, true, false, packed).empty(),
            "A measure-zero orientation tie must not reject mixed normal fields");
    source.positions = {{0, 0, 0}, {1e18f, 0, -1e-35f}, {0, 1, 0}};
    source.normals.assign(3, {-1, 0, 0});
    require(prepare_shading_normals(source, {}, true, packed).empty(),
            "Tiny plane projection witness must have representable vertex normals");
    require(!validate_normal_emission(source, {}, true, false, packed).empty(),
            "Plane normalization must not hide a positive-area PBRT orientation conflict");
    require(validate_normal_emission(source, {}, true, true, packed).empty(),
            "Plane orientation uncertainty is irrelevant to two-sided emission");
    source.positions = {{-1, -1, 3}, {2, -1, 2}, {-1, 2, 1}};
    source.normals.assign(3, {1, 1, -1});
    require(prepare_shading_normals(source, {}, false, packed).empty(),
            "Oblique tangent normals should prepare");
    require(!validate_normal_emission(source, {}, false, false, packed).empty(),
            "An everywhere ambiguous oblique tangent must not choose an arbitrary emitting side");
    constexpr float delta = 0x1p-21f;
    source.positions = {{0, 0, 3}, {1, 1, 4 + delta}, {0, 1 - delta, 4}};
    source.normals.assign(3, {-1, 0, 0});
    require(prepare_shading_normals(source, {}, false, packed).empty(),
            "Cross cancellation witness must have representable vertex normals");
    require(!validate_normal_emission(source, {}, false, false, packed).empty(),
            "Rounded cross cancellation must not silently reverse the whole emitting face");
}
} // namespace

int main() {
    try {
        transforms();
        numeric_domain();
        emitter_ties();
        std::cout << "Triangle attribute preparation checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
