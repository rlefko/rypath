// Adapted from PBRT v4 util/hash.h (MurmurHash64A).
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// PBRT hashes the packed binary32 ray origin, direction, and distance.
// Assemble words explicitly: Metal float3 has padding whereas PBRT Point3f
// and Vector3f contain exactly three floats on the supported reference build.
inline float alpha_uniform(metal::float3 origin, metal::float3 direction, float t) {
    constexpr ulong m = 0xc6a4a7935bd1e995ul;
    const uint words[7] = {as_type<uint>(origin.x),
                           as_type<uint>(origin.y),
                           as_type<uint>(origin.z),
                           as_type<uint>(direction.x),
                           as_type<uint>(direction.y),
                           as_type<uint>(direction.z),
                           as_type<uint>(t)};
    ulong h = 28ul * m;
    for (uint i = 0; i < 6; i += 2) {
        ulong k = ulong(words[i]) | (ulong(words[i + 1]) << 32);
        k *= m;
        k ^= k >> 47;
        k *= m;
        h ^= k;
        h *= m;
    }
    h ^= ulong(words[6]);
    h *= m;
    h ^= h >> 47;
    h *= m;
    h ^= h >> 47;
    return float(uint(h)) * 0x1p-32f;
}
