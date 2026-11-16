# Sampling and descriptor-default fixtures

The owned scenes isolate direct-emission edges, diffuse/area lighting, thin-lens
EWA texture footprints and homogeneous scattering. Gaussian cases include omitted
parameters, anisotropic support and authored sigma. `edge-coarse` uses radii
0.0625 and sigma 0.01: PBRT's 2×2 piecewise-constant sampling table is materially
different from a continuous truncated Gaussian at the near-center emitter edge.
This is a normalized tabulated-filter target, not a continuous-Gaussian claim.

`cornell.pbrt` is an explicitly modified CC0 Cornell derivative by Benedikt
Bitterli, converted to PBRT v4 by Matt Pharr. It retains all geometry and materials
from the existing adapted fixture, with Gaussian filtering, the declared sampler,
8×8 film, 4096 samples, CIE1931/ISO100/FLOAT settings and maximum depth five.
The upstream identity and preserved notice are in `provenance.json` and
`assets/Cornell-LICENSE.txt`. This adds selected derivative coverage rather than
new unmodified scene-corpus conformance. No external assets are downloaded.

`manifest.json` checks every scene's bytes/hash; `dependencies.json` declares the
complete relocated input/notice closure. The chart is an unchanged owned asset.

The comparison script runs 18 randomized cases: Halton permuted digits and
ZSobol permuted digits/fast Owen/Owen. Each uses 16 seeds per backend, rypath
0–15 and PBRT 16–31, 8×8 pixels and 4096 samples per pixel. Five declared regions
each supply RGB seed means, yielding 270 fixed statistical gates. The gate is
five combined standard errors plus `2e-5 * max(1, abs(reference))`. Independent
replicates are seed means. Deterministic Halton
none/Owen and unscrambled ZSobol use separate known-answer tests.

The implicit/explicit pair separately checks exact within-backend image equality
at the authored ZSobol/fastowen 16-spp default. No repeated-seed or cross-backend
confidence interval is inferred from that pair. Film stays explicitly 8×8 to
bound rendering; complete Film omission is tested by portable translation tests.
Only PBRT's reference world-coordinate/thread CLI controls are added: descriptor
kinds, sample counts and default semantics are not replaced by harness defaults.

```sh
python3 scripts/test_compare_sampling_defaults.py
python3 scripts/compare_sampling_defaults.py \
  --rypath build/release/rypath_pbrt_reference \
  --output-dir .cache/sampling-defaults/reference
python3 scripts/generate_sampling_default_samples.py \
  --output-dir .cache/sampling-defaults/previews --render
```

The preview command creates three scenes, display PNGs, retained renderer logs,
SHA-256 metadata and a gallery with labels outside image pixels. The 16-spp
default image isolates direct emission; the Halton texture studio and ZSobol
Cornell derivative use 512 spp by default. Images are raw renderer results,
without denoising. These previews are not convergence or benchmark evidence.
