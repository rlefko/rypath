# Enhanced ReSTIR development

The target is native 1920×1080, one fresh four-wavelength hero path per pixel,
Area ReSTIR, one corrected MCMC mutation, and denoising: **20 FPS for Cornell and
Prism, and at least 10 FPS for Pavilion day and night**. These are complete-pipeline
budgets of 50 ms and 100 ms per frame. Bare path-tracing throughput is diagnostic,
not the acceptance criterion. Report static and moving-camera results, frame-time
tails, startup, and image quality separately. Preserve authored scene semantics;
resolution reductions, model simplifications, radiance clamping, and biased reuse
are not implicit ways to satisfy these targets.

## Current implementation and paper coverage

The existing Area mode is a full-replay GRIS adaptation. It does not yet implement
all of [ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
or the original paper's hybrid reconnection architecture. Its warm static frame
performs six full path evaluations per pixel after exact temporal memoization:
one fresh evaluation, one mutation, and four spatial evaluations.

| Mechanism | Current coverage |
| --- | --- |
| Contribution-aware resampling MIS | Source/destination target adaptation |
| Reciprocal spatial work sharing, §3 | Reciprocal stencil and shared reverse targets; not the paper's pairing textures |
| Final vector-weight shading, §6.3 | Conditional-mean readout, separate from the selected history sample |
| Contribution-path reservoir, §§2.3 and 6.1 | Opt-in finite-depth `(U,j)` with fresh local selection; imports still use full replay |
| Hybrid reconnection and Jacobian, Eq. 2 | Missing; imported keys still retrace complete paths |
| Dual-footprint eligibility, Eq. 5 | Missing |
| Forced NEE endpoint reconnection, §6.2.3 | Missing |
| Selective shift-pair compaction, §6.2.2 | Missing; ordinary wavefront compaction is a different operation |
| Initial-only external roulette, §6.2.4 | Missing |
| Reservoir reprojection and dual motion, §6.4 | Missing; denoiser motion vectors do not implement this |
| Duplication maps, §5 | Missing; adaptive confidence introduces bias and must remain a separately labeled option |

Published performance is evidence about that implementation and hardware, not a
forecast for M4 Max or for our spectral/coated-material model.

## Implementation order and contracts

1. Establish controlled long-history comparisons. The repeated native study has
   unresolved Pavilion differences, including differences between unchanged
   baseline processes. Use compensated traversal and, where needed, sessions
   sharing the same acceleration structure to separate traversal sensitivity from
   cache or transport changes. Do not turn an observed difference into an error
   tolerance after the fact.
2. Reduce transport shader storage with measured execution changes. The tested
   preparation/event split was slower and remains rejected for adoption. A new
   approach must reduce expensive shader storage and complete frame time while
   preserving transport; material grouping must earn its queue and memory costs.
3. Introduce a contribution-path representation with a proved local selection
   weight. On a fixed counting measure over contribution slots, selection with
   probability `target[j] / sum(target)` gives initial normalizer `sum(target)`;
   it is not a weight-one fresh sample. Preserve early emission, NEE technique
   MIS, hero likelihoods, collapse compensation, roulette and inactive-slot
   support. Verify the conversion independently before renderer integration.
4. Add invertible hybrid shifts and cached suffixes, with forward/reverse support
   checks and the actual Jacobian in resampling MIS. Add footprint eligibility,
   forced light reconnection and compaction of pairs that require replay. Start
   with explicit supported continuous charts; replay unsupported Dirac, coating
   and spectral cases without silently substituting a different model.
5. Integrate the corrected mutation with that representation. A wavelength change
   cannot reuse wavelength-dependent suffix radiance unchanged. Measure actual
   prefix/suffix work and retain the spectral color controls.
6. Optimize reconstruction and validate the complete pipeline at matched quality.
   Keep raw accumulation, denoised output and display transforms separate.

The finite-depth contribution representation is a foundation, not a claim that
reconnection, external roulette, or the complete Enhanced algorithm is present.
Unbounded-depth support requires its own declared measure and sampling contract;
it must not be implemented by an unreported truncation.

## Contribution-path runtime

`--area-path-domain contribution` opts into a fixed counting measure over
`E_0..E_D` and `N_0..N_(D-1)`, with slots `E_d=2*d` and `N_d=2*d+1`. It currently
requires four-wavelength hero transport, packed one-event wavefront Area replay,
and finite authored depth 0 through 65. The default `summed` domain is retained.
Unsupported combinations fail explicitly.

Fresh paths evaluate the original full transport tree and select an atom with
probability `t_j/S`, where `S=sum(t_j)`. The initial normalizer is `S`, not one or
`S/slot_count`. Inactive atoms retain the positive target floor. Imports, temporal
memoization, reverse targets and MCMC keep the same `j`; a wavelength mutation
fully reevaluates that atom. The fixed slot travels with its selected replay
payload and weight. Mode, depth and target scale belong to history/cache identity.

Each atom retains the original technique MIS and four spectral likelihood
weights. Projection uses all four original sensor wavelength PDFs, including for
emission before a later hero collapse. Projecting individual atoms changes FLOAT
addition grouping relative to projecting the summed path, so summed-versus-atom
comparisons use physical and statistical checks rather than bitwise equality.
Cache and queue controls within one domain still require exact equality.

The CPU checks cover fixed-slot support, signed readout, fresh generalized MIS,
and nonuniform 131-slot selection. GPU controls cover positive depth-zero and
depth-65 endpoints, early emission, dispersive/coated materials, null/invisible
crossings, repeated sample labels, queue capacities and cached replay. A 32-seed
paired multileaf mean check uses one whole-frame difference per independent seed;
its five-standard-error gate is a bias smoke test, not proof of unbiasedness or
scene-quality qualification.

## Retained measurements and limitations

The `3ed5432` runtime's native study used three seeds, static and phased-motion
scenarios, eight warmup and 32 measured frames per run: 48 runs and 1,536 measured
frames. Median paired speedups were 3.65/3.36× for Cornell static/motion,
3.90/3.59× for Prism, 2.33/2.13× for Pavilion day and 2.26/2.08× for Pavilion night.
These timings are not quality-qualified: 20 of 48 final raw/reconstructed FLOAT
comparisons differ, all in Pavilion. All captures are finite; Cornell and Prism
comparisons are exact. This bounded study is not the default 96-frame protocol.
It includes a recorded pause between conditions and ordinary desktop activity.

Pavilion day/static/seed 61002 controls retain the failure. Restoring the earlier
52-byte native metadata layout or disabling temporal caching does not restore
baseline identity. An unchanged baseline repeat also differs, with a smaller
observed maximum difference. These facts do not establish a sole cause. A follow-up static native-1080p
Pavilion-day diagnostic (seed 61002, 40 frames including eight warmup submissions)
built one shared native acceleration structure and used independent session slots
and history banks. Cache-on and cache-off raw XYZ matched bitwise in every frame;
mutation/accounting checks also matched. This isolates memoization on that AS. It
does not compare reconstructed output or qualify independent AS builds, motion,
other seeds, or Pavilion night.

Separate native-1080p bare-PT measurements on that runtime, with eight warmup and
32 measured frames, give Cornell 129.44 ms/frame (7.73 FPS) and Prism
141.18 ms/frame (7.08 FPS). A short denoiser screen adds about 27–38 ms for signed
temporal/spatial reconstruction and 10–12 ms for native MetalFX. Neither those
figures nor the combined throughput meet the complete-pipeline target.

Two execution experiments were rejected for default adoption. Splitting material
preparation from the transport event preserved all paired raw FLOAT controls,
but made the native Cornell/Prism screen about 33–37% slower. Xcode still reported
192 registers in both kernels, with 3.25 KiB and 4.16 KiB of spills. The additional
boundary did not remove the expensive shader storage. Caching normalized
geometric normals preserved paired raw/reconstructed FLOAT pixels, but a native
two-scene screen showed no meaningful complete-pipeline gain and costs about
33 MB at 1080p. Both controls are archived and excluded from this implementation. These short experiments
used two warmup and three measured frames and are not sustained qualification.

The acceleration stack (#74–77) was merged in `a6d7503`. All required hosted
checks passed; the macOS job needed one unchanged retry after pipeline creation
returned nil in a standalone arithmetic probe. The original failure and the
successful retry are retained separately. The merged stack does not close the
long-history Pavilion qualification gap above.

Raw reports, frozen executables/libraries, failed controls and provenance remain
outside Git in `.cache/validation/hero-performance-implementation-001/`.

Primary foundations: [GRIS](https://graphics.cs.utah.edu/research/projects/gris/),
[corrected MCMC mutations](https://arxiv.org/abs/2211.00166),
[Area ReSTIR](https://research.nvidia.com/publication/2024-07_area-restir-resampling-real-time-defocus-and-antialiasing),
and [wavefront path tracing](https://research.nvidia.com/publication/2013-07_megakernels-considered-harmful-wavefront-path-tracing-gpus).
