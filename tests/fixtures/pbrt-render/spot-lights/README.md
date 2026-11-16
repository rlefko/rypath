# Spot-light reference fixtures

These original GPL-3.0-only wrappers isolate spectral spot illumination against
PBRT v4 `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. They use 8×8 full-precision
CIE 1931 images and 4,096 independent samples per pixel. The physical reference
is the pinned cubic smoothstep in cosine space, not a quartic penumbra law.

| Fixture | Focus |
| --- | --- |
| `penumbra` | 25° outer cone, 15° transition; the visible plane spans center, falloff and outside-cone regions |
| `spectral-power` | Nonconstant numeric intensity, scale 0.7 and positive power 12 with cone-integral normalization |
| `affine-transform` | Nonuniform scale 1.6/0.7, xy shear 0.5, translation and oblique local from/to direction |
| `mixed` | Spot with omitted 30°/5° angle defaults, plus point, area and environment illumination |
| `rough-reflection` | Direct rough GGX dielectric reflection at maximum depth 1 |
| `rough-transmission` | Direct rough GGX dielectric transmission at maximum depth 1 |
| `volume` | Bounded homogeneous fog between camera, spot and diffuse backdrop |
| `external-quad` | Owned wrapper around the unchanged pinned external quad |

The affine fixture compares the full inverse-transformed outgoing direction;
normalizing a transformed world axis alone does not reproduce its angular lobe.
All from/to pairs are distinct and all explicit cone widths and transitions are
positive and unambiguous. Spot samples are delta-position light samples and use
finite inverse-square visibility for surface and volume direct illumination.

Run the bounded suite explicitly:

```sh
python3 scripts/compare_spot_lights.py \
  --rypath build/debug/rypath_pbrt_reference \
  --output-dir build/spot-reference-comparison
```

This is opt-in; routine CI runs only the Python harness unit tests. The suite
uses 16 independent seed means per backend: rypath 0–15, pinned PBRT 16–31.
Each full image and quadrant RGB channel uses a predeclared gate of five combined
standard errors plus `2e-5 * max(1, abs(reference))`. Pixels are not statistical
replicates. Every process has a 120-second timeout; failures and error diagnostics
are retained and rejected without retry. `--pbrt-only` checks reference inputs
and positive signal, without making a backend-agreement claim.

`manifest.json` pins wrapper bytes and SHA-256; `dependencies.json` pins complete
entry-relative dependency closures, staged beside relocated wrappers.
`provenance.json` records asset ownership separately. Run provenance records the
authored hash, separate backend comparison hashes, dependencies and overrides.

The sole reference override is `Integrator.lightsampler=uniform` for PBRT's
`affine-transform` input; rypath receives the authored input. The pinned
[`SpotLight::Bounds`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp#L1371)
transforms the cone axis while retaining circular cone angles, although
[`SampleLi`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.h#L776)
evaluates the full inverse-transformed direction. Under nonuniform scale/shear,
this bound can reject valid emission directions before direct-light evaluation.
The uniform sampler preserves those directions without changing the light model.
The original default-BVH run failed nine affine gates; replacing only the PBRT
sampler with uniform passed all fifteen using the same independent seeds and
unchanged statistical gate. The final complete suite uses this explicit override;
authored fixture bytes remain unchanged.

`assets/sportscar-quad.ply` preserves all 412 bytes of
[`sportscar/geometry/Plane_0000_m000.ply`](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/sportscar/geometry/Plane_0000_m000.ply)
at scenes revision `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`.
Yasutoshi Mori created the Sportscar model, licensed under
[CC BY 2.0](https://creativecommons.org/licenses/by/2.0/).
The upstream exporter comment remains intact; see the
[existing attribution](../ply-assets/README.md). The owned `external-quad`
wrapper transforms this unchanged selected asset. This validates selected asset
coverage only; the original Sportscar scene and full corpus are outside scope.
