# PBRT box-filter radius validation

This extends the box camera-sample distribution to independently authored radii.
It changes the filtered image integral for nondefault radii. Transport,
wavelength sampling, sample accounting and scene/checkpoint contract versions
remain unchanged. Previously supported box and triangle camera paths retain
their arithmetic; the demo requires the original box radii (0.5, 0.5). Filter
packing initializes both radii for the demo as well as PBRT, preserving the
demo's historical camera path.

## Sampling and numerical contract

The normalized density is `1/(4*rx*ry)` inside the pixel-centered rectangle
`[-rx,rx] × [-ry,ry]`. Each axis defaults independently to 0.5 and must narrow
to a positive normal binary32 value. The shader evaluates `(2*u-1)*r`, avoiding
an overflowing `2*r` intermediate. This is the continuous distribution sampled
by the [pinned PBRT BoxFilter](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/filters.h).
No filter integral is computed. Samples retain their originating pixel and
unit weight outside the image, without clipping, splatting or edge normalization.
No random dimensions or retries are added.

Both radii must be exactly 0.5 to select the legacy raster expressions in
primary rays, lens rays and texture differentials. Other boxes use the existing
centered raster, nested FMA and scaled-normalization path. Triangle expressions
are unchanged. The conservative float support bounds described in the
[triangle record](pbrt-filter-validation.md) now cover nondefault boxes at
translation, progressive-driver and native-request boundaries. Tiny accepted
radii can round away at pixel centers. Bounds deliberately reject possible
intermediate overflow, subnormal collapse and unsafe cancellation. Narrow boxes
can avoid unsampled film corners; the legacy corner check runs only for the
default box. Failed requests leave committed film intact and permit retry.

## Fixed acceptance protocol

Portable tests cover independent defaults, typed precedence, include source
locations, nonpositive/subnormal/overflow rejection, smallest-normal acceptance,
large dimensions, tiny-radius rounding, FMA overflow and collapse. Native tests
use independent geometric oracles, including anisotropic wide and narrow boxes,
rotated/reflected cameras, large origins and all Sobol randomizations. The lens
oracle independently computes each analytic offset, narrows it to float, then
evaluates `(float(pixel)+.5f)+offset`, float division and the two float FMAs
before double-precision focus/plane geometry. This explicit input staging
accounts for camera rounding at large origins; it does not call production
filter helpers. High-index checks include 32-bit carry and the upper Sobol range.

The 1×1 depth-zero emitter cases use 32,768 paths and the original fixed seeds.
With box radii (1,1), an emitter beginning at x=.75 has probability 1/8. With
radii (1.5,.75), x>=.75 and world y>=.25 has probability 1/12. Their
default-box controls are black. A narrow box with radii (.125,.25) and an
emitter beginning at x=.0625 has probability 1/4, while its default-box control
has probability 7/16. Every case and control requires
`abs(observed-P) <= 6*sqrt(P*(1-P)/32768)+2e-5`.
Constant skies preserve exact radiance, random words, wavelength and counts.
Native tests cover owned filter storage and failed-batch atomicity/retry.
Serialized checkpoint tests use active-lens textured and scattering-medium
scenes, independent/Sobol samplers, and changed batch sizes with exact raw films.

`compare_box_filters.py` compares eight cases: an owned asymmetric pinhole
emitter and focus-plane, defocus and EWA texture-filtered lens scenes, each with
independent and fastowen Sobol sampling. Radii are explicitly x=1.5, y=.75.
The owned lens fixtures and chart are copied unchanged with a declared complete
dependency closure. Every case uses 8×8 pixels, 4,096 spp, sixteen independent
seeds per backend (rypath 0–15, PBRT 16–31), full-frame and four-quadrant RGB
means. All 120 gates require five combined standard errors plus
`2e-5*max(1,abs(reference))`. Linear PFM decoding retains HDR/negative values.
Provenance records both radii, hashes, seeds, settings, source revision/dirty
state and explicit PBRT world-coordinate override.

## Evidence on this change

Final source/tests revision: `c2916d3`, parent `01ca38f`; documentation edits do
not alter the tested source. Measurements were obtained on 2026-11-16 on Apple
M4 Max, macOS 26.6 (25G5065a), Xcode 26.6 (17F113), CMake 4.0.3 and Ninja
1.13.2. PBRT is pinned to
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.

| Check on the final source | Result |
| --- | --- |
| Debug full CTest | 176/176 passed, no skips; 376.04 seconds |
| Release full CTest | 176/176 passed, no skips; 322.13 seconds |
| ASan/UBSan full CTest | 176/176 passed, no skips; 384.12 seconds |
| Metal-disabled LLVM 18 CTest | 134/134 passed; 18.33 seconds |
| Offline Python tools | 153 tests passed |
| Static checks | Seven changed C++ sources passed clang-tidy; native analyzer clean |
| Metal API validation (`MTL_DEBUG_LAYER=1`) | 44/44 passed, no skips; 329.56 seconds |
| Release pinned-PBRT comparison | Eight cases, 120/120 gates; 19.20 seconds wall time |
| Legacy raw-film comparison | 58/58 PFM outputs byte-identical to `95ac4fc`; 3.70 seconds |
| Demo raw-film comparison | Checkpoint film payload byte-identical to `95ac4fc`; 0.20 seconds |
| Deliberate old-offset mutation | Rejected by the analytic box edge test, exit 1; 1.72 seconds |

The full suites include analytic filter coverage, sampler streams, owned native
storage, failed-batch atomicity/retry, lens geometry and serialized checkpoint
resume. Commands use the supported configure/build/test presets; logs are
retained under ignored `build/box-*-full.log`, `build/box-portable.log`,
`build/box-python.log` and `build/box-native-analysis.log`. Independent reviews
cleared the final source. These timings measure validation cost, not renderer
performance.

The final Release comparison produced 256 PFM outputs. Its maximum
error/bound ratio was 0.489013 for fastowen defocus top-right red. Artifacts,
per-seed measurements and provenance are under ignored
`build/box-reference-release/box-filters-r662mryj/`; the source revision is
`c2916d3` and the dirty state comprises documentation edits. The statistical
summary measures 19.09 seconds inside the harness; 19.20 seconds includes its
process startup and setup.

SHA-256 identities in the comparison provenance and legacy report agree:

| Artifact | SHA-256 |
| --- | --- |
| Release `rypath_pbrt_reference` | `93f210caa06c38ece97c1cd75e82f0d0fac1bf3cb04bebe5b042bde8ff5212a0` |
| Release `rypath.metallib` | `619260efd683d7af604658acb397f4476b69e032e1bde944c67b20d8ffacb085` |
| Pinned CPU PBRT | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |

The legacy inventory covers 52 existing surface, lighting, spectrum, texture
and triangle-filter cases plus six lens cases. The exact inputs, dependency
hashes and raw-output hashes are retained in the durable checkout's ignored
`.cache/renderer-followups/legacy/box/report.json`, with baseline and candidate
outputs beside it. Demo settings are 16×12, 64 spp, seed 17 and batch size 8;
`.cache/renderer-followups/demo/box.json` records matching raw checkpoint-film
payload hashes and the actual demo binary hash. These are selected regression
cases, not an exhaustive equivalence claim.

The mutation forces nondefault box offsets back to half-pixel radii. The edge
case returned zero hits instead of probability .125, exceeding the fixed
0.0109818868753 error limit; triangle controls passed before that failure.
`build/box-mutation/provenance.json` retains mutant source, library and test
hashes, and `build/box-mutation.log` records the expected exit 1. This directly
checks sensitivity to the prior ignored-radius behavior without changing gates.
The lead pipeline's commands, exit statuses and timings are retained in the
durable checkout's ignored `.cache/renderer-followups/final-validation.json`;
strict validation details are in `build/box-strict.log`.

The showcase generator stages original narrow/default/wide scenes at 768×512,
512 spp. It does not render or publish images itself. This bounded validation
does not establish full external-corpus conformance.
