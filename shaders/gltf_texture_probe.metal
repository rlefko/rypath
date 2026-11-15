// SPDX-License-Identifier: GPL-3.0-only

#include "../src/gltf_texture_probe_wire.h"
#include "image_texture.h"

using namespace metal;
namespace wire = rypath::render_wire;
namespace probe = rypath::gltf_texture_probe;

kernel void rypath_gltf_texture_probe(const device wire::ImageTexture *textures [[buffer(0)]],
                                      const device wire::TextureLevel *levels [[buffer(1)]],
                                      const device wire::Word *texels [[buffer(2)]],
                                      const device probe::Query *queries [[buffer(3)]],
                                      device probe::Result *results [[buffer(4)]],
                                      constant probe::Parameters &parameters [[buffer(5)]],
                                      uint id [[thread_position_in_grid]]) {
    if (id >= parameters.query_count)
        return;
    const auto q = queries[id];
    float4 value(NAN);
    if (q.texture < parameters.texture_count && q.texel_word_count <= parameters.texel_word_count) {
        const device auto &texture = textures[q.texture];
        if (texture.level_begin <= parameters.level_count &&
            texture.level_count <= parameters.level_count - texture.level_begin)
            value = rypath_image::gltf_rgba(texture, levels, texels, q.texel_word_count,
                                            float2(q.u, q.v), float2(q.dsdx, q.dtdx),
                                            float2(q.dsdy, q.dtdy));
    }
    results[id] = {value.r, value.g, value.b, value.a};
}
