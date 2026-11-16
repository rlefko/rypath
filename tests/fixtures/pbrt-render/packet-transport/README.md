# Packet transport expectation comparisons

`manifest.json` pins the source bytes and complete declared dependency closure
for ten default cases and three optional coated-material cases. The suite generates
comparison variants; it does not edit the source fixtures or remove geometry,
materials, textures or lighting. Every changed render descriptor is retained in
run provenance. The changes select independent sampling, BDPT (or the explicit
packet-path control), bounded depth,
and (for the lens case) Gaussian filtering. All films are authored at 8×8,
4,096 samples per pixel, CIE 1931, and binary32 output.

The diffuse Cornell case has depth five and exercises connection strategies with
at least two vertices on each subpath. Existing fixtures cover smooth/rough
glass, a conductor mirror, constant/image environments, and mixed distant,
point and spot lights. `glass-delta-chain.pbrt` is an owned scene with two glass
slabs and four smooth interfaces. `visible-clamp.pbrt` is an owned constant
visible emitter under a thin lens and Gaussian filter, with depth zero and a
low finite film threshold. The optional `coated`, `coated-path` and
`coated-samples16` cases retain the existing finite-layer model boundary and
isolate the integrator and internal coating evaluation count. The path control
uses a finite threshold of `1e30` to select packet transport without clamping
the measured contributions. The failed coated BDPT comparisons are recorded in
[Pavilion validation](../../../../docs/pavilion-validation.md#coated-material-comparison-boundary).

The clamped case tests matched camera/wavelength/filter proposals without light
connections. Contribution clamping is a biased nonlinear film operation whose
expectation can depend on proposal and MIS distributions. Consequently the
BDPT connection and camera-splat/lens cases remain unclamped; this suite makes
no claim that complex clamped PBRT and rypath estimators have identical means.

Each backend uses sixteen independent seed replicates (rypath 0–15, PBRT
16–31). Pixels are correlated observations, not independent replicates. The
fixed gate for every RGB full-image and quadrant mean is five combined standard
errors plus `2e-5 * max(1, abs(reference))`. No result-dependent tolerance or
seed adjustment is permitted. The CPU reference is PBRT v4
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`, with a clean source checkout,
world render coordinates and one worker thread. Each process has a 600-second
timeout, and reported ERROR/FATAL/SEVERE diagnostics fail even with exit zero.

```sh
python3 scripts/compare_packet_transport.py --pbrt-only \
  --output-dir .cache/packet-reference
python3 scripts/compare_packet_transport.py \
  --rypath build/release/rypath_pbrt_reference \
  --reference-run .cache/packet-reference/packet-transport-RUN \
  --output-dir .cache/packet-comparison
```

Omit `--reference-run` to render both backends together. `--case` selects a
bounded subset or any of the three optional coating cases. A PBRT-only run establishes valid
reference inputs and retained replicates; it establishes no renderer agreement.
The run retains source variants, binary/metallib/input hashes, process commands,
stdout/stderr, PFM images, seed-region measurements and fixed gate results.

Reused references must match scene text, the complete declared dependency closure,
reference CLI settings, protocol and PBRT executable hash. Adding unrelated cases
to the manifest does not invalidate an otherwise identical reference.

The four local scenes are project-authored GPL-3.0-only fixtures. Reused owned
fixtures retain their own notices. Cornell derivatives retain their CC0 notices
and attribution to Benedikt Bitterli and Matt Pharr; their manifest-declared
license files are staged with each variant. The tiny floating EXR asset retains
its existing owned-fixture provenance under `../hdr-io/`. No full upstream scene
coverage is claimed.
