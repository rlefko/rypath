// SPDX-License-Identifier: GPL-3.0-only
// Portions adapted from PBRT v3, Copyright (c) 1998-2016 Matt Pharr,
// Greg Humphreys, and Wenzel Jakob (BSD-2-Clause; third_party/pbrt-v3/LICENSE.txt).
// Measured mappings follow PBRT v4, Copyright (c) 1998-2020 Matt Pharr,
// Wenzel Jakob, and Greg Humphreys (Apache-2.0; third_party/pbrt/LICENSE.txt).
#pragma once
#include "../src/bsdf_table_wire.h"
// Fourier interpolation follows PBRT v3's nonuniform Catmull-Rom convention;
// measured tensor mappings follow PBRT v4. All evaluations use radiance transport.
struct TableBsdfEvaluation {
    float3 value;
    float pdf;
    bool rgb;
    bool valid;
};
struct TableBsdfSample {
    float3 wi;
    float3 value;
    float pdf;
    bool rgb;
    bool accepted;
    bool valid;
};
inline float tb_f(const device uint *t, uint i) { return as_type<float>(t[i]); }
inline uint tb_interval(const device uint *t, uint off, uint n, float x) {
    uint lo = 0, hi = n - 1;
    while (lo + 1 < hi) {
        uint m = (lo + hi) / 2;
        if (tb_f(t, off + m) <= x)
            lo = m;
        else
            hi = m;
    }
    return min(lo, n - 2);
}
struct TbWeights {
    int offset;
    float4 w;
};
inline TbWeights tb_catmull(const device uint *t, float x) {
    uint n = t[1], p = t[5], i = tb_interval(t, p, n, x);
    float x0 = tb_f(t, p + i), x1 = tb_f(t, p + i + 1), h = x1 - x0;
    float u = clamp((x - x0) / h, 0.f, 1.f), u2 = u * u, u3 = u2 * u;
    TbWeights a;
    a.offset = int(i) - 1;
    a.w = float4(0, 2 * u3 - 3 * u2 + 1, -2 * u3 + 3 * u2, 0);
    float w = u3 - 2 * u2 + u;
    if (i > 0) {
        w *= h / (x1 - tb_f(t, p + i - 1));
        a.w[0] = -w;
        a.w[2] += w;
    } else {
        a.w[1] -= w;
        a.w[2] += w;
    }
    w = u3 - u2;
    if (i + 2 < n) {
        w *= h / (tb_f(t, p + i + 2) - x0);
        a.w[1] -= w;
        a.w[3] = w;
    } else {
        a.w[1] -= w;
        a.w[2] += w;
    }
    return a;
}
inline float tb_coeff(const device uint *t, TbWeights i, TbWeights o, uint channel, uint k) {
    float v = 0;
    for (int b = 0; b < 4; ++b)
        for (int a = 0; a < 4; ++a) {
            float w = i.w[a] * o.w[b];
            if (w == 0)
                continue;
            uint cell = uint(o.offset + b) * t[1] + uint(i.offset + a), p = t[7] + 2 * cell,
                 m = t[p + 1];
            if (k < m)
                v += w * tb_f(t, t[8] + t[p] + channel * m + k);
        }
    return v;
}
inline float tb_fourier(const device uint *t, TbWeights i, TbWeights o, uint c, float phi) {
    float v = 0;
    for (uint k = 0; k < t[2]; ++k)
        v += tb_coeff(t, i, o, c, k) * cos(float(k) * phi);
    return v;
}
inline float tb_row(const device uint *t, TbWeights o, uint i, bool cdf) {
    float v = 0;
    for (int b = 0; b < 4; ++b) {
        if (o.w[b] == 0)
            continue;
        uint cell = uint(o.offset + b) * t[1] + i;
        if (cdf)
            v += o.w[b] * tb_f(t, t[6] + cell);
        else {
            uint p = t[7] + 2 * cell;
            if (t[p + 1])
                v += o.w[b] * tb_f(t, t[8] + t[p]);
        }
    }
    return v;
}
inline TableBsdfEvaluation tb_fourier_eval(const device uint *t, float3 wo, float3 wi) {
    float mi = -wi.z, mo = wo.z;
    TbWeights i = tb_catmull(t, mi), o = tb_catmull(t, mo);
    float den = length(wi.xy) * length(wo.xy);
    float cp = den > 0 ? clamp(dot(-wi.xy, wo.xy) / den, -1.f, 1.f) : 1.f, phi = acos(cp);
    float y = max(0.f, tb_fourier(t, i, o, 0, phi));
    float rho = tb_row(t, o, t[1] - 1, true) * 6.283185307179586f;
    float scale = mi != 0 ? 1.f / abs(mi) : 0.f;
    if (mi * mo > 0) {
        float eta = tb_f(t, 4);
        scale *= mi > 0 ? 1.f / (eta * eta) : eta * eta;
    }
    float3 value(y);
    if (t[3] == 3) {
        float r = tb_fourier(t, i, o, 1, phi), b = tb_fourier(t, i, o, 2, phi);
        value = max(float3(0), float3(r, 1.39829f * y - .100913f * b - .297375f * r, b));
    }
    value *= scale;
    float pdf = rho > 0 ? y / rho : 0;
    return {value, pdf, t[3] == 3, all(isfinite(value)) && isfinite(pdf) && pdf >= 0};
}
inline TableBsdfSample tb_fourier_sample(const device uint *t, float3 wo, float2 u) {
    TbWeights o = tb_catmull(t, wo.z);
    uint n = t[1], nodes = t[5];
    float maximum = tb_row(t, o, n - 1, true);
    if (!(maximum > 0))
        return {float3(0), float3(0), 0, t[3] == 3, false, true};
    float target = clamp(u.y, 0x1p-24f, 1.f - 0x1p-24f) * maximum;
    uint lo = 0, hi = n - 1;
    while (lo + 1 < hi) {
        uint m = (lo + hi) / 2;
        if (tb_row(t, o, m, true) <= target)
            lo = m;
        else
            hi = m;
    }
    uint j = min(lo, n - 2);
    float x0 = tb_f(t, nodes + j), x1 = tb_f(t, nodes + j + 1), h = x1 - x0;
    float f0 = tb_row(t, o, j, false), f1 = tb_row(t, o, j + 1, false);
    float d0 =
        j > 0 ? h * (f1 - tb_row(t, o, j - 1, false)) / (x1 - tb_f(t, nodes + j - 1)) : f1 - f0;
    float d1 =
        j + 2 < n ? h * (tb_row(t, o, j + 2, false) - f0) / (tb_f(t, nodes + j + 2) - x0) : f1 - f0;
    float desired = (target - tb_row(t, o, j, true)) / h, a = 0, b = 1;
    // Normalize the Hermite integral before inversion. An absolute residual on
    // authored coefficients would change the proposal when the BSDF is dimmed.
    float amplitude = max(max(abs(f0), abs(f1)), max(abs(d0), abs(d1)));
    if (!(amplitude > 0) || !isfinite(amplitude))
        return {float3(0), float3(0), 0, t[3] == 3, false, false};
    f0 /= amplitude;
    f1 /= amplitude;
    d0 /= amplitude;
    d1 /= amplitude;
    desired /= amplitude;
    float z = f0 != f1 ? (f0 - sqrt(max(0.f, f0 * f0 + 2 * desired * (f1 - f0)))) / (f0 - f1)
                       : desired / f0;
    for (uint k = 0; k < 40; ++k) {
        if (!(z >= a && z <= b))
            z = .5f * (a + b);
        float F = z * (f0 + z * (.5f * d0 + z * ((-2 * d0 - d1) / 3 + f1 - f0 +
                                                 z * ((d0 + d1) * .25f + (f0 - f1) * .5f))));
        float f =
            f0 + z * (d0 + z * (-2 * d0 - d1 + 3 * (f1 - f0) + z * (d0 + d1 + 2 * (f0 - f1))));
        if (abs(F - desired) < 1e-6f || b - a < 1e-6f)
            break;
        if (F < desired)
            a = z;
        else
            b = z;
        z -= (F - desired) / f;
    }
    float mi = x0 + h * z;
    TbWeights i = tb_catmull(t, mi);
    float a0 = tb_coeff(t, i, o, 0, 0);
    if (!(a0 > 0))
        return {float3(0), float3(0), 0, t[3] == 3, false, true};
    bool flip = u.x >= .5f;
    float q = flip ? 2 * (1 - u.x) : 2 * u.x;
    target = q * 3.141592653589793f;
    a = 0;
    b = 3.141592653589793f;
    float phi = q * 3.141592653589793f;
    for (uint iter = 0; iter < 40 && q > 0 && q < 1; ++iter) {
        float F = phi, f = 1;
        for (uint k = 1; k < t[2]; ++k) {
            float ak = tb_coeff(t, i, o, 0, k) / a0;
            F += ak * sin(float(k) * phi) / float(k);
            f += ak * cos(float(k) * phi);
        }
        F -= target;
        if (F > 0)
            b = phi;
        else
            a = phi;
        if (abs(F) < 1e-6f || b - a < 1e-6f)
            break;
        phi -= F / f;
        if (!(phi > a && phi < b))
            phi = .5f * (a + b);
    }
    if (flip)
        phi = 6.283185307179586f - phi;
    float base = atan2(wo.y, wo.x), s = sqrt(max(0.f, 1 - mi * mi));
    float3 wi = -float3(s * cos(base + phi), s * sin(base + phi), mi);
    TableBsdfEvaluation e = tb_fourier_eval(t, wo, wi);
    return {wi, e.value, e.pdf, e.rgb, e.valid && e.pdf > 0, e.valid};
}
// Bilinear measured distributions store density per grid cell, matching PBRT's
// PiecewiseLinear2D normalization. Additional parameters interpolate complete slices.
struct TbLinear {
    uint i;
    float u;
};
inline TbLinear tb_linear(const device uint *t, uint off, uint n, float x) {
    if (n == 1)
        return {0, 0};
    uint i = tb_interval(t, off, n, x);
    return {i, clamp((x - tb_f(t, off + i)) / (tb_f(t, off + i + 1) - tb_f(t, off + i)), 0.f, 1.f)};
}
inline float tb_vertex(const device uint *t, uint d, uint x, uint y, float phi, float theta,
                       float wave) {
    uint w = t[d], h = t[d + 1], base = t[d + 3];
    if (d == RYPATH_TABLE_NDF || d == RYPATH_TABLE_SIGMA)
        return tb_f(t, base + y * w + x);
    TbLinear p = tb_linear(t, t[5], t[1], phi), q = tb_linear(t, t[6], t[2], theta),
             l = tb_linear(t, t[7], t[3], wave);
    uint nw = d == RYPATH_TABLE_SPECTRA ? t[3] : 1;
    float value = 0;
    for (uint a = 0; a < 2; ++a)
        for (uint b = 0; b < 2; ++b)
            for (uint c = 0; c < (nw > 1 ? 2u : 1u); ++c) {
                float wt = (a ? p.u : 1 - p.u) * (b ? q.u : 1 - q.u) *
                           (nw > 1 ? (c ? l.u : 1 - l.u) : 1.f);
                if (wt == 0)
                    continue;
                uint slice = ((p.i + a) * t[2] + q.i + b) * nw + (nw > 1 ? l.i + c : 0);
                value += wt * tb_f(t, base + slice * w * h + y * w + x);
            }
    return value;
}
inline float tb_density(const device uint *t, uint d, float2 p, float phi, float theta,
                        float wave) {
    uint w = t[d], h = t[d + 1];
    float2 z = clamp(p, 0.f, 1.f) * float2(w - 1, h - 1);
    uint x = min(uint(z.x), w - 2), y = min(uint(z.y), h - 2);
    z -= float2(x, y);
    float v = mix(mix(tb_vertex(t, d, x, y, phi, theta, wave),
                      tb_vertex(t, d, x + 1, y, phi, theta, wave), z.x),
                  mix(tb_vertex(t, d, x, y + 1, phi, theta, wave),
                      tb_vertex(t, d, x + 1, y + 1, phi, theta, wave), z.x),
                  z.y);
    return v * float((w - 1) * (h - 1));
}
// Cache lookups interpolate the same normalized slices as tb_vertex. Integration
// is linear in those parameter weights, so cumulative rows preserve the model.
inline float tb_cumulative(const device uint *t, uint d, uint x, uint y, float phi, float theta,
                           bool marginal) {
    uint slot =
        d == RYPATH_TABLE_VNDF ? RYPATH_TABLE_VNDF_CONDITIONAL : RYPATH_TABLE_LUMINANCE_CONDITIONAL;
    uint width = marginal ? 1 : t[d], height = t[d + 1], base = t[slot + (marginal ? 1 : 0)];
    TbLinear p = tb_linear(t, t[5], t[1], phi), q = tb_linear(t, t[6], t[2], theta);
    float value = 0;
    for (uint a = 0; a < 2; ++a)
        for (uint b = 0; b < 2; ++b) {
            float weight = (a ? p.u : 1 - p.u) * (b ? q.u : 1 - q.u);
            if (weight == 0)
                continue;
            uint slice = (p.i + a) * t[2] + q.i + b;
            value +=
                weight * tb_f(t, base + slice * width * height + y * width + (marginal ? 0 : x));
        }
    return value;
}
inline float tb_row_mass(const device uint *t, uint d, uint y, float phi, float theta) {
    return tb_cumulative(t, d, t[d] - 1, y, phi, theta, false);
}
inline float tb_conditional(const device uint *t, uint d, uint x, uint y, float z, float phi,
                            float theta) {
    return mix(tb_cumulative(t, d, x, y, phi, theta, false),
               tb_cumulative(t, d, x, y + 1, phi, theta, false), z);
}
inline float tb_linear_inverse(float mass, float v0, float v1) {
    if (mass <= 0)
        return 0;
    if (abs(v1 - v0) < 1e-4f * (v0 + v1))
        return 2 * mass / (v0 + v1);
    return 2 * mass / (v0 + sqrt(max(0.f, v0 * v0 + 2 * mass * (v1 - v0))));
}
struct TbWarp {
    float2 p;
    float pdf;
    bool valid;
};
inline TbWarp tb_warp(const device uint *t, uint d, float2 u, float phi, float theta,
                      bool inverse) {
    uint w = t[d], h = t[d + 1];
    float2 pos;
    uint x, y;
    float2 z;
    float r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    if (inverse) {
        pos = clamp(u, 0.f, 1.f) * float2(w - 1, h - 1);
        x = min(uint(pos.x), w - 2);
        y = min(uint(pos.y), h - 2);
        z = pos - float2(x, y);
        r0 = tb_row_mass(t, d, y, phi, theta);
        r1 = tb_row_mass(t, d, y + 1, phi, theta);
    } else {
        u = clamp(u, 0x1p-24f, 1.f - 0x1p-24f);
        uint low = 0, high = h - 1;
        while (low + 1 < high) {
            uint middle = (low + high) / 2;
            if (tb_cumulative(t, d, 0, middle, phi, theta, true) < u.y)
                low = middle;
            else
                high = middle;
        }
        y = min(low, h - 2);
        r0 = tb_row_mass(t, d, y, phi, theta);
        r1 = tb_row_mass(t, d, y + 1, phi, theta);
        float mass = u.y - tb_cumulative(t, d, 0, y, phi, theta, true);
        if (!(r0 + r1 > 0))
            return {float2(0), 0, true};
        z.y = clamp(tb_linear_inverse(mass, r0, r1), 0.f, 1.f);
        float target = u.x * mix(r0, r1, z.y);
        low = 0;
        high = w - 1;
        while (low + 1 < high) {
            uint middle = (low + high) / 2;
            if (tb_conditional(t, d, middle, y, z.y, phi, theta) < target)
                low = middle;
            else
                high = middle;
        }
        x = min(low, w - 2);
        target -= tb_conditional(t, d, x, y, z.y, phi, theta);
        c0 = mix(tb_vertex(t, d, x, y, phi, theta, 0), tb_vertex(t, d, x, y + 1, phi, theta, 0),
                 z.y);
        c1 = mix(tb_vertex(t, d, x + 1, y, phi, theta, 0),
                 tb_vertex(t, d, x + 1, y + 1, phi, theta, 0), z.y);
        if (!(c0 + c1 > 0))
            return {float2(0), 0, true};
        z.x = clamp(tb_linear_inverse(target, c0, c1), 0.f, 1.f);
    }
    c0 = mix(tb_vertex(t, d, x, y, phi, theta, 0), tb_vertex(t, d, x, y + 1, phi, theta, 0), z.y);
    c1 = mix(tb_vertex(t, d, x + 1, y, phi, theta, 0), tb_vertex(t, d, x + 1, y + 1, phi, theta, 0),
             z.y);
    float pdf = mix(c0, c1, z.x) * float((w - 1) * (h - 1));
    if (!inverse)
        return {(float2(x, y) + z) / float2(w - 1, h - 1), pdf, isfinite(pdf) && pdf >= 0};
    float sx = z.x * (c0 + .5f * z.x * (c1 - c0)) + tb_conditional(t, d, x, y, z.y, phi, theta);
    float sy = z.y * (r0 + .5f * z.y * (r1 - r0)) + tb_cumulative(t, d, 0, y, phi, theta, true);
    float rowmass = mix(r0, r1, z.y);
    return {float2(rowmass > 0 ? sx / rowmass : 0, sy), pdf, isfinite(pdf) && pdf >= 0};
}
inline float2 tb_square(float3 v) {
    return float2(sqrt(atan2(length(v.xy), v.z) / 1.5707963267948966f),
                  (atan2(v.y, v.x) + 3.141592653589793f) / 6.283185307179586f);
}
inline TableBsdfEvaluation tb_measured_eval(const device uint *t, float3 wo, float3 wi,
                                            float wave) {
    if (wo.z * wi.z <= 0)
        return {float3(0), 0, false, true};
    if (wo.z < 0) {
        wo = -wo;
        wi = -wi;
    }
    float3 wm = normalize(wo + wi);
    float phi = atan2(wo.y, wo.x), theta = acos(clamp(wo.z, -1.f, 1.f));
    float2 p = tb_square(wm);
    if (t[4])
        p.y -= phi / 6.283185307179586f;
    p.y -= floor(p.y);
    TbWarp inv = tb_warp(t, RYPATH_TABLE_VNDF, p, phi, theta, true);
    float spec = max(0.f, tb_density(t, RYPATH_TABLE_SPECTRA, inv.p, phi, theta, wave));
    float sigma = tb_density(t, RYPATH_TABLE_SIGMA, tb_square(wo), 0, 0, 0),
          ndf = tb_density(t, RYPATH_TABLE_NDF, p, 0, 0, 0);
    float value = sigma > 0 ? spec * ndf / (4 * sigma * wi.z) : 0;
    float jac = 4 * dot(wo, wm) *
                max(2 * 3.141592653589793f * 3.141592653589793f * p.x * length(wm.xy), 1e-6f);
    float pdf = inv.pdf * tb_density(t, RYPATH_TABLE_LUMINANCE, inv.p, phi, theta, 0) / jac;
    return {float3(value), pdf, false,
            inv.valid && sigma > 0 && isfinite(value) && isfinite(pdf) && pdf >= 0};
}
inline TableBsdfSample tb_measured_sample(const device uint *t, float3 wo, float2 u, float wave) {
    bool flip = wo.z <= 0;
    if (flip)
        wo = -wo;
    float phi = atan2(wo.y, wo.x), theta = acos(clamp(wo.z, -1.f, 1.f));
    TbWarp lum = tb_warp(t, RYPATH_TABLE_LUMINANCE, u, phi, theta, false);
    if (lum.valid && lum.pdf == 0)
        return {float3(0), float3(0), 0, false, false, true};
    TbWarp normal = tb_warp(t, RYPATH_TABLE_VNDF, lum.p, phi, theta, false);
    if (normal.valid && normal.pdf == 0)
        return {float3(0), float3(0), 0, false, false, true};
    if (!lum.valid || !normal.valid)
        return {float3(0), float3(0), 0, false, false, false};
    float tm = normal.p.x * normal.p.x * 1.5707963267948966f,
          pm = normal.p.y * 6.283185307179586f - 3.141592653589793f;
    if (t[4])
        pm += phi;
    float3 wm = float3(sin(tm) * cos(pm), sin(tm) * sin(pm), cos(tm)),
           wi = 2 * dot(wo, wm) * wm - wo;
    if (wi.z <= 0)
        return {float3(0), float3(0), 0, false, false, true};
    TableBsdfEvaluation e = tb_measured_eval(t, wo, wi, wave);
    if (flip)
        wi = -wi;
    return {wi, e.value, e.pdf, false, e.valid && e.pdf > 0, e.valid};
}
inline TableBsdfEvaluation table_bsdf_evaluate(const device uint *t, float3 wo, float3 wi,
                                               float wavelength) {
    if (t[0] == RYPATH_TABLE_FOURIER)
        return tb_fourier_eval(t, wo, wi);
    if (t[0] == RYPATH_TABLE_MEASURED)
        return tb_measured_eval(t, wo, wi, wavelength);
    return {float3(0), 0, false, false};
}
inline TableBsdfSample table_bsdf_sample(const device uint *t, float3 wo, float2 u,
                                         float wavelength) {
    if (t[0] == RYPATH_TABLE_FOURIER)
        return tb_fourier_sample(t, wo, u);
    if (t[0] == RYPATH_TABLE_MEASURED)
        return tb_measured_sample(t, wo, u, wavelength);
    return {float3(0), float3(0), 0, false, false, false};
}
