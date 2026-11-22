// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#else
#include <array>
#include <bit>
#include <cstdint>
#endif

namespace rypath_packet_roulette {
#if defined(__METAL_VERSION__)
using Vector = metal::float4;
#define RYPATH_RR_THREAD thread
inline unsigned bits(float value) { return as_type<unsigned>(value); }
#else
using Vector = std::array<float, 4>;
#define RYPATH_RR_THREAD
inline std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
#endif

inline bool finite(float value) { return (bits(value) & 0x7fffffffu) < 0x7f800000u; }
inline bool positive_normal(float value) {
    const auto word = bits(value);
    return word >= 0x00800000u && word < 0x7f800000u;
}

// Multiplication can flush two nonzero grazing cosines to signed zero.
inline bool opposite_hemispheres(float a, float b) { return (a > 0 && b < 0) || (a < 0 && b > 0); }

struct EtaScale {
    float value = 1;
    bool enabled = true;
};

// Eta is sampled event metadata, not a new validity predicate on physical paths.
// Unsafe eta arithmetic disables only this heuristic for the rest of the walk.
// Subnormal scale intermediates are unsafe on devices that may flush them to zero.
inline void account_transmission(RYPATH_RR_THREAD EtaScale &state, float eta, bool transmission) {
    if (!state.enabled || !transmission)
        return;
    if (!positive_normal(eta)) {
        state.enabled = false;
        return;
    }
    const float square = eta * eta;
    const float next = state.value * square;
    if (!positive_normal(square) || !positive_normal(next)) {
        state.enabled = false;
        return;
    }
    state.value = next;
}

struct Decision {
    Vector survivor;
    float termination = 0;
    bool eligible = false;
};

inline Decision prepare(Vector beta, RYPATH_RR_THREAD const EtaScale &state, unsigned depth) {
    Decision result{beta, 0, false};
    if (!state.enabled || depth <= 1 || !positive_normal(state.value))
        return result;
    float maximum = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const float value = beta[lane];
        if (!finite(value) || ((bits(value) >> 31) && (bits(value) & 0x7fffffffu)))
            return result;
        if (value > maximum)
            maximum = value;
    }
    // A common positive scale commutes with max, including monotone FLOAT rounding.
    const float rr_maximum = maximum * state.value;
    if (!positive_normal(rr_maximum) || rr_maximum >= 1)
        return result;
    const float q = 1 - rr_maximum; // PBRT max(0,1-max); here 0<max<1.
    const float survival = 1 - q;
    if (!finite(q) || !(q > 0 && q < 1) || !positive_normal(survival))
        return result;
    // Check EVERY survivor lane before inspecting the random outcome. An unsafe
    // probability or division skips this event with survival 1 and unchanged beta;
    // never silently kill a nonzero tiny-probability path or clamp its throughput.
    for (unsigned lane = 0; lane < 4; ++lane) {
        if ((bits(beta[lane]) & 0x7fffffffu) == 0)
            continue;
        const float value = beta[lane] / survival;
        if (!positive_normal(value))
            return Decision{beta, 0, false};
        result.survivor[lane] = value;
    }
    result.termination = q;
    result.eligible = true;
    return result;
}

// One Bernoulli decision and one common survival factor for the complete packet.
// The caller retains the current arrival/NEE state; only future beta may change.
inline bool continue_path(RYPATH_RR_THREAD Vector &beta, RYPATH_RR_THREAD const EtaScale &state,
                          unsigned depth, float u) {
    const Decision decision = prepare(beta, state, depth);
    if (!decision.eligible || !finite(u) || !(u >= 0 && u < 1))
        return true;
    if (u < decision.termination)
        return false;
    beta = decision.survivor;
    return true;
}
#undef RYPATH_RR_THREAD
} // namespace rypath_packet_roulette
