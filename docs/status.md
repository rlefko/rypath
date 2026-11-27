# Project status

The [Enhanced ReSTIR follow-up](restir-enhanced.md) targets native-1080p,
Area reuse, one mutation and denoising at 20 FPS for Cornell/Prism and 10+ FPS
for Pavilion day/night. The opt-in finite-depth contribution-path representation
adds selected-prefix replay with exact bounded controls and retains full replay
as its default oracle. A separate opt-in Enhanced domain now evaluates canonical
atoms without outer roulette and uses independently weighted discovery roulette
for fresh paths. It also owns supported terminal-light endpoints for forced NEE
reconnection and carries strict temporal/spatial confidence in resampling MIS.
The scaled-weight repair at `3335bba` completes the four-scene native screen;
its intermediate timings remain below target. Canonical law 4 now prepares
primary/secondary geometry and neutral interior charts. Focused physical, NEE,
geometry and utility GPU checks pass. Forced connection and candidate activation
remain absent, with no new native-v4 image or timing claim.
Adaptive interior reconnection and cached suffixes remain unfinished, so this is
not yet the complete Enhanced algorithm. The frame-rate targets are unmet.

The [hero execution follow-up](hero-execution.md) combines exact temporal
memoization with smaller current, historical and sampled-light state. Its short
native-1080p Area-one screen retains exact final raw/reconstructed FLOAT pixels
in Cornell, Prism and Pavilion day/night. Command-level diagnostics and the
paired timing protocol are in [hero performance](hero-performance.md).
The 10× speedup and native-1080p 20–30 FPS targets remain unmet; the short static
screen does not qualify sustained or moving-camera performance. Further shader
scheduling and contribution-path reconnection remain measured follow-up work.

[Compact native triangle metadata](geometry.md#compact-native-triangle-metadata)
reduces Pavilion retained AS memory by 28.28% with exact final raw/denoised controls.
Its short screen establishes no rendering speedup; full procedural and compensated
hint records remain unchanged.

[Spectral noise controls](spectral-color-noise.md) add visible-CDF hero sampling
and explicit spectral-refresh/reciprocal spatial reuse. At `2fa3266`, the bounded
Cornell/Prism study improves Cornell strongly but fails Prism RGB-error gates,
including a signed-reconstruction regression. Both relative native cost gates
pass, while throughput remains about 0.30 FPS. New reuse settings remain
experimental; automatic selection still uses legacy mutation and adjacent sources.

[Hero wavelength PT](hero-wavelength.md) originally added four uniform visible wavelengths,
hero-driven continuation and cumulative spectral/direct-light balance MIS.
It is explicit through `--spectral-transport hero`; automatic selection retains
the prior scalar/packet behavior. Coated materials use a compensated hero-only
fallback. A 16-seed raw screen strongly reduces Cornell variance, while Prism
shows no established variance benefit and higher cost. One Prism regional mean
safeguard remains failed; a controlled roulette diagnostic reduces the discrepancy
without establishing its sole cause. This limits default adoption, not availability
for explicit comparisons. Historical scalar/packet results below do not measure hero.

The reconstruction repair includes [identical-frame capture and replay](reconstruction-diagnostics.md),
connected surface filtering, explicit signed variance policies and experimental
MetalFX input/appearance guides. Scene-quality qualification remains open;
existing performance and quality figures below remain historical.

The eight [golden-pipeline stages](golden-pipeline.md) have implemented baselines:
resident frames, native traversal, wavefront PT, full-replay Area reuse with
corrected MCMC, separate reconstruction, a native viewer and bounded end-to-end
studies.
The **native-1080p 20–30 rendered FPS target is not met across the scene suite**.
The [audited qualification record](pipeline-qualification.md) covers 17 quality
jobs (26,240 frames and 1,980 EXRs) and 108 native timing runs (3,024 frames) at
historical source `0ef51ba`. Pavilion day/night stress performance was not measured
in this snapshot; broader stress coverage remains open.

The [ReSTIR importance repair](restir-importance.md) adds source-target MIS and
final-selection marginalization. Its completed 212-job, six-seed 160×90 raw-MSE
screen shows scene-dependent gains and retained Pavilion tails; Cornell is worse
than eight independent estimates averaged, and no incremental mutation benefit
is established. Native follow-up stopped on a cold Pavilion-day GPU reservoir
validation failure after ten jobs completed; native and equal-time/total-work
acceptance remain open. Contract 5 fixes the identified rejection of representable
subnormal contributions; the failed cold frame now completes with Metal API
validation and nine focused Release tests pass. This recovery does not replace
the failed study or qualify performance. Earlier Area studies below measure the superseded
estimator; independent sampling remains the viewer default.


The reconciled MCMC-stage integration `df65c62` passes all 247 mandatory Debug
tests. Hero's analytical glass/TIR and CLI controls pass five focused tests at
`b3e0f05`; six focused sanitizer checks passed at `ddc87d2`. Each pull request
records its current publication checks separately. Mechanical checks do not
qualify scene quality or performance.

The [core execution follow-up](core-performance.md) enables exact guide reuse.
At `54fa952`, native static complete-pipeline p95 falls **4.5–29.5%** across the
six scenes versus frozen `b8dc9cc`, with matching final raw/reconstructed captures.
Only defocus meets p95 ≤50 ms in all three seeds; none meets p95 ≤33.33 ms in all
three. Query shadows, default AS builds and packed one-event execution remain the
defaults after the alternative policies miss their adoption gates. Moving frames
regenerate guides; this does not remove the coated scene's shading cost.

At historical source `0ef51ba`, the following viewer comparison was measured.
The [viewer](viewer.md) defaults to independent sampling and signed temporal
[reconstruction](reconstruction.md). Independent/temporal has lower post-cap
physical MSE than Area-one/temporal in all six static fixtures. Opt-in
[Area reuse](area-reuse.md) performs complete destination replay, with
[one corrected MCMC step](mcmc-mutations.md) and zero/two/four-step ablations.
This is a correctness baseline, not the published optimized reconnection method.
MetalFX remains explicit; its measured limitations apply to our integration,
not to the framework generally.

In that historical qualification, three-seed native complete-pipeline throughput
spans 2.93–21.97 FPS for independent sampling and 0.48–6.66 FPS for Area-one.
Only 2/90 primary runs meet
p95 ≤50 ms (two defocus/independent seeds); none meet approximately 33.33 ms,
and no scene/arm passes all three seeds. Four audited sustained runs add 1,784
frames and meet their duration requirements; none reaches 20 FPS. Next optimization
targets are stronger importance sampling, replay/reconnection efficiency and
guide/filter cost, with measured quality benefits. Offline acceptance remains separate.

Pavilion scene support merged in [PR #50](https://github.com/rlefko/rypath/pull/50).
[PR #51](https://github.com/rlefko/rypath/pull/51) delivers the subsequent transport,
sampling and performance fixes. The remaining Pavilion convergence investigation
is explicitly deferred to follow-up at the maintainer's request; its failed
quality qualification remains recorded. This is not completed PBRT parity.

The selected scene slice supports spectral packet transport, native geometry,
PT/BDPT connections and splats, raw FLOAT EXR output and separately accumulated
authored film clamping. See the [model record](pavilion-validation.md),
[architecture](architecture.md) and [closure history](pavilion-closure-validation.md).

## Renderer 27 evidence

Implementation revision `2cba19f4` passes **230 mandatory Debug tests**, **17 focused
Release tests with Metal API validation**, the separate CLI raw-output check and
**12 independent decoder tests**. All eight jobs in its
[CI run](https://github.com/rlefko/rypath/actions/runs/45003602662) pass. These
results identify the measured implementation revision; later documentation-only
commits have their own publication checks.

Packet PT now uses eta-compensated outer roulette; BDPT keeps its existing walk.
Contracts are **3/12/27/9/2**. The 1,008-byte Vertex uses a 1,024-byte slot.
All 37 generated-path fixtures pass: 313/313 canonical strategy records agree,
versus 33/313 for the inconsistent-density control. Selected shallow PT and BDPT
captures match renderer26 exactly. See [roulette validation](packet-roulette-validation.md).

The completed performance study passes all six 64-spp gates, with **48 captures
and 48 same-arm output audits**. At 320×170, mean render/accumulation time is
**47.9% lower by day** and **17.7% lower by night** than the frozen fast renderer14
baseline. This compares rypath revisions, not PBRT or time to equal image quality.
Both **1600×850 day-256/night-512** images and all four modified-reference raw/clamped
companions are complete, with finite/count/provenance audits and retained previews.

The fresh quality study completed **384 native captures and 256 records**, using
64 runs per renderer and scene. All **36 raw RGB mean safeguards pass**, but
**0/48 simultaneous variance/tail bounds pass**. The common uncertainty margin
multiplies point ratios by 251.9; 42/48 point ratios are at most 1.25. Raw pool/glass
and two night-tail excesses remain. Passing the permissive mean safeguards does
not establish equal converged images, and the current margin's driver is unresolved.

## Deferred follow-up

The [Pavilion investigation record](pavilion-convergence-follow-up.md) preserves
exact metrics, consumed seeds, source/model identities, images, artifact locations
and a concrete restart sequence in [issue #52](https://github.com/rlefko/rypath/issues/52).
Raw bias versus variance, per-sample clamping's
effect on the limiting image, secondary specular texture footprints, and time to
a shared quality target remain open. No further Pavilion study is running.

Full pinned-corpus coverage and offline MVP acceptance remain unestablished.
Unsupported geometry/media receive explicit diagnostics; broader materials,
cameras, media and mesh support remain on the [roadmap](roadmap.md).
