# Dielectric comparison fixtures

These eight original GPL-3.0-only PBRT wrappers isolate smooth and anisotropic
single-scattering GGX dielectric behavior. They use 8×8 pixels, 4,096 independent
samples per pixel, CIE 1931, linear sRGB, and full precision film. The harness
uses rypath seeds 0–15 and PBRT seeds 16–31. Full image and four quadrant RGB
means use five combined standard errors plus `2e-5 * max(1, abs(reference))`.

For matched reference rendering only, the harness changes the roughness graph's
zero endpoint to 0.01 for both backends. Pinned PBRT can otherwise produce
negative checker values through cancellation, then replace invalid radiance
with black. The native zero/0.15 fixture remains unchanged. Provenance records
the override, original/staged hashes, executable and Metal library hashes,
revisions, seeds and settings. Renderer error diagnostics reject the run.
`--pbrt-only` validates inputs and does not establish image agreement.

| Entry | Selected coverage |
| --- | --- |
| smooth-environment | Constant eta 1.5, smooth reflection and radiance transmission |
| smooth-area | Smooth transmitted finite-emitter hit |
| rough-isotropic | Isotropic GGX transmission, area/environment MIS |
| anisotropic | Distinct u/v widths under asymmetric finite illumination |
| anisotropy-rotated | Same model with UV frame rotated 90 degrees |
| numeric-file-optical | Synthetic dispersive file eta, unremapped width, transformed external PLY |
| detail-alpha | Instance, binary procedural alpha, bump and normal-map precedence |
| roughness-graph | Native smooth/rough checker; matched comparison uses 0.01/0.15 |

`assets/eta.spd` is original synthetic optical data, with no physical-material
claim. `assets/normal-tilted.png` is the unchanged owned core-texture fixture.
`assets/sportscar-quad.ply` preserves all 412 bytes of upstream
[`sportscar/geometry/Plane_0000_m000.ply`](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/sportscar/geometry/Plane_0000_m000.ply)
at scenes revision `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`. Yasutoshi Mori
created the Sportscar model, licensed under
[CC BY 2.0](https://creativecommons.org/licenses/by/2.0/); these asset terms
remain separate from the wrappers' GPL. The retained upstream exporter comment
and [existing attribution](../ply-assets/README.md) identify the source.
This owned wrapper establishes selected mesh-asset coverage; it does not claim
compatibility with the full original Sportscar scene.

`manifest.json` pins entry sizes and hashes. `dependencies.json` pins each
entry's complete dependency closure, copied beside relocated wrappers by the
comparison harness. The three dependency files total 590 bytes. The pinned
PBRT implementation revision is `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.
