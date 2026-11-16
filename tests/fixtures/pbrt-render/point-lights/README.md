# Point-light reference fixtures

These original GPL-3.0-only wrappers isolate point inverse-square illumination,
additive lights, spectral intensity, mixed point/area/environment illumination, rough
GGX dielectric reflection/transmission, and homogeneous-medium transport. They
use 8×8 full-precision CIE 1931 images and 4,096 independent samples per pixel.
`mixed` retains point, area, and environment illumination, exposing accidental MIS
weighting of delta-position light samples. Rough reflection/transmission use
maxdepth 1 to isolate direct BSDF evaluation. `volume` places a bounded fog box
between the camera, point light, and diffuse backdrop.

Run the bounded suite explicitly:

```sh
python3 scripts/compare_point_lights.py \
  --rypath build/debug/rypath_pbrt_reference \
  --output-dir build/point-reference-comparison
```

This is opt-in; routine CI runs only the Python harness unit tests. The suite
uses 16 independent seed means per backend: rypath 0–15, pinned PBRT 16–31.
Each full image and quadrant RGB channel uses a predeclared gate of five combined
standard errors plus `2e-5 * max(1, abs(reference))`. Pixels are not statistical
replicates. Every process has a 120-second timeout; failures and error diagnostics
are retained and rejected without retry. `--pbrt-only` checks reference inputs
and positive signal, without making a backend-agreement claim.

`manifest.json` pins wrapper bytes and SHA-256; `dependencies.json` pins complete
entry-relative dependency closures, staged beside relocated wrappers. No source
overrides are applied. `provenance.json` records asset ownership separately.

`assets/sportscar-quad.ply` preserves all 412 bytes of
[`sportscar/geometry/Plane_0000_m000.ply`](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/sportscar/geometry/Plane_0000_m000.ply)
at scenes revision `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`.
Yasutoshi Mori created the Sportscar model, licensed under
[CC BY 2.0](https://creativecommons.org/licenses/by/2.0/).
The upstream exporter comment remains intact; see the
[existing attribution](../ply-assets/README.md). The owned `external-quad`
wrapper transforms this unchanged selected asset. This validates selected asset
coverage only; the original Sportscar scene and full corpus are outside scope.
