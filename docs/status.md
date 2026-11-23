# Project status

The [golden pipeline](golden-pipeline.md) is the current development direction:
resident frames, native triangle traversal, wavefront PT, Area ReSTIR with MCMC,
and separate reconstruction in a native viewer. Persistent
[frame sessions and the headless benchmark](frame-rendering.md) support native
triangle traversal and [wavefront execution](wavefront-rendering.md), with
matched megakernel and compensated-profile controls. The retained native
megakernel baselines are 86.1–86.6 ms for Cornell and 196.6–199.4 ms for the coated
packet scene; subsequent wavefront measurements are reported separately.
The experimental [native viewer](viewer.md) now connects these implemented
stages with camera controls, raw/reconstructed comparison and paired EXR capture.
Native 1080p at 20–30 rendered FPS remains a target; end-to-end image quality,
sustained performance and signed-temporal UI qualification are pending.

Opt-in [Area-domain path reuse](area-reuse.md) now replays film, lens, wavelength
and complete indirect transport. Bounded importance contrast limits reuse-induced
weights. [Corrected MCMC mutations](mcmc-mutations.md) run before spatial reuse,
with a one-step default and zero/two/four-step ablations. Debug, Release and
sanitizer arithmetic/lifetime checks and small mean studies pass; visual improvement
and time to image quality remain unqualified.

Separate [frame reconstruction](reconstruction.md) consumes completed raw frames.
It offers MetalFX denoising/upscaling and a native-resolution signed-float temporal
fallback, with guide approximation counters, retained GPU outputs and direct
linear-RGB captures. The fallback avoids clipping fresh spectral samples before
filtering. Integrated correctness checks pass; reference-image quality and
complete-pipeline timing remain to be qualified. The viewer exposes
`--mode temporal|metalfx|raw`, with MetalFX still the default. Implemented modes and
passing renderer checks do not establish viewer image quality.

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
