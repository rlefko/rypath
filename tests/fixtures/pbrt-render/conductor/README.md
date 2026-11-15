# Conductor comparison fixtures

These eight original GPL-3.0-only PBRT wrappers isolate smooth and anisotropic
single-scattering GGX conductor behavior. All use 8×8 pixels, 4,096 independent
samples per pixel, CIE 1931, linear sRGB, and full precision film. The comparison
uses rypath seeds 0–15 and PBRT seeds 16–31; full image and four quadrant RGB means
use five combined standard errors plus `2e-5 * max(1, abs(reference))`.

| Entry | Selected coverage |
| --- | --- |
| smooth-cu-environment | Default copper eta/k, smooth reflection into constant environment |
| smooth-ag-area | Named silver eta/k, smooth finite-emitter hit |
| rough-au | Named gold eta/k, isotropic roughness, area and environment MIS |
| anisotropic-cu | Distinct u/v roughness under asymmetric finite illumination |
| anisotropy-rotated | Same model with UV frame rotated 90 degrees |
| numeric-file-optical | Numeric eta, file k above one, unremapped microfacet width, transformed external PLY |
| detail-alpha | Instance, binary procedural alpha, bump and normal-map precedence, anisotropic material |
| roughness-graph | Checker roughness graph crossing exactly smooth and rough regions |

`assets/k.spd` is original optical data for this test, with no physical-material
claim. `assets/normal-tilted.png` is the unchanged owned core-texture fixture.
`assets/sportscar-quad.ply` preserves all 412 bytes of upstream
[`sportscar/geometry/Plane_0000_m000.ply`](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/sportscar/geometry/Plane_0000_m000.ply)
at scenes revision `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`. Yasutoshi Mori
created the Sportscar model, licensed under
[CC BY 2.0](https://creativecommons.org/licenses/by/2.0/); these asset terms
remain separate from the wrappers' GPL. See the retained upstream exporter
comment and [existing attribution](../ply-assets/README.md). No further assets
are required; the original Sportscar scene is outside this selected coverage.

`manifest.json` pins entry sizes and hashes; `dependencies.json` pins each entry's complete dependency closure. The harness
checks size and SHA-256 and stages those files beside every relocated wrapper.
Named copper/silver/gold optical data comes from pinned PBRT's licensed original
spectral knots already retained in rypath, not a new downloaded corpus.
