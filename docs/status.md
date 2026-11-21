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

Current format/identity/renderer/sampling/accumulation contracts are **3/12/24/7/2**.
Renderer 24 skips canonical stored-density queries that PT never consumes while
retaining sampled and consumed density checks; BDPT still finalizes both stored
directions. This is an explicit numerical error-domain change, documented in
[checkpointing](checkpointing.md).

## Published and local validation

Published `6a645d3` passes all **228 mandatory Debug tests** and all eight jobs in
[CI run 44938487943](https://github.com/rlefko/rypath/actions/runs/44938487943).
That CI result covers the published revision, not the later local specialization.

Local, unpushed `ed70254` adds an optional PT/BDPT pipeline function constant.
Production binds the scene's fixed choice; diagnostic probes retain runtime
selection when the constant is unset. Its current evidence is:

- Eight focused Release tests pass, with API validation for GPU work.
- Generic full-context output retains all **2,944 words and 8,800 typed fields**
  exactly against the accepted renderer 24 control.
- The actual-walker oracle passes **373/373** canonical records versus **81/373**
  for the matched mixed-density inverse.
- Day/night controls retain the accepted renderer 24 identity, every Film byte
  and every raw/authored-clamped FLOAT component bit.

Local validation covers these focused gates; full-suite and CI qualification for
`ed70254` remain open. Detailed gates and historical revisions remain in the
[closure record](pavilion-closure-validation.md).

## Current performance and open work

The completed specialized DAY64 pilot uses one warm-up per arm and five
alternating measured pairs at 320×170, 64 spp, seed/material 0. All twelve same-arm
Film/FLOAT controls pass. Recorded means are:

| Host measurement | Specialized renderer 24 | Frozen fast-v5 |
| --- | ---: | ---: |
| Render and accumulation | 8.258184 s | 7.107336 s |
| Whole process | 24.158364 s | 51.9789489 s |

The loop ratio is **1.1619239614**. Both the 1.10 ratio limit and 7.779024-second
loop ceiling fail; the process-time gate passes. These are host loop/process
measurements, not isolated GPU kernel time. Specialization has no established
speed win. An earlier attempt stopped at a prelaunch Xcode-process check; it is
preserved and excluded, not counted as a renderer performance result.

The isolated `-O2` control preserves full-context fields, but production packet
rendering reaches its unchanged 300-second compilation deadline. O2 is rejected;
production retains **O1**. A completed same-shader tile control preserves all
eight full Film/FLOAT outputs, but reducing the cap to 4,096 pixels increases
GPU commands from 128 to 896 and the loop from 8.22 to 36.13 seconds. This rules
out that scheduling candidate; it does not identify a GPU hotspot. A compact
vertex-storage candidate is prepared for separate qualification.

The completed day soffit factorial retains all 32 seeds per arm at
256 spp, with 128 ownership and eight window controls passing. Concentric mapping
reduces measured authored-clamped RGB variance by approximately 7–12% under
Halton in that region; independent sampling is nearly unchanged. Rare bright
paths remain, and raw variance is sensitive to those events. Concentric/Halton
is the candidate for fresh full-image quality evaluation; no sampler change has
been adopted.

Current full 48-capture performance acceptance, 64-replicate raw-quality admission
and final 1600×850 day-256/night-512 images remain open. No current parity,
convergence or time-to-quality claim follows from the fixed-sample controls.

## Historical evidence and coverage boundary

The frozen renderer 18 quality study passes **0/48** simultaneous variance/tail
bounds and **26/36** raw-mean safeguards. Later repairs address repeated Halton
light paths, importance shading-normal correction, conservative triangle
coverage, pole clipping, coating proposal measures and canonical MIS geometry.
Those repairs and earlier full-size images do not replace fresh current quality
admission. See [transport regressions](transport-regression-validation.md),
[sampling](sampling-convergence-validation.md) and the
[Pavilion closure record](pavilion-closure-validation.md) for retained failures,
component measurements and revision-specific checks.

This remains selected-scene coverage. Unsupported geometry/media receive
explicit diagnostics. Full pinned-corpus coverage and offline MVP acceptance
remain unestablished; broader materials, cameras, media and mesh-format support
remain on the [roadmap](roadmap.md). PR #51 remains draft and unmerged.
