// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt. A sampled light has no scattering material/frame.
// Keep the complete SurfacePoint for emission textures and geometry PDFs.
struct HeroLightEndpoint {
    SurfacePoint point;
    float4 beta;
    float2 uv;
    float pdf_fwd;
    uint type, instance, primitive, kind, light, path_id;
    bool connectible, delta_light;
};
static_assert(sizeof(HeroLightEndpoint) == 256 && alignof(HeroLightEndpoint) == 16 &&
                  sizeof(HeroLightEndpoint) < sizeof(Vertex),
              "Hero sampled-light endpoint layout");
