// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/replay_shared.h"
// Explicit adapter only. Existing renderer sampling is unchanged until callers
// opt in, validate the payload, and keep destination identity in wire::Sample.
namespace rypath_replay_sampler {
using namespace rypath_replay;
inline metal::uint4 vector(Words4 words) { return {words.x, words.y, words.z, words.w}; }
inline Words4 transport(thread const ReplayState &state, Word block) {
    return tail_words(state, {TailDomain::transport, block, 0, 0});
}
inline Words4 dielectric(thread const ReplayState &state, Word bounce) {
    return tail_words(state, {TailDomain::dielectric, bounce, 0, 0});
}
inline Words4 material(thread const ReplayState &state, Word bounce, Word material_id, Word role) {
    return tail_words(state, {TailDomain::material, bounce, material_id, role});
}
inline Words4 medium(thread const ReplayState &state, Word draw) {
    return tail_words(state, {TailDomain::medium, draw, 0, 0});
}
inline Words2 film(thread const ReplayState &state) { return {state.film[0], state.film[1]}; }
inline Words2 lens(thread const ReplayState &state) { return {state.lens[0], state.lens[1]}; }
inline Word wavelength(thread const ReplayState &state) { return state.wavelength; }
} // namespace rypath_replay_sampler
