# Compact hero execution

The implementation integrated through `193ebc2` combines exact temporal-evaluation
caching with compact hero path, current-BSDF and sampled-light storage. It changes
execution storage and repeated work, preserving the transport model, random
streams, scattering equations, roulette, MIS and contribution order. Raw spectral
accumulation remains separate from reconstruction. See the
[performance follow-up](hero-performance.md) for diagnostics and qualification scope.

## Contract-10 storage

Current Metal AIR reports these layouts; they are distinct from the historical
contract-8 sizes retained in [wavefront execution](wavefront-rendering.md).

| Storage | Classic packet, bytes | Compact hero, bytes |
| --- | ---: | ---: |
| Packed path | 1,344 | 560 |
| Hot path | 336 | 336 |
| Retained endpoint | Vertex: 1,008 | HeroPrevious: 224 |

The compact current event, HeroCurrentVertex, is 496 bytes. Its HeroFrozenBSDF is
104 bytes; a sampled HeroLightEndpoint is 256 bytes. The light endpoint keeps the
SurfacePoint needed for emission and geometry PDFs, without a scattering payload
or frame. Classic packet and legacy controls keep their full Vertex representation.

Material and texture evaluation still runs at the hit. Frozen storage retains
the resulting per-family parameters, active mask and immutable scene-table
pointers; table families also retain their wavelength. Current and represented
history queries evaluate the BSDF from those prepared values and neighbor
geometry. They do not re-evaluate scene textures or repeat material preparation.
Sampled-light emission evaluation retains its original behavior.

The compact event writes directly into its zero-initialized current endpoint,
removing two redundant endpoint copies. Its outer event function is inlined;
sampling helpers retain their existing call boundaries. The ordinary full-Vertex
templates remain independent controls. Post-collapse reads through the current
alias consume only the unchanged geometry/frame/seed fields.

The BSDF uses a tag/mask and 24 initialized uint payload words. A persistent union
was unsafe under Metal copying: bytes considered padding in its largest member
could be omitted even when another member stored live values there. Explicit
words preserve that data. Device pointers retain their complete object
representation through byte copies; addresses are not narrowed through float.

Every active wavelength is prepared and validated, including companions whose
coating data will not be retained. Only hero0's coating payload is stored, but
invalid active companions still fail preparation. Arrival emission keeps its
pre-collapse contribution state. Collapse still precedes future light/BSDF
sampling; its density/throughput compensation and active-mask changes are unchanged.

The host sizes selected pools from the contract handshake and checks sizes,
alignments and allocation bounds. Contract 10 exposes only the versioned
`rypath_wavefront_contract_v10` symbol. A new host cannot find it in an old library;
an old host cannot find the removed unversioned symbol in a new library. Both
mismatches fail before completing frames or consuming sample indices.

## Retained validation receipts

Evidence is retained outside Git under
`.cache/validation/hero-performance-implementation-001/`. The frozen
`combined-light-artifacts` include binary/metallib hashes, source.diff and new
files for the pre-inlining implementation integrated at `ffadff3`; these receipts
bind that snapshot. The subsequent event experiment is identified separately below.

- `combined-gpu.log`: seven focused Release tests pass with Metal API validation
  in 211.92 seconds: wavefront queue, Area frame, temporal cache, hero render,
  mutation frame, current BSDF and sampled-light endpoint. These are test durations.
- `stale-contract-both-directions.json`: new-host/old-library and
  old-host/new-library lookup failures each report zero completed frames and
  zero consumed sample indices; executable/metallib hashes are retained.
- `combined-matrix-screen/manifest.json` and `pixel-audit.json`: the cumulative
  `compact-cache-light` arm retains bit-exact final raw and reconstructed FLOAT
  RGB against the baseline in all four scenes, eight image comparisons total.

These checks support the bounded execution changes. They do not establish
convergence or universal image identity across scenes and schedules.

## Short native static screen

The frozen screen uses native 1920×1080 on M4 Max, two warmup and three measured
frames per run, seed 51001, visible hero wavelengths, Area replay with one
spectral-refresh mutation, reciprocal16 sources and conditional-mean output.
Reconstruction is signed temporal with conservative variance. CPU wall throughput
includes the complete frame pipeline; startup and post-loop exports are separate.
API/shader validation and profiling are disabled for these timing runs.

| Scene | Frozen baseline, FPS | Cache + compact + light, FPS |
| --- | ---: | ---: |
| Cornell | 0.270115 | 0.730929 |
| Prism | 0.271217 | 0.727409 |
| Pavilion day | 0.138606 | 0.294264 |
| Pavilion night | 0.131355 | 0.275469 |

This initial screen is one short static seed, not sustained or moving-camera qualification.
The table measures the cumulative integrated implementation; it does not isolate
a performance contribution from caching, path/BSDF storage or light storage.
Further experimental variants are outside this integrated result.

### Event reuse and inlining screen

The later `remaining-matrix-screen` repeats two controls and six variants on all
four scenes: 32 runs and 160 native frames with the same two-warmup/three-measured
protocol and seed. All 64 final raw/reconstructed comparisons retain exact FLOAT
bits against the earlier baseline, including signed zeros. The integrated event
variant (`outer-inline-artifacts`) gives these complete-pipeline rates:

| Scene | Repeated compact control, FPS | Inlined compact event, FPS | Seconds per frame |
| --- | ---: | ---: | ---: |
| Cornell | 0.732745 | 0.983115 | 1.017 |
| Prism | 0.729039 | 1.063001 | 0.941 |
| Pavilion day | 0.297703 | 0.323684 | 3.089 |
| Pavilion night | 0.277885 | 0.300364 | 3.329 |

The event-only Release hero/current tests pass in 84.08 seconds. This variant
does not include the neutral typed-scattering experiment or the small texture,
coating and resource-reference variants. Their artifacts and results remain
retained; no isolated substantial gain was established for them. These short
results still need repeated static/motion qualification, especially for tail timing.

A retained independent-PT capture of compact-only snapshot `9f71064` reports
5.70 KiB of spilled storage per shading thread, versus 8.91 KiB in the earlier
capture. This is a compiler/profiler observation before light compaction, not a
complete-pipeline FPS measurement. Capture/replay timing is not rendering FPS.
The native 20–30 FPS target and the follow-up's 10× qualification remain unmet.
