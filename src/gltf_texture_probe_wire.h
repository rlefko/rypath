// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace rypath {
namespace gltf_texture_probe {
struct Query {
    unsigned texture;
    float u, v, dsdx, dtdx, dsdy, dtdy;
    unsigned texel_word_count;
};
struct Result {
    float r, g, b, a;
};
struct Parameters {
    unsigned texture_count, level_count, texel_word_count, query_count;
};
static_assert(sizeof(Query) == 32 && sizeof(Result) == 16 && sizeof(Parameters) == 16 &&
                  __builtin_offsetof(Query, texel_word_count) == 28,
              "glTF texture probe ABI");
} // namespace gltf_texture_probe
} // namespace rypath
