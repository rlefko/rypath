# Packet PT roulette

This change adds outer Russian roulette to vacuum packet PT. BDPT keeps its
finite-depth camera and light walks. It follows PBRT v4's
[eta-compensated continuation policy](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer),
with explicit numerical fallbacks for the supported Metal arithmetic domain.
This changes the estimator and finite sample output, not the intended raw
finite-depth transport model. It is not a raw-variance reduction claim.

## Estimator and numerical policy

After accepted scattering, PT tracks the product of squared relative refractive
indices separately from spectral throughput. Dielectric sampling returns the
relative index actually used by its optical calculation; it is not reconstructed
from a second direction test. Thin sheets, diffuse transmission and reflection
have unit compensation. Fourier transmission uses its stored bottom/top index
under the existing radiance convention. Internal coating roulette is unchanged.

Starting at the second scattering, the maximum active component of
`beta * etaScale` determines survival. One decision applies to the whole packet.
A terminated path keeps the current vertex and its emission/direct lighting;
only future transport ends. A survivor divides future throughput by its survival
probability. Directional sampling densities and MIS factors remain separate.
Null boundaries do not advance depth. The authored depth cap remains in force.

The draw uses the unused fourth component of the existing camera-continuation
block. Other transport dimensions and material-local streams retain their
allocation. Renderer/sampling contracts advance to **27/9**; checkpoint format,
identity schema, accumulation and Vertex storage retain **3/12/2** and a
1,024-byte slot for the 1,008-byte Vertex.

Unsafe cumulative eta compensation disables roulette for the remainder of that
path. An unrepresentable current probability or compensated survivor skips that
roulette decision. These decisions precede the random comparison and preserve
raw throughput; they neither clamp a contribution nor silently discard it.
Ordinary representable cases use PBRT's `q = max(0, 1 - maxComponent)` and
survivor division by `1 - q`. This fallback is our adaptation, not a claim of
identical PBRT behavior at numerical extremes.

## Why clamped and raw noise differ

For survival probability `p`, roulette replaces a contribution `X` with `X/p`
on surviving paths. Its expectation remains `X` before nonlinear processing.
If both values exceed a later cap `K`, the expected capped value becomes `p*K`
instead of `K`. Thus roulette can thin capped bright events while increasing
raw per-sample variance. The film clamps the complete camera sample, so this
single-contribution argument is explanatory rather than a general formula for
the rendered image.

The completed renderer-26 study at `9db2b5e` retained 64 independent runs per
renderer and scene. All 36 raw-mean safeguards passed; none of the 48 simultaneous
variance/tail bounds passed. Daytime pool clamped tail energy was 4.59376 times
the modified PBRT reference, while raw pool variance was about 0.315 times the
reference. The failed quality verdict remains unchanged.

A subsequent controlled reference replay used day seed554, pixel(137,153), and
all 256 samples. The observer reproduced both complete original FLOAT images
and all accepted per-sample observation rows exactly. Disabling only that
pixel's outer roulette termination/rescaling preserved every other pixel and
all within-arm raw/clamped spectral inputs. All four native renders succeeded.

At selected sample28, depth-two roulette had termination probability
0.8494660854 and survived. Its derived raw luminance contribution was 7.69365974;
without roulette it was 1.15815674. The corresponding authored-clamped
contributions were 0.195310939 and 0.195310934. These are rounding-affected
increments derived from cumulative public film outputs, not exact clamp-local
values. They confirm saturation of the extra roulette weight in this sample.
They alone do not establish how much of the full pool discrepancy roulette explains.

A subsequent global control retained the original 64 daytime reference seeds
512–575 and disabled only outer roulette termination/rescaling, preserving the
original draw. All 64 full 320×170, 256-spp clamped renders succeeded. The seed554
selected pixel matched the earlier control exactly, and the unchanged metric
implementation reproduced both original ensembles' variance and tail energies
bit-for-bit before computing the new values.

| Day pool comparison | Original reference | Reference without outer roulette |
| --- | ---: | ---: |
| rypath26 / reference variance | 1.322864 | 0.998374 |
| rypath26 / reference positive-tail energy | 4.593758 | 0.972043 |
| Reference mean clamped luminance | 0.0283839024 | 0.0286069181 |

The controlled policy change accounts for the observed daytime pool gap in this
fixed population. It does not establish the cause of every region or night-time
discrepancy, raw-mean preservation for the control, or new quality acceptance.
These are descriptive point estimates about each ensemble's own mean, with no
new bootstrap bounds or substituted admission population. The operation took
731.6 seconds; all 198 retained output files were authenticated. This CPU
diagnostic duration is not a rypath performance result.

## Qualification status

The local renderer-27 implementation passed independent source review and 17
focused Release tests on M4 Max with Metal API validation. These cover the
shared roulette helper, actual sampled eta, packet scattering, native transport,
dielectrics, full-context densities, checkpoint contracts and raw output codecs.
The separate CLI raw-output test and all 12 independent decoder tests pass.
Both actual host/library mismatch directions (26/27 and 27/26) reject correctly.

The native two-sheet fixture retains 8,192 paths over eight fixed seeds. Its raw
mean is 0.250244 against the independent analytic target 0.25, within the
predeclared six-standard-error bound. In the later-dispersion fixture, every
owner retains its earlier point-light contribution; 6,142 packets stop before
dispersion and 2,050 reach it. The paired spectral-integral difference is
0.002502 within the predeclared 0.011217 bound. These are bounded estimator
checks, not a general unbiasedness proof.

All four supported samplers reproduce 1,024 owners exactly across tile and
threadgroup changes, and every Film field matches after checkpoint/resume with
different batch sizes. A separate old26/new27 comparison uses one identical
capture object linked to each version's libraries. Shallow PT and two BDPT
fixtures across all four samplers produce identical complete typed streams:
2,048 packets, 42 splats and all Film fields per arm, 221,117 bytes each.

Fresh scene quality, performance qualification and current-revision CI remain
pending. The retained renderer-26 CI and quality results do not qualify renderer27.
The fixed 64-run clamped-only reference control reuses the original population
for diagnosis and cannot replace fresh quality admission. The separate specular
texture-footprint policy gap remains open, with no measured variance attribution yet.
