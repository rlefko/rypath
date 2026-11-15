# Core texture reference scenes

The scenes, the SPD, and the generated PNGs are project-authored and licensed
under GPL-3.0-only. `generate_assets.py` creates the PNGs with
Python's standard library. `dependencies.json` pins the byte count and SHA-256
of each scene dependency.

| Scene | Coverage |
| --- | --- |
| `core-texture-graph` | 3D checkerboard, spectrum bilerp and mix, a raw file-spectrum operand, planar point-filtered grayscale imagemap |
| `core-texture-alpha` | Fractional camera visibility and constant-zero camera coverage against a uniform environment |
| `core-texture-detail` | Directional area lighting of authored bump and tangent-space normal maps |
| `core-texture-lights` | Visible and reflected image area emission, rotated equal-area environment mapping and illuminance normalization |

Run the comparison from the repository root after building the Release
`rypath_pbrt_reference` target and the pinned PBRT reference.

```sh
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir .cache/pbrt-reference/core-textures \
  --scene core-texture-graph \
  --scene core-texture-alpha \
  --scene core-texture-detail \
  --scene core-texture-lights
```

Each renderer produces 16 independent 8×8 raw linear PFM images at 4,096
samples per pixel. The comparison uses seeds 0–15 for rypath and 16–31 for
PBRT. Each channel and declared region must agree within five combined standard
errors plus `2e-5 * max(1, abs(reference))`.

The initial 3D checker graph placed its surface at texture-space depth 2,
exactly along a checker discontinuity with zero differential in that axis.
PBRT reconstructs the hit by summing three barycentric positions, which can
round its depth below 2. rypath reconstructs the affine hit from vertex
differences, which retains 2 in this scene. The boundary setup passed 1 of
15 gates. Translating the checker declaration by 0.125 along Z isolated the
difference without changing the geometry, camera, seeds, or thresholds.
The shifted comparison passed all 15 graph gates.
