# Project status

## Current milestone

Pavilion scene support merged in [PR #50](https://github.com/rlefko/rypath/pull/50).
[PR #51](https://github.com/rlefko/rypath/pull/51) remains a draft while correctness,
performance and raw-image quality closure continue. It is not completed PBRT parity.

The selected Pavilion slice supports spectral packet transport, native geometry,
PT/BDPT connections and splats, raw FLOAT EXR output and separately accumulated
authored film clamping. See the [Pavilion model record](pavilion-validation.md),
[architecture](architecture.md) and [closure evidence](pavilion-closure-validation.md)
for scope, limitations and detailed validation history.

Current candidate `e8838e6` uses format/identity/renderer/sampling/accumulation
contracts **3/12/25/7/2**. Renderer 25 halves the reserved packet vertex slot to
1,024 bytes, preserving the actual 1,008-byte vertex and transport arithmetic.
Compact storage is retained for the next candidate after material, walker,
scene and fixed daytime timing qualification. Renderer 24's explicit numerical
error-domain change remains: PT skips unused stored BDPT densities while
retaining sampled and consumed checks; BDPT finalizes both directions. See
[checkpointing](checkpointing.md).

## Published and local validation

Published `6a645d3` passes all **228 mandatory Debug tests** and all eight jobs in
[CI run 44938487943](https://github.com/rlefko/rypath/actions/runs/44938487943).
That result covers the published revision, not later local candidates.

Local compact renderer 25 at `e8838e6` passes:

- All **11 focused Release tests**, with API validation for GPU work, and all
  **12 independent decoder tests**. Two stale host fixture assumptions failed
  first and were corrected without production changes; both failures remain
  retained. All GPU tests in those focused attempts passed.
- All **2,936 non-version output words and 8,800 typed material fields** exactly
  against renderer 24; only eight renderer metadata words differ. Both actual
  host/library mismatch directions between renderers 24 and 25 are rejected.
- The actual walker: **373/373** canonical records versus **81/373** for the
  matched mixed-density inverse; maximum canonical hardware-weight error is
  **1.27605e-7**. Literal PT retains zero unused stores and no virtual BDPT family.
- Both day/night 320×170/64-spp controls: all **11,750,400 Film payload bytes**
  and every raw/authored-clamped FLOAT component remain exact against renderer
  24, with the declared contract/identity/build transition and **64 commands**.

These are focused local checks, not current full-suite or CI qualification.
Detailed source associations, corrections and receipts remain in the
[closure record](pavilion-closure-validation.md).

## Current performance and open work

The compact renderer-25 DAY64 pilot uses one warm-up per arm and five alternating
measured pairs at 320×170, 64 spp, seed/material 0. All twelve same-arm Film/FLOAT
controls pass, and compact25 is faster in every measured pair. Recorded means are:

| Host measurement | Compact renderer 25 | Frozen fast-v5 |
| --- | ---: | ---: |
| Render and accumulation | 6.464162 s | 7.081396 s |
| Whole process | 22.3684843 s | 52.108765 s |

The loop ratio is **0.912837242826132**. The unchanged 1.10 ratio limit,
7.779024-second loop ceiling and process-time gate all pass. This qualifies the
fixed daytime pilot, not full two-scene performance or time to quality.

The earlier specialized renderer 24 pilot remains a retained failure: loop
8.258184 versus 7.107336 seconds, ratio 1.1619239614, failing both loop gates.
O2 also remains rejected after its production compilation timeout; O1 is retained.
A same-shader tile control preserves all eight Film/FLOAT outputs but reducing
the cap to 4,096 raises commands from 128 to 896 and the loop from 8.22 to 36.13
seconds. That result rejects the smaller tile without identifying a GPU hotspot.

The completed selected day soffit factorial retains all 32 seeds per arm at
256 spp, with 128 owner and eight window controls passing. Concentric mapping
reduces measured clamped RGB variance by approximately 7–12% under Halton in that
region; independent sampling is nearly unchanged. Raw variance remains sensitive
to rare events. Concentric/Halton renderer 26 / sampling 8 helpers are source-prepared,
but the mapping patch is **not applied or adopted**. Its separate qualification
and fresh full-image quality evaluation remain pending.

Current full 48-capture performance acceptance, fresh 64-replicate quality admission
and final 1600×850 day-256/night-512 images remain open. No parity, convergence or
time-to-quality claim follows from the fixed-sample controls.

## Historical evidence and coverage boundary

The frozen renderer 18 quality study passes **0/48** simultaneous variance/tail
bounds and **26/36** raw-mean safeguards. Later repairs address repeated Halton
light paths, importance shading-normal correction, conservative triangle
coverage, pole clipping, coating proposal measures and canonical MIS geometry.
Those repairs and earlier full-size images do not replace current quality
admission. See [transport regressions](transport-regression-validation.md),
[sampling](sampling-convergence-validation.md) and the
[closure record](pavilion-closure-validation.md) for revision-specific evidence.

This remains selected-scene coverage. Unsupported geometry/media receive
explicit diagnostics. Full pinned-corpus coverage and offline MVP acceptance
remain unestablished; broader materials, cameras, media and mesh support remain
on the [roadmap](roadmap.md). PR #51 remains draft and unmerged.
