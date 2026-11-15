// SPDX-License-Identifier: Apache-2.0
// Texture mappings, checkerboard, dots, marble, and noise adapted from pinned
// pbrt-v4 (b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// License text: third_party/pbrt/LICENSE.txt.

#pragma once

#include "../src/texture_graph_wire.h"

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#define RYPATH_GRAPH_DEVICE const device
#define RYPATH_GRAPH_THREAD thread
#define RYPATH_GRAPH_TABLE constant
#define RYPATH_GRAPH_MATH metal
#else
#include <algorithm>
#include <cmath>
#define RYPATH_GRAPH_DEVICE const
#define RYPATH_GRAPH_THREAD
#define RYPATH_GRAPH_TABLE constexpr
#define RYPATH_GRAPH_MATH std
#endif

namespace rypath {
namespace texture_graph {

namespace wire = texture_graph_wire;
using wire::Context;
using wire::Mapped2D;
using wire::Mapping;
using wire::Node;
using wire::Point;
using wire::Word;

using RYPATH_GRAPH_MATH::abs;
using RYPATH_GRAPH_MATH::acos;
using RYPATH_GRAPH_MATH::atan2;
using RYPATH_GRAPH_MATH::clamp;
using RYPATH_GRAPH_MATH::copysign;
using RYPATH_GRAPH_MATH::exp;
using RYPATH_GRAPH_MATH::floor;
using RYPATH_GRAPH_MATH::fma;
using RYPATH_GRAPH_MATH::fmod;
using RYPATH_GRAPH_MATH::isfinite;
using RYPATH_GRAPH_MATH::log;
using RYPATH_GRAPH_MATH::max;
using RYPATH_GRAPH_MATH::min;
using RYPATH_GRAPH_MATH::sin;
using RYPATH_GRAPH_MATH::sqrt;

inline float lerp(float t, float a, float b) { return (1.0f - t) * a + t * b; }

inline float dot(Point a, Point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Point scaled(Point point, float factor) {
    return {point.x * factor, point.y * factor, point.z * factor};
}

inline bool finite(Point point) {
    return isfinite(point.x) && isfinite(point.y) && isfinite(point.z);
}

inline bool finite(Mapped2D mapped) {
    return isfinite(mapped.s) && isfinite(mapped.t) && isfinite(mapped.dsdx) &&
           isfinite(mapped.dtdx) && isfinite(mapped.dsdy) && isfinite(mapped.dtdy);
}

inline Point transformed(RYPATH_GRAPH_DEVICE Mapping &mapping, Point p, bool position) {
    const auto m = mapping.texture_from_render;
    const Point r{m[0] * p.x + m[1] * p.y + m[2] * p.z, m[4] * p.x + m[5] * p.y + m[6] * p.z,
                  m[8] * p.x + m[9] * p.y + m[10] * p.z};
    return position ? Point{r.x + m[3], r.y + m[7], r.z + m[11]} : r;
}

struct Mapped3D {
    Point p;
    Point dx;
    Point dy;
};

inline Mapped3D map3d(RYPATH_GRAPH_DEVICE Mapping &mapping,
                      const RYPATH_GRAPH_THREAD Context &ctx) {
    return {transformed(mapping, ctx.p, true), transformed(mapping, ctx.dpdx, false),
            transformed(mapping, ctx.dpdy, false)};
}

inline bool map2d(RYPATH_GRAPH_DEVICE Mapping &mapping, const RYPATH_GRAPH_THREAD Context &ctx,
                  RYPATH_GRAPH_THREAD Mapped2D &mapped, bool footprint = true) {
    const auto values = mapping.values;
    if (mapping.kind == wire::uv) {
        mapped = {values[0] * ctx.u + values[2], values[1] * ctx.v + values[3], 0, 0, 0, 0};
        if (footprint) {
            mapped.dsdx = values[0] * ctx.dudx;
            mapped.dtdx = values[1] * ctx.dvdx;
            mapped.dsdy = values[0] * ctx.dudy;
            mapped.dtdy = values[1] * ctx.dvdy;
        }
        return finite(mapped);
    }

    const auto c =
        footprint ? map3d(mapping, ctx) : Mapped3D{transformed(mapping, ctx.p, true), {}, {}};
    if (mapping.kind == wire::planar) {
        const Point vs{values[0], values[1], values[2]};
        const Point vt{values[3], values[4], values[5]};
        mapped = {values[6] + dot(c.p, vs),
                  values[7] + dot(c.p, vt),
                  dot(vs, c.dx),
                  dot(vt, c.dx),
                  dot(vs, c.dy),
                  dot(vt, c.dy)};
        return finite(mapped);
    }

    constexpr float pi = 3.14159265358979323846f;
    const Point p = c.p;
    const float xy2 = p.x * p.x + p.y * p.y;
    if (!finite(p) || (footprint && !(xy2 > 0.0f && isfinite(xy2)))) {
        return false;
    }
    const float azimuth = atan2(p.y, p.x);
    Point dt{0, 0, 1};
    if (mapping.kind == wire::cylindrical) {
        mapped = {(pi + azimuth) * (1.0f / (2.0f * pi)), p.z, 0, 0, 0, 0};
    } else if (mapping.kind == wire::spherical) {
        const float length2 = xy2 + p.z * p.z;
        if (!(length2 > 0.0f && isfinite(length2))) {
            return false;
        }
        const float phi = azimuth < 0.0f ? azimuth + 2.0f * pi : azimuth;
        mapped = {acos(clamp(p.z / sqrt(length2), -1.0f, 1.0f)) * (1.0f / pi),
                  phi * (1.0f / (2.0f * pi)),
                  0,
                  0,
                  0,
                  0};
        if (footprint) {
            const float xy = sqrt(xy2);
            dt = scaled({p.x * p.z / xy, p.y * p.z / xy, -xy}, 1.0f / (pi * length2));
        }
    } else {
        return false;
    }

    // Pinned PBRT returns (theta, phi) but assigns the phi derivatives to s.
    if (footprint) {
        const Point ds = scaled({-p.y, p.x, 0}, 1.0f / (2.0f * pi * xy2));
        mapped.dsdx = dot(ds, c.dx);
        mapped.dtdx = dot(dt, c.dx);
        mapped.dsdy = dot(ds, c.dy);
        mapped.dtdy = dot(dt, c.dy);
    }
    return finite(mapped);
}

// Periodic part of the square wave's second antiderivative. The omitted
// linear term cancels in a centered second difference.
inline float checker_periodic_integral(float phase) {
    const float half_phase = phase * 0.5f;
    const float y = half_phase - floor(half_phase) - 0.5f;
    return y * (1.0f - 2.0f * abs(y));
}

inline float checker_filter(float x, float radius) {
    const float cell = floor(x);
    const float parity = abs(fmod(cell, 2.0f)) == 0.0f ? 1.0f : -1.0f;
    if (radius == 0)
        return parity;
    if (radius <= 1.0f) {
        // Integrate the tent's triangular caps beyond the adjacent boundaries.
        // This avoids subtracting nearly equal antiderivatives for small radii.
        const float fraction = x - cell;
        // Subtract the nearest boundary directly, retaining tiny distances
        // near zero on the negative side rather than rounding 1-fraction.
        const float upper_distance = fraction > 0.5f ? (cell + 1.0f) - x : 1.0f - fraction;
        const float left = max(0.0f, 1.0f - fraction / radius);
        const float right = max(0.0f, 1.0f - upper_distance / radius);
        return parity * (1.0f - left * left - right * right);
    }
    // Keep phase before adding a broad footprint, and avoid radius-squared
    // overflow. Input coordinates and derivatives must still be finite.
    const float phase = fmod(x, 2.0f);
    const float radius_phase = fmod(radius, 2.0f);
    return ((checker_periodic_integral(phase + radius_phase) -
             2.0f * checker_periodic_integral(phase) +
             checker_periodic_integral(phase - radius_phase)) /
            radius) /
           radius;
}

inline float checked_axis(float x, float dx, float dy) {
    return checker_filter(x, 1.5f * max(abs(dx), abs(dy)));
}

inline bool checker_weight(RYPATH_GRAPH_DEVICE Mapping &mapping,
                           const RYPATH_GRAPH_THREAD Context &ctx,
                           RYPATH_GRAPH_THREAD float &weight) {
    if (mapping.kind == wire::spatial) {
        const auto c = map3d(mapping, ctx);
        if (!(finite(c.p) && finite(c.dx) && finite(c.dy))) {
            return false;
        }
        weight = 0.5f - 0.5f * checked_axis(c.p.x, c.dx.x, c.dy.x) *
                            checked_axis(c.p.y, c.dx.y, c.dy.y) *
                            checked_axis(c.p.z, c.dx.z, c.dy.z);
    } else {
        Mapped2D c;
        if (!map2d(mapping, ctx, c)) {
            return false;
        }
        weight = 0.5f - checked_axis(c.s, c.dsdx, c.dsdy) * checked_axis(c.t, c.dtdx, c.dtdy) / 2;
    }
    return isfinite(weight);
}

// The Perlin permutation from PBRT util/noise.cpp.
static RYPATH_GRAPH_TABLE unsigned char noise_permutation[256] = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103,
    30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,
    197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174,
    20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231,
    83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143,
    54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196,
    135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124,
    123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,
    182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101,
    155, 167, 43,  172, 9,   129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185,
    112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,
    51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176,
    115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243,
    141, 128, 195, 78,  66,  215, 61,  156, 180,
};

inline int permute(int index) { return noise_permutation[index & 255]; }

inline float gradient(int x, int y, int z, float dx, float dy, float dz) {
    const int hash = permute(permute(permute(x) + y) + z) & 15;
    const float u = hash < 8 || hash == 12 || hash == 13 ? dx : dy;
    const float v = hash < 4 || hash == 12 || hash == 13 ? dy : dz;
    return (hash & 1 ? -u : u) + (hash & 2 ? -v : v);
}

inline float noise_weight(float t) {
    const float t2 = t * t;
    return 6.0f * (t * (t2 * t2)) - 15.0f * (t2 * t2) + 10.0f * (t * t2);
}

inline float noise(Point p) {
    if (!finite(p)) {
        return NAN;
    }
    constexpr float limit = float(1 << 30);
    p = {fmod(p.x, limit), fmod(p.y, limit), fmod(p.z, limit)};
    const int ix = int(floor(p.x)), iy = int(floor(p.y)), iz = int(floor(p.z));
    const float dx = p.x - float(ix), dy = p.y - float(iy), dz = p.z - float(iz);
    const float wx = noise_weight(dx), wy = noise_weight(dy), wz = noise_weight(dz);
    const float x00 =
        lerp(wx, gradient(ix, iy, iz, dx, dy, dz), gradient(ix + 1, iy, iz, dx - 1, dy, dz));
    const float x10 = lerp(wx, gradient(ix, iy + 1, iz, dx, dy - 1, dz),
                           gradient(ix + 1, iy + 1, iz, dx - 1, dy - 1, dz));
    const float x01 = lerp(wx, gradient(ix, iy, iz + 1, dx, dy, dz - 1),
                           gradient(ix + 1, iy, iz + 1, dx - 1, dy, dz - 1));
    const float x11 = lerp(wx, gradient(ix, iy + 1, iz + 1, dx, dy - 1, dz - 1),
                           gradient(ix + 1, iy + 1, iz + 1, dx - 1, dy - 1, dz - 1));
    return lerp(wz, lerp(wy, x00, x10), lerp(wy, x01, x11));
}

inline bool inside_dot(Mapped2D c) {
    const float s_cell = floor(c.s + 0.5f), t_cell = floor(c.t + 0.5f);
    if (!(noise({s_cell + 0.5f, t_cell + 0.5f, 0.5f}) > 0.0f)) {
        return false;
    }
    constexpr float radius = 0.35f;
    constexpr float max_shift = 0.5f - radius;
    const float s_center = s_cell + max_shift * noise({s_cell + 1.5f, t_cell + 2.8f, 0.5f});
    const float t_center = t_cell + max_shift * noise({s_cell + 4.5f, t_cell + 9.8f, 0.5f});
    const float ds = c.s - s_center, dt = c.t - t_center;
    return ds * ds + dt * dt < radius * radius;
}

inline float partial_octave(float fraction) {
    const float t = clamp((fraction - 0.3f) / (0.7f - 0.3f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float fractal(Mapped3D c, float roughness, Word octaves, bool turbulence = false) {
    if (!(finite(c.p) && finite(c.dx) && finite(c.dy)) || octaves > wire::max_octaves) {
        return NAN;
    }
    const float length2 = max(dot(c.dx, c.dx), dot(c.dy, c.dy));
    constexpr float inverse_log2 = 1.4426950408889634f;
    const float n = clamp(-1.0f - log(length2) * inverse_log2 / 2.0f, 0.0f, float(octaves));
    const Word whole = Word(floor(n));
    float sum = 0.0f, frequency = 1.0f, amplitude = 1.0f;
    for (Word octave = 0; octave < whole; ++octave) {
        const float value = noise(scaled(c.p, frequency));
        sum += amplitude * (turbulence ? abs(value) : value);
        frequency *= 1.99f;
        amplitude *= roughness;
    }
    const float partial = partial_octave(n - float(whole));
    const float value = noise(scaled(c.p, frequency));
    if (!turbulence) {
        return sum + amplitude * partial * value;
    }
    sum += amplitude * lerp(partial, 0.2f, abs(value));
    // Match PBRT's unresolved octave bias, including the partial octave index.
    for (Word octave = whole; octave < octaves; ++octave) {
        sum += amplitude * 0.2f;
        amplitude *= roughness;
    }
    return sum;
}

inline Mapped3D scaled(Mapped3D c, float factor) {
    return {scaled(c.p, factor), scaled(c.dx, factor), scaled(c.dy, factor)};
}

static RYPATH_GRAPH_TABLE Point marble_colors[9] = {
    {0.58f, 0.58f, 0.6f}, {0.58f, 0.58f, 0.6f}, {0.58f, 0.58f, 0.6f},
    {0.5f, 0.5f, 0.5f},   {0.6f, 0.59f, 0.58f}, {0.58f, 0.58f, 0.6f},
    {0.58f, 0.58f, 0.6f}, {0.2f, 0.2f, 0.33f},  {0.58f, 0.58f, 0.6f},
};

inline Point lerp(float t, Point a, Point b) {
    return {lerp(t, a.x, b.x), lerp(t, a.y, b.y), lerp(t, a.z, b.z)};
}

inline Point marble_rgb(float phase) {
    const float t = (0.5f + 0.5f * sin(phase)) * 6.0f;
    const int first = min(int(floor(t)), 5);
    const float segment = t - float(first);
    const Point a = lerp(segment, marble_colors[first], marble_colors[first + 1]);
    const Point b = lerp(segment, marble_colors[first + 1], marble_colors[first + 2]);
    const Point c = lerp(segment, marble_colors[first + 2], marble_colors[first + 3]);
    return scaled(lerp(segment, lerp(segment, a, b), lerp(segment, b, c)), 1.5f);
}

inline Word child_count(Word kind) {
    switch (kind) {
    case wire::scale:
    case wire::directionmix:
    case wire::checkerboard:
    case wire::dots:
        return 2;
    case wire::mix:
        return 3;
    case wire::bilerp:
        return 4;
    default:
        return 0;
    }
}

inline Word child_type(RYPATH_GRAPH_DEVICE Node &node, Word slot) {
    if ((node.kind == wire::scale && slot == 1) || (node.kind == wire::mix && slot == 2)) {
        return wire::scalar;
    }
    return node.type;
}

inline float sampled_value(RYPATH_GRAPH_DEVICE render_wire::SpectralKnot *knots, Word count,
                           Word begin, Word length, float wavelength) {
    if (begin > count || length == 0 || length > count - begin) {
        return NAN;
    }
    Word left = begin, right = begin + length - 1;
    if (wavelength < knots[left].wavelength || wavelength > knots[right].wavelength) {
        return 0.0f;
    }
    while (right - left > 1) {
        const Word middle = left + (right - left) / 2;
        if (knots[middle].wavelength <= wavelength) {
            left = middle;
        } else {
            right = middle;
        }
    }
    const float a = knots[left].wavelength, b = knots[right].wavelength;
    if (a == wavelength) {
        return knots[left].value;
    }
    if (b == wavelength) {
        return knots[right].value;
    }
    const float width = b - a;
    const float t = isfinite(width) ? (wavelength - a) / width
                                    : (0.5f * wavelength - 0.5f * a) / (0.5f * b - 0.5f * a);
    return lerp(t, knots[left].value, knots[right].value);
}

inline float sigmoid_spectrum(float a, float b, float c, float wavelength) {
    const float p = fma(fma(a, wavelength, b), wavelength, c);
    if (!isfinite(p)) {
        return NAN;
    }
    if (abs(p) <= 1.0f) {
        return 0.5f + 0.5f * (p / sqrt(1.0f + p * p));
    }
    const float reciprocal = 1.0f / p;
    return 0.5f + 0.5f * copysign(1.0f / sqrt(1.0f + reciprocal * reciprocal), p);
}

inline float blackbody_spectrum(float temperature, float wavelength) {
    constexpr float c2 = 14'387'770.0f;
    if (!(temperature > 0.0f && isfinite(temperature))) {
        return NAN;
    }
    if (wavelength < 360.0f || wavelength > 830.0f || temperature < c2 / (128.0f * wavelength)) {
        return 0.0f;
    }
    const float x = (c2 / temperature) / wavelength;
    constexpr float x_peak = 4.965114268247028f;
    constexpr float log_peak_factor = -0.007001607544495994f;
    // Approximate -expm1(-x) around zero, where direct subtraction loses precision.
    const float factor =
        x < 0.0625f
            ? x * (1.0f + x * (-0.5f + x * (1.0f / 6.0f + x * (-1.0f / 24.0f + x / 120.0f))))
            : 1.0f - exp(-x);
    const float log_ratio = 5.0f * log(x / x_peak) + x_peak - x + log_peak_factor - log(factor);
    return min(exp(log_ratio), 1.0f);
}

template <typename ImageSampler, typename RgbSampler>
inline bool
evaluate_leaf(RYPATH_GRAPH_DEVICE Node &node, RYPATH_GRAPH_DEVICE render_wire::SpectralKnot *knots,
              Word knot_count, const RYPATH_GRAPH_THREAD Context &ctx, float wavelength,
              const RYPATH_GRAPH_THREAD ImageSampler &images,
              const RYPATH_GRAPH_THREAD RgbSampler &rgb, RYPATH_GRAPH_THREAD float &value) {
    switch (node.kind) {
    case wire::constant_node:
        value = node.values[0];
        break;
    case wire::knots:
        value = sampled_value(knots, knot_count, node.payload[0], node.payload[1], wavelength);
        break;
    case wire::rgb_sigmoid:
        value = sigmoid_spectrum(node.values[0], node.values[1], node.values[2], wavelength);
        break;
    case wire::blackbody:
        value = blackbody_spectrum(node.values[0], wavelength);
        break;
    case wire::image: {
        Mapped2D c;
        if (!map2d(node.mapping, ctx, c) || !images(node.payload[0], node.type == wire::spectrum,
                                                    node.payload[1] != 0, c, wavelength, value)) {
            return false;
        }
        break;
    }
    case wire::fbm:
    case wire::wrinkled:
        value = fractal(map3d(node.mapping, ctx), node.values[0], node.payload[0],
                        node.kind == wire::wrinkled);
        break;
    case wire::windy: {
        const auto c = map3d(node.mapping, ctx);
        value = abs(fractal(scaled(c, 0.1f), 0.5f, 3)) * fractal(c, 0.5f, 6);
        break;
    }
    case wire::marble: {
        const auto c = scaled(map3d(node.mapping, ctx), node.values[1]);
        const float phase = c.p.y + node.values[2] * fractal(c, node.values[0], node.payload[0]);
        if (!isfinite(phase)) {
            return false;
        }
        const Point color = marble_rgb(phase);
        if (!rgb(color.x, color.y, color.z, wavelength, node.payload[1] != 0, value)) {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    return isfinite(value);
}

struct Frame {
    Word index;
    Word next;
    Word parent_slot;
    float inputs[4];
    float weights[2];
};

inline bool prepare_branches(RYPATH_GRAPH_DEVICE Node &node, const RYPATH_GRAPH_THREAD Context &ctx,
                             RYPATH_GRAPH_THREAD Frame &frame) {
    switch (node.kind) {
    case wire::directionmix:
        frame.weights[0] = abs(dot(ctx.n, {node.values[0], node.values[1], node.values[2]}));
        frame.weights[1] = 1.0f - frame.weights[0];
        return isfinite(frame.weights[0]);
    case wire::checkerboard:
        if (!checker_weight(node.mapping, ctx, frame.weights[1])) {
            return false;
        }
        frame.weights[0] = 1.0f - frame.weights[1];
        break;
    case wire::dots:
    case wire::bilerp: {
        Mapped2D c;
        // Both PBRT nodes use only st. At an angular pole the coordinate can
        // be defined even when its azimuth derivatives are singular.
        if (!map2d(node.mapping, ctx, c, false)) {
            return false;
        }
        if (node.kind == wire::dots) {
            frame.weights[1] = inside_dot(c) ? 1.0f : 0.0f;
            frame.weights[0] = 1.0f - frame.weights[1];
        } else {
            frame.weights[0] = c.s;
            frame.weights[1] = c.t;
        }
        break;
    }
    }
    return true;
}

inline Word next_branch(Word kind, RYPATH_GRAPH_THREAD Frame &frame) {
    const Word next = frame.next++;
    switch (kind) {
    case wire::scale:
        return next == 0 ? 1 : (frame.inputs[1] == 0.0f ? wire::no_texture : 0);
    case wire::mix:
        if (next == 0) {
            return 2;
        }
        if (next == 1) {
            frame.weights[0] = 1.0f - frame.inputs[2];
            frame.weights[1] = frame.inputs[2];
        }
        [[fallthrough]];
    case wire::directionmix:
    case wire::checkerboard:
    case wire::dots: {
        const Word slot = kind == wire::mix ? next - 1 : next;
        return frame.weights[slot] == 0.0f ? wire::no_texture : slot;
    }
    case wire::bilerp:
        // PBRT evaluates the corners v00, v10, v01, v11.
        return ((next & 1u) << 1u) | (next >> 1u);
    default:
        return wire::no_texture;
    }
}

inline float blend(Word kind, const RYPATH_GRAPH_THREAD Frame &frame) {
    const auto inputs = frame.inputs;
    if (kind == wire::scale) {
        return inputs[0] * inputs[1];
    }
    if (kind == wire::dots) {
        return inputs[frame.weights[1] == 1.0f ? 1 : 0];
    }
    if (kind == wire::bilerp) {
        const float s = frame.weights[0], t = frame.weights[1];
        return (1.0f - s) * (1.0f - t) * inputs[0] + s * (1.0f - t) * inputs[2] +
               (1.0f - s) * t * inputs[1] + s * t * inputs[3];
    }
    return frame.weights[0] * inputs[0] + frame.weights[1] * inputs[1];
}

// Both backends run the same bounded depth-first traversal. Frame state retains
// branch results so children can be selected only after their controls evaluate.
template <typename ImageSampler, typename RgbSampler>
inline bool evaluate(RYPATH_GRAPH_DEVICE Node *nodes, Word node_count,
                     RYPATH_GRAPH_DEVICE render_wire::SpectralKnot *knots, Word knot_count,
                     Word root, const RYPATH_GRAPH_THREAD Context &ctx, float wavelength,
                     const RYPATH_GRAPH_THREAD ImageSampler &images,
                     const RYPATH_GRAPH_THREAD RgbSampler &rgb, RYPATH_GRAPH_THREAD float &value) {
    if (root >= node_count || !isfinite(wavelength)) {
        return false;
    }
    Frame stack[wire::max_depth];
    stack[0] = {root, 0, 0, {}, {}};
    Word depth = 1, visits = 0;
    while (depth != 0) {
        RYPATH_GRAPH_THREAD auto &frame = stack[depth - 1];
        RYPATH_GRAPH_DEVICE auto &node = nodes[frame.index];
        const Word children = child_count(node.kind);
        if (frame.next == 0) {
            if (++visits > wire::max_visits || node.type > wire::spectrum ||
                !prepare_branches(node, ctx, frame)) {
                return false;
            }
        }

        float result = 0.0f;
        if (children == 0) {
            if (!evaluate_leaf(node, knots, knot_count, ctx, wavelength, images, rgb, result)) {
                return false;
            }
        } else if (frame.next < children) {
            const Word slot = next_branch(node.kind, frame);
            if (slot != wire::no_texture) {
                const Word child = node.children[slot];
                if (child >= node_count || depth == wire::max_depth ||
                    nodes[child].type != child_type(node, slot)) {
                    return false;
                }
                stack[depth++] = {child, 0, slot, {}, {}};
            }
            continue;
        } else {
            result = blend(node.kind, frame);
            if (!isfinite(result)) {
                return false;
            }
        }
        --depth;
        if (depth == 0) {
            value = result;
            return true;
        }
        stack[depth - 1].inputs[frame.parent_slot] = result;
    }
    return false;
}

} // namespace texture_graph
} // namespace rypath

#undef RYPATH_GRAPH_DEVICE
#undef RYPATH_GRAPH_THREAD
#undef RYPATH_GRAPH_TABLE
#undef RYPATH_GRAPH_MATH
