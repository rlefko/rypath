// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// PBRT v4 b0bdf461 light bounds, angular SAH and bilinear-area adaptations.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt and README.md.
#include "light_sampling.hpp"
#include "image_texture.hpp"
#include "packet_spectrum_shared.h"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <numeric>
namespace rypath {
double light_spectrum_power(const std::array<float, 471> &spectrum) {
    const auto lambda = rypath_packet_spectrum::sample_visible(.5f);
    double value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value += double(spectrum[static_cast<unsigned>(std::round(lambda.lambda[i]) - 360)]) /
                 lambda.pdf[i] / 4;
    if (value == 0)
        for (float v : spectrum)
            value += v;
    return value;
}
std::optional<double> light_rgb_power(const std::array<float, 3> &rgb) {
    const float amplitude = 2 * std::max({rgb[0], rgb[1], rgb[2]});
    if (amplitude == 0)
        return 0;
    if (!std::isfinite(amplitude))
        return {};
    const auto spectrum =
        reconstruct_srgb({rgb[0] / amplitude, rgb[1] / amplitude, rgb[2] / amplitude});
    if (!spectrum)
        return {};
    const auto lambda = rypath_packet_spectrum::sample_visible(.5f);
    double value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        auto w = static_cast<unsigned>(std::round(lambda.lambda[i]) - 360);
        value += double(amplitude) * evaluate_srgb(*spectrum, float(w + 360)) * pbrt_d65()[w] /
                 lambda.pdf[i] / 4;
    }
    return value;
}
namespace {
std::optional<std::array<float, 3>> proposal_texel(const ImagePyramid &image, unsigned x,
                                                   unsigned y) {
    if (!image.gltf)
        return image_base_texel_linear_rgb(image, x, y);
    // glTF retains already-linear RGBA float mip pyramids rather than native light images.
    // Read its checked base representation without weakening PBRT light-image validation.
    if (image.storage != ImageStorage::linear_float || image.source_channels != 4 ||
        image.levels.empty())
        return {};
    const auto &base = image.levels.front();
    if (!base.width || !base.height || base.width > image_dimension_ceiling ||
        base.height > image_dimension_ceiling || x >= base.width || y >= base.height ||
        base.offset % 4 || base.offset > image.texels.size())
        return {};
    const auto words = std::size_t(base.width) * base.height * 4;
    if (words > image.texels.size() - base.offset)
        return {};
    const auto index = base.offset + 4 * (std::size_t(y) * base.width + x);
    std::array<float, 3> rgb;
    for (unsigned c = 0; c < 3; ++c) {
        rgb[c] = std::bit_cast<float>(image.texels[index + c]);
        if (!std::isfinite(rgb[c]) || rgb[c] < 0)
            return {};
    }
    const float alpha = std::bit_cast<float>(image.texels[index + 3]);
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 1)
        return {};
    return rgb;
}
} // namespace
std::optional<double> light_image_power(const ImagePyramid &image, std::array<float, 3> tint) {
    if (image.levels.empty())
        return {};
    const auto &base = image.levels.front();
    double sum = 0;
    for (unsigned y = 0; y < base.height; ++y)
        for (unsigned x = 0; x < base.width; ++x) {
            const auto rgb = proposal_texel(image, x, y);
            if (!rgb)
                return {};
            const auto value =
                light_rgb_power({(*rgb)[0] * tint[0], (*rgb)[1] * tint[1], (*rgb)[2] * tint[2]});
            if (!value)
                return {};
            sum += *value;
        }
    return sum / (double(base.width) * base.height);
}
std::optional<double> light_image_bounds_power(const ImagePyramid &image, std::array<float, 3> tint,
                                               bool maximum_channel) {
    if (image.levels.empty())
        return {};
    const auto &base = image.levels.front();
    double sum = 0;
    for (unsigned y = 0; y < base.height; ++y)
        for (unsigned x = 0; x < base.width; ++x) {
            const auto rgb = proposal_texel(image, x, y);
            if (!rgb)
                return {};
            const std::array<double, 3> value{double((*rgb)[0]) * tint[0],
                                              double((*rgb)[1]) * tint[1],
                                              double((*rgb)[2]) * tint[2]};
            sum += maximum_channel ? *std::max_element(value.begin(), value.end())
                                   : (value[0] + value[1] + value[2]) / 3;
        }
    return sum / (double(base.width) * base.height);
}
std::optional<double> light_projection_power(const ImagePyramid &image, float inverse_tangent,
                                             float half_width, float half_height) {
    if (image.levels.empty())
        return {};
    const auto &base = image.levels.front();
    double sum = 0;
    for (unsigned y = 0; y < base.height; ++y)
        for (unsigned x = 0; x < base.width; ++x) {
            const auto rgb = image_base_texel_linear_rgb(image, x, y);
            if (!rgb)
                return {};
            const auto value = light_rgb_power(*rgb);
            if (!value)
                return {};
            const double sx = (2 * (x + .5) / base.width - 1) * half_width / inverse_tangent;
            const double sy = (2 * (y + .5) / base.height - 1) * half_height / inverse_tangent;
            const double cosine = 1 / std::sqrt(1 + sx * sx + sy * sy);
            sum += *value * cosine * cosine * cosine;
        }
    return 4 * double(half_width) * half_height / (double(inverse_tangent) * inverse_tangent) *
           sum / (double(base.width) * base.height);
}
std::optional<std::size_t> light_sampling_bytes(std::size_t n) {
    constexpr std::size_t stride = 256;
    if (n > (std::numeric_limits<std::size_t>::max() - 256) / stride || n > UINT32_MAX / 2)
        return {};
    return 256 + stride * n;
}
std::optional<std::size_t> light_direction_sampling_bytes(std::uint32_t width,
                                                          std::uint32_t height) {
    if (!width || !height || width > image_dimension_ceiling || height > image_dimension_ceiling)
        return {};
    const auto count = std::size_t(height) + 1 + std::size_t(height) * (std::size_t(width) + 1);
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(float))
        return {};
    return count * sizeof(float);
}
// The public API names the ordered projection extents; keep its existing signature.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::optional<LightDirectionSampling>
build_light_direction_sampling(const ImagePyramid &image, std::size_t budget, std::string &error,
                               float invtan, float half_width, float half_height) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    error.clear();
    if (image.levels.empty()) {
        error = "Light direction sampling requires an image";
        return {};
    }
    const auto &w = image.levels.front();
    const auto bytes = light_direction_sampling_bytes(w.width, w.height);
    if (!bytes || *bytes > budget) {
        error = "Light direction sampling table exceeds resource budget";
        return {};
    }
    LightDirectionSampling out;
    out.width = w.width;
    out.height = w.height;
    out.cdf.resize(*bytes / sizeof(float));
    std::vector<double> row(std::size_t(w.width) + 1), sums(w.height);
    double total = 0;
    for (unsigned y = 0; y < w.height; ++y) {
        row[0] = 0;
        for (unsigned x = 0; x < w.width; ++x) {
            auto rgb = image_base_texel_linear_rgb(image, x, y);
            if (!rgb) {
                error = "Invalid directional light image texel";
                return {};
            }
            double mass = (double((*rgb)[0]) + (*rgb)[1] + (*rgb)[2]) / 3;
            if (invtan > 0) {
                double sx = (2 * (x + .5) / w.width - 1) * half_width / invtan,
                       sy = (2 * (y + .5) / w.height - 1) * half_height / invtan;
                double cosine = 1 / std::sqrt(1 + sx * sx + sy * sy);
                mass *= cosine * cosine * cosine;
            }
            if (!(mass >= 0 && std::isfinite(mass))) {
                error = "Invalid directional light sampling weight";
                return {};
            }
            row[x + 1] = row[x] + mass;
        }
        sums[y] = row.back();
        total += sums[y];
        const auto offset = std::size_t(w.height) + 1 + std::size_t(y) * (w.width + 1);
        for (unsigned x = 1; x <= w.width; ++x)
            out.cdf[offset + x] = sums[y] > 0 ? static_cast<float>(row[x] / sums[y])
                                              : float(x) / static_cast<float>(w.width);
        out.cdf[offset + w.width] = 1;
    }
    double sum = 0;
    for (unsigned y = 0; y < w.height; ++y) {
        sum += sums[y];
        out.cdf[y + 1] = total > 0 ? static_cast<float>(sum / total)
                                   : float(y + 1) / static_cast<float>(w.height);
    }
    out.cdf[w.height] = 1;
    return out;
}
namespace {
using V = std::array<double, 3>;
V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(V a) { return std::sqrt(dot(a, a)); }
V crossv(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V lerpv(double t, V a, V b) {
    return {(1 - t) * a[0] + t * b[0], (1 - t) * a[1] + t * b[1], (1 - t) * a[2] + t * b[2]};
}

V vector(std::array<float, 3> a) { return {a[0], a[1], a[2]}; }
LightSamplingInput unite(const LightSamplingInput &a, const LightSamplingInput &b) {
    if (a.power == 0)
        return b;
    if (b.power == 0)
        return a;
    LightSamplingInput r = a;
    r.power = a.power + b.power;
    r.two_sided = a.two_sided || b.two_sided;
    r.cos_emission = std::min(a.cos_emission, b.cos_emission);
    for (unsigned i = 0; i < 3; ++i) {
        r.low[i] = std::min(a.low[i], b.low[i]);
        r.high[i] = std::max(a.high[i], b.high[i]);
    }
    const double ta = std::acos(std::clamp(double(a.cos_outer), -1., 1.)),
                 tb = std::acos(std::clamp(double(b.cos_outer), -1., 1.));
    V wa = vector(a.direction), wb = vector(b.direction);
    const double angle = std::acos(std::clamp(dot(wa, wb), -1., 1.));
    if (std::min(angle + tb, std::numbers::pi) <= ta)
        return r;
    if (std::min(angle + ta, std::numbers::pi) <= tb) {
        r.direction = b.direction;
        r.cos_outer = b.cos_outer;
        return r;
    }
    const double theta = (ta + tb + angle) / 2 + 1e-6;
    if (theta >= std::numbers::pi) {
        r.cos_outer = -1;
        r.direction = {0, 0, 1};
        return r;
    }
    V tangent;
    for (unsigned i = 0; i < 3; ++i)
        tangent[i] = wb[i] - dot(wa, wb) * wa[i];
    double length = norm(tangent);
    if (length < 1e-12) {
        unsigned axis = std::abs(wa[0]) < .8 ? 0 : 1;
        V basis{};
        basis[axis] = 1;
        for (unsigned i = 0; i < 3; ++i)
            tangent[i] = basis[i] - wa[axis] * wa[i];
        length = norm(tangent);
    }
    const double rotate = theta - ta;
    for (unsigned i = 0; i < 3; ++i)
        r.direction[i] =
            static_cast<float>(std::cos(rotate) * wa[i] + std::sin(rotate) * tangent[i] / length);
    const double direction_length = norm(vector(r.direction));
    for (auto &v : r.direction)
        v = static_cast<float>(v / direction_length);
    r.cos_outer = std::nextafter(static_cast<float>(std::cos(theta)), -INFINITY);
    return r;
}
double cost(const LightSamplingInput &b, const LightSamplingInput &all, unsigned axis) {
    if (b.power == 0)
        return 0;
    const double o = std::acos(std::clamp(double(b.cos_outer), -1., 1.)),
                 e = std::acos(std::clamp(double(b.cos_emission), -1., 1.));
    const double w = std::min(o + e, std::numbers::pi), pi = std::numbers::pi;
    const double omega =
        2 * pi * (1 - b.cos_outer) +
        pi / 2 * (2 * w * std::sin(o) - std::cos(o - 2 * w) - 2 * o * std::sin(o) + b.cos_outer);
    V d = sub(vector(b.high), vector(b.low)), ad = sub(vector(all.high), vector(all.low));
    if (ad[axis] <= 0)
        return INFINITY;
    return b.power * omega * (*std::max_element(ad.begin(), ad.end()) / ad[axis]) * 2 *
           (d[0] * d[1] + d[1] * d[2] + d[2] * d[0]);
}
} // namespace
bool light_patch_rectangle(const std::array<std::array<double, 3>, 4> &p) {
    V n = crossv(sub(p[1], p[0]), sub(p[2], p[0]));
    const double nl = norm(n), diagonal = norm(sub(p[3], p[0]));
    bool rectangle =
        nl > 0 && diagonal > 0 && std::abs(dot(sub(p[3], p[0]), n) / (diagonal * nl)) <= 1e-5;
    V center{};
    for (const auto &v : p)
        for (unsigned a = 0; a < 3; ++a)
            center[a] += v[a] / 4;
    const double d = dot(sub(p[0], center), sub(p[0], center));
    constexpr std::array<unsigned, 4> next{2, 0, 3, 1};
    for (unsigned i = 0; i < 4; ++i) {
        const auto delta = sub(p[i], center);
        rectangle = rectangle && norm(sub(p[i], p[next[i]])) > 0 && d > 0 &&
                    std::abs(dot(delta, delta) - d) / d <= 1e-4;
    }
    return rectangle;
}
double light_patch_area(const std::array<std::array<double, 3>, 4> &p) {
    if (light_patch_rectangle(p))
        return norm(sub(p[2], p[0])) * norm(sub(p[1], p[0]));
    V grid[4][4];
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
            grid[i][j] = lerpv(double(i) / 3, lerpv(double(j) / 3, p[0], p[2]),
                               lerpv(double(j) / 3, p[1], p[3]));
    double area = 0;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            area += .5 * norm(crossv(sub(grid[i + 1][j + 1], grid[i][j]),
                                     sub(grid[i + 1][j], grid[i][j + 1])));
    return area;
}
// The public API distinguishes the outer cone from the emission cone by name.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void light_positional_cone(LightSamplingInput &input, const std::array<float, 9> &m, float outer,
                           float emission) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    V a{m[0], m[1], m[2]}, b{m[3], m[4], m[5]}, c{m[6], m[7], m[8]};
    double aa = dot(a, a), bb = dot(b, b), cc = dot(c, c), tolerance = 0x1p-20 * aa;
    // A nonuniform affine direction map does not preserve the local cone's angles.
    // Its full-sphere bound retains support for the unchanged transformed emission.
    input.two_sided = false;
    input.cos_outer = -1;
    input.cos_emission = 0;
    if (!(aa > 0 && std::isfinite(aa)) || std::abs(aa - bb) > tolerance ||
        std::abs(aa - cc) > tolerance || std::abs(dot(a, b)) > tolerance ||
        std::abs(dot(a, c)) > tolerance || std::abs(dot(b, c)) > tolerance)
        return;
    auto axis = crossv(a, b);
    double determinant = dot(axis, c);
    if (!(std::abs(determinant) > 0))
        return;
    const double length = norm(axis);
    for (unsigned i = 0; i < 3; ++i)
        input.direction[i] = static_cast<float>(axis[i] / std::copysign(length, determinant));
    // Conservative angular slop covers accepted binary32 similarity roundoff.
    input.cos_outer = static_cast<float>(
        std::cos(std::min(std::numbers::pi, std::acos(std::clamp(double(outer), -1., 1.)) + 2e-6)));
    input.cos_outer = std::max(-1.f, std::nextafter(input.cos_outer, -INFINITY));
    input.cos_emission = emission;
}
void light_normal_cone(LightSamplingInput &input, std::span<const std::array<double, 3>> normals) {
    V axis{};
    for (const auto &n : normals) {
        double length = norm(n);
        if (!(length > 0 && std::isfinite(length))) {
            input.cos_outer = -1;
            return;
        }
        for (unsigned a = 0; a < 3; ++a)
            axis[a] += n[a] / length;
    }
    double length = norm(axis);
    if (!(length > 0)) {
        input.cos_outer = -1;
        return;
    }
    for (auto &v : axis)
        v /= length;
    double cosine = 1;
    for (const auto &n : normals)
        cosine = std::min(cosine, dot(axis, n) / norm(n));
    for (unsigned a = 0; a < 3; ++a)
        input.direction[a] = static_cast<float>(axis[a]);
    input.cos_outer = std::nextafter(static_cast<float>(std::clamp(cosine, -1., 1.)), -INFINITY);
    input.cos_outer = std::max(-1.f, input.cos_outer);
}
std::optional<LightSamplingTables> build_light_sampling(std::span<const LightSamplingInput> in,
                                                        std::size_t budget, std::string &error) {
    error.clear();
    const auto bytes = light_sampling_bytes(in.size());
    if (!bytes || *bytes > budget) {
        error = "Light sampling tables exceed preparation budget";
        return {};
    }
    LightSamplingTables out;
    out.records.resize(in.size(), {UINT32_MAX, 0, 0, 0});
    out.alias.resize(in.size());
    if (in.empty())
        return out;
    double total = 0, bounds_total = 0;
    std::vector<double> probabilities(in.size());
    std::vector<LightSamplingInput> bvh(in.begin(), in.end());
    std::vector<std::uint32_t> finite;
    finite.reserve(in.size());
    out.nodes.reserve(in.size() * 2 - 1);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto &v = in[i];
        if (!(v.power >= 0 && std::isfinite(v.power))) {
            error = "Invalid light sampling power";
            return {};
        }
        for (unsigned a = 0; a < 3; ++a)
            if (!std::isfinite(v.low[a]) || !std::isfinite(v.high[a]) || v.low[a] > v.high[a]) {
                error = "Invalid light sampling bounds";
                return {};
            }
        if (!(v.bounds_power == -1 || (v.bounds_power >= 0 && std::isfinite(v.bounds_power)))) {
            error = "Invalid light bounds power";
            return {};
        }
        auto &bounds = bvh[i];
        bounds.power = v.bounds_power < 0 ? v.power : v.bounds_power;
        // A zero proxy must never remove a light whose Phi estimate proves it nonblack.
        if (bounds.power == 0 && v.power > 0)
            bounds.power = v.power;
        // Fixed-wavelength Phi can miss a nonblack narrow spectrum. A positive
        // bounds proxy supplies support without altering either light's emission.
        probabilities[i] = v.power == 0 && bounds.power > 0 ? bounds.power : v.power;
        total += probabilities[i];
        if (!v.infinite)
            bounds_total += bounds.power;
        if (v.infinite) {
            out.infinite.push_back(static_cast<std::uint32_t>(i));
            out.records[i].flags = 1;
        } else if (bounds.power > 0)
            finite.push_back(static_cast<std::uint32_t>(i));
    }
    if (!std::isfinite(total) || !std::isfinite(bounds_total)) {
        error = "Light sampling total power overflow";
        return {};
    }
    for (auto id : finite) {
        const auto normalized = static_cast<float>(bvh[id].power / bounds_total);
        if (!(normalized >= std::numeric_limits<float>::min())) {
            error = "Positive light bounds power underflows binary32";
            return {};
        }
    }
    std::vector<std::uint32_t> small, large;
    small.reserve(in.size());
    large.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        double p = total > 0 ? probabilities[i] / total : 1. / static_cast<double>(in.size());
        out.alias[i].pmf = static_cast<float>(p);
        if (p > 0 && !(out.alias[i].pmf >= std::numeric_limits<float>::min())) {
            error = "Light selection PMF underflows binary32";
            return {};
        }
        probabilities[i] = p * static_cast<double>(in.size());
        (probabilities[i] < 1 ? small : large).push_back(static_cast<std::uint32_t>(i));
    }
    while (!small.empty() && !large.empty()) {
        auto s = small.back();
        small.pop_back();
        auto l = large.back();
        large.pop_back();
        out.alias[s].threshold = static_cast<float>(probabilities[s]);
        out.alias[s].alias = l;
        probabilities[l] -= 1 - probabilities[s];
        (probabilities[l] < 1 ? small : large).push_back(l);
    }
    for (auto i : small) {
        out.alias[i].threshold = 1;
        out.alias[i].alias = i;
    }
    for (auto i : large) {
        out.alias[i].threshold = 1;
        out.alias[i].alias = i;
    }
    struct BuildState {
        std::uint32_t parent;
        unsigned depth;
    };
    std::function<std::uint32_t(std::size_t, std::size_t, BuildState)> build;
    build = [&](std::size_t begin, std::size_t end, BuildState state) {
        const auto [parent, depth] = state;
        const auto index = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.emplace_back();
        LightSamplingInput bounds;
        constexpr double infinity = std::numeric_limits<double>::infinity();
        std::array<double, 3> clo{infinity, infinity, infinity},
            chi{-infinity, -infinity, -infinity};
        for (auto k = begin; k < end; ++k) {
            const auto &v = bvh[finite[k]];
            bounds = unite(bounds, v);
            for (unsigned a = 0; a < 3; ++a) {
                double c = (double(v.low[a]) + v.high[a]) / 2;
                clo[a] = std::min(clo[a], c);
                chi[a] = std::max(chi[a], c);
            }
        }
        render_wire::LightBounds node{};
        node.parent = parent;
        node.light = UINT32_MAX;
        node.low = {bounds.low[0], bounds.low[1], bounds.low[2]};
        node.high = {bounds.high[0], bounds.high[1], bounds.high[2]};
        node.power = static_cast<float>(bounds.power / bounds_total);
        node.direction = {bounds.direction[0], bounds.direction[1], bounds.direction[2]};
        node.cos_outer = bounds.cos_outer;
        node.cos_emission = bounds.cos_emission;
        node.flags = bounds.two_sided ? 1 : 0;
        if (end - begin == 1) {
            auto id = finite[begin];
            node.light = id;
            out.records[id].leaf = index;
        } else {
            double best = INFINITY;
            unsigned split_axis = 0, split_bucket = 0;
            bool found = false;
            if (depth < 48)
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (clo[axis] == chi[axis])
                        continue;
                    std::array<LightSamplingInput, 12> buckets{};
                    for (auto k = begin; k < end; ++k) {
                        const auto &v = bvh[finite[k]];
                        auto b = std::min(
                            11u, static_cast<unsigned>(
                                     12 * ((double(v.low[axis]) + v.high[axis]) / 2 - clo[axis]) /
                                     (chi[axis] - clo[axis])));
                        buckets[b] = unite(buckets[b], v);
                    }
                    // Pinned PBRT omits bucket-zero split in its minimum scan.
                    for (unsigned split = 1; split < 11; ++split) {
                        LightSamplingInput left, right;
                        for (unsigned k = 0; k <= split; ++k)
                            left = unite(left, buckets[k]);
                        for (unsigned k = split + 1; k < 12; ++k)
                            right = unite(right, buckets[k]);
                        double c = cost(left, bounds, axis) + cost(right, bounds, axis);
                        if (c > 0 && c < best) {
                            best = c;
                            split_axis = axis;
                            split_bucket = split;
                            found = true;
                        }
                    }
                }
            std::size_t mid = begin + (end - begin) / 2;
            if (found) {
                auto it = std::stable_partition(
                    finite.begin() + static_cast<std::ptrdiff_t>(begin),
                    finite.begin() + static_cast<std::ptrdiff_t>(end), [&](auto id) {
                        const auto &v = bvh[id];
                        auto b = std::min(
                            11u, static_cast<unsigned>(
                                     12 *
                                     ((double(v.low[split_axis]) + v.high[split_axis]) / 2 -
                                      clo[split_axis]) /
                                     (chi[split_axis] - clo[split_axis])));
                        return b <= split_bucket;
                    });
                auto candidate = static_cast<std::size_t>(it - finite.begin());
                if (candidate > begin && candidate < end)
                    mid = candidate;
            }
            build(begin, mid, {index, depth + 1});
            node.child = build(mid, end, {index, depth + 1});
        }
        out.nodes[index] = node;
        return index;
    };
    if (!finite.empty())
        build(0, finite.size(), {UINT32_MAX, 0});
    return out;
}
} // namespace rypath
