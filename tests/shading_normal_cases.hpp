// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "shading_normal_probe_wire.h"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace shading_normal_test {
using Query = rypath::shading_normal_probe::Query;
inline std::vector<Query> queries() {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Query> q{
        {.6f, .8f, .6f, .8f, .7f, .9f},
        {-.6f, .8f, -.6f, .8f, 4.f / 9, 9.f / 4},
        {.6f, -.8f, .6f, -.8f, 9.f / 4, 4.f / 9},
        {.4f, .7f, .8f, .3f, .2f, .9f},
        {-.4f, -.7f, .8f, -.3f, .2f, .9f},
        {.4f, .7f, .8f, 0, 1, 1},
        {.4f, .7f, 0, .3f, 1, 1},
        {.4f, 0, .8f, .3f, 1, 1},
        {0, .7f, .8f, .3f, 1, 1},
        {1, 1, 0x1p-100f, 1, 0x1p-100f, .5f},
        {1, 0x1p-100f, 0x1p-100f, 1, 0x1p-120f, 0x1p-120f},
        {1, 0x1p-100f, 0x1p-100f, 1, 0x1p-120f, 0x1p-100f},
        {0x1p-100f, 1, 1, 0x1p-100f, 0x1p100f, 0x1p120f},
        {0x1p-75f, 1, 1, 0x1p-75f, 0x1p100f, 0x1p100f},
        {1, 0x1p-75f, 0x1p-75f, 1, 0x1p-100f, 0x1p-100f},
        {inf, 1, 1, 1, 1, 1},
        {1, nan, 1, 1, 1, 1},
        {1, 1, 1, 1, -1, 1},
        {1, 1, 1, 1, 1, -1},
        {1, 1, 1, 1, inf, 1},
        {1, 1, 1, 1, 1, nan},
        {1, 0x1p-100f, 0x1p-40f, 1, .31830988618379067154f, 1},
        {.4f, .7f, .8f, -0.0f, 1, 1},
        {.4f, -0.0f, .8f, .3f, 1, 1},
        {nan, 0, 0, 0, 0, 1},
        {0, 0, inf, 0, 0, 1},
        {0, 0, 0, nan, 0, 1},
        {1, 1e-8f, .5f, .25f, .2f, .9f},
    };
    // Deterministic signed-cosine grid; no production decomposition in oracle.
    for (int i = 1; i <= 17; ++i)
        for (int j = 1; j <= 13; ++j)
            q.push_back({float(i) / 18, -float(j) / 14, -.71f, .39f, .27f, .83f});
    return q;
}
inline long double oracle(Query q, bool product) {
    if (!std::isfinite(q.wo_ns) || !std::isfinite(q.wi_ns) || !std::isfinite(q.wo_ng) ||
        !std::isfinite(q.wi_ng) || !std::isfinite(q.a) || q.a < 0 ||
        (product && (!std::isfinite(q.b) || q.b < 0)))
        return std::numeric_limits<long double>::quiet_NaN();
    const long double denominator = std::abs((long double)q.wo_ng * q.wi_ns);
    if (denominator == 0)
        return 0;
    return (long double)q.a * (product ? (long double)q.b : 1) *
           std::abs((long double)q.wo_ns * q.wi_ng) / denominator;
}
inline long double endpoint_oracle(Query q, bool product) {
    const auto original = oracle(q, product);
    // Independent direct formula after the analytic shading-cosine cancellation.
    if (std::isnan(original))
        return original;
    if (q.wo_ng == 0 || q.wi_ns == 0)
        return 0;
    return (long double)q.a * (product ? (long double)q.b : 1) *
           std::abs((long double)q.wo_ns * q.wi_ng / q.wo_ng);
}
inline void check(float observed, long double exact) {
    if (std::isnan(exact)) {
        if (!std::isnan(observed))
            throw std::runtime_error("Shading-normal invalid input did not yield NaN");
    } else if (exact > std::numeric_limits<float>::max()) {
        if (!std::isinf(observed))
            throw std::runtime_error("Shading-normal final overflow was hidden");
    } else if (exact == 0) {
        if (observed != 0)
            throw std::runtime_error("Shading-normal exact boundary is nonzero");
    } else if (!std::isfinite(observed) || std::abs((long double)observed - exact) >
                                               8 * std::numeric_limits<float>::epsilon() * exact +
                                                   2 * std::numeric_limits<float>::denorm_min()) {
        throw std::runtime_error("Shading-normal independent long-double oracle mismatch");
    }
}
inline void check_identity(Query q, float a, float ab) {
    if (q.wo_ns == q.wo_ng && q.wi_ns == q.wi_ng && q.wo_ng != 0 && q.wi_ns != 0 &&
        std::isfinite(q.a) && q.a >= 0 && std::isfinite(q.b) && q.b >= 0) {
        if (std::bit_cast<unsigned>(a) != std::bit_cast<unsigned>(q.a) ||
            std::bit_cast<unsigned>(ab) != std::bit_cast<unsigned>(q.a * q.b))
            throw std::runtime_error("Flat correction changed existing multiplication bits/eta");
    }
}
} // namespace shading_normal_test
