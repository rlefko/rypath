// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Philox4x32-10 follows Random123 v1.14.0; see third_party/random123 for
// copyright, license, algorithm reference and pinned known-answer vectors.
#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#define RYPATH_REPLAY_THREAD thread
#else
#include <cstdint>
#define RYPATH_REPLAY_THREAD
#endif
namespace rypath_replay {
#if defined(__METAL_VERSION__)
using Word = uint;
using Wide = ulong;
constant constexpr Word contract = 1;
#else
using Word = std::uint32_t;
using Wide = std::uint64_t;
inline constexpr Word contract = 1;
#endif
struct Words2 {
    Word x, y;
};
struct Words4 {
    Word x, y, z, w;
};
// Canonical coordinates, never an output owner. Every explicit coordinate may
// change with tail_key held fixed. Lens coordinates remain latent for pinholes.
// The ideal replay measure is the product of a uniform finite tail key and five
// uniform explicit words. fresh() approximates that measure with separate Philox
// blocks under the usual pseudorandom-stream assumption: its finite run-seed
// ensemble is NOT claimed to be an exact independent uniform product ensemble.
struct ReplayState {
    Word tail_key[2];
    Word film[2];
    Word lens[2];
    Word wavelength;
    Word version;
};
struct FreshIdentity {
    Word pixel, sample_low, sample_high, seed_low, seed_high;
};
// Each stream gets a distinct complete counter word. Full-width ordinals,
// material IDs and roles are not OR'd, narrowed, or hashed together.
enum class TailDomain : Word {
    transport = 0x72707431u,
    dielectric = 0x72706431u,
    material = 0x72706d31u,
    medium = 0x72707631u
};
struct TailAddress {
    TailDomain domain;
    Word ordinal, material, role;
};
enum class FreshDomain : Word {
    tail_key = 0x66726b31u,
    film = 0x66726631u,
    lens = 0x66726c31u,
    wavelength = 0x66727731u
};
// Reserved for separate future decision generators. Selection/mutation entropy
// must not be taken from a selected payload or its explicit coordinate words.
enum class DecisionDomain : Word { selection = 0x72737331u, mutation = 0x72736d31u };

inline Words4 philox(Words4 counter, Words2 key) {
    constexpr Word m0 = 0xd2511f53u, m1 = 0xcd9e8d57u;
    for (Word round = 0; round < 10; ++round) {
        const Wide p0 = Wide(m0) * counter.x;
        const Wide p1 = Wide(m1) * counter.z;
        counter = {Word(p1 >> 32) ^ counter.y ^ key.x, Word(p1), Word(p0 >> 32) ^ counter.w ^ key.y,
                   Word(p0)};
        key.x += 0x9e3779b9u;
        key.y += 0xbb67ae85u;
    }
    return counter;
}
inline Words4 fresh_counter(RYPATH_REPLAY_THREAD const FreshIdentity &identity,
                            FreshDomain domain) {
    return {identity.pixel, identity.sample_low, identity.sample_high, Word(domain)};
}
inline Words4 fresh_words(RYPATH_REPLAY_THREAD const FreshIdentity &identity, FreshDomain domain) {
    return philox(fresh_counter(identity, domain), {identity.seed_low, identity.seed_high});
}
inline ReplayState fresh(RYPATH_REPLAY_THREAD const FreshIdentity &identity) {
    const auto key = fresh_words(identity, FreshDomain::tail_key);
    const auto film = fresh_words(identity, FreshDomain::film);
    const auto lens = fresh_words(identity, FreshDomain::lens);
    const auto wavelength = fresh_words(identity, FreshDomain::wavelength);
    return {{key.x, key.y}, {film.x, film.y}, {lens.x, lens.y}, wavelength.x, contract};
}
inline bool valid(RYPATH_REPLAY_THREAD const ReplayState &state) {
    return state.version == contract;
}
inline Words4 tail_counter(TailAddress address) {
    return {address.ordinal, address.material, address.role, Word(address.domain)};
}
// Precondition: valid(state). The evaluator checks the payload version before
// work. No destination pixel, camera, output sample or queue slot enters here.
inline Words4 tail_words(RYPATH_REPLAY_THREAD const ReplayState &state, TailAddress address) {
    return philox(tail_counter(address), {state.tail_key[0], state.tail_key[1]});
}
inline float uniform(Word word) { return float(word >> 8) * 0x1p-24f; }
static_assert(sizeof(Word) == 4 && sizeof(Words2) == 8 && sizeof(Words4) == 16 &&
                  sizeof(ReplayState) == 32 && alignof(ReplayState) == 4,
              "Replay payload ABI");
static_assert(__builtin_offsetof(ReplayState, film) == 8 &&
                  __builtin_offsetof(ReplayState, lens) == 16 &&
                  __builtin_offsetof(ReplayState, wavelength) == 24 &&
                  __builtin_offsetof(ReplayState, version) == 28,
              "Replay coordinate offsets");
} // namespace rypath_replay
#undef RYPATH_REPLAY_THREAD
