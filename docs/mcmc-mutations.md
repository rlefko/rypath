# Weight-corrected Area mutations

The opt-in `area-replay` estimator performs one mutation per pixel by default,
between temporal and spatial reuse. `FrameConfig::mutation_steps` and the
benchmark's `--mutations` accept exactly `0`, `1`, `2`, or `4`. Independent
rendering remains the default estimator and applies zero mutations. Reports and
EXR provenance distinguish requested steps, effective steps, attempts and accepts.

This is our finite-replay-domain adaptation of Sawhney et al.,
[Decorrelating ReSTIR Samplers via MCMC Mutations](https://arxiv.org/html/2211.00166),
Section 3, Equation 16 and Appendix A. Their weight correction permits mutations
of properly weighted reservoir samples without a burn-in period; their schedule
places mutations after temporal reuse. Our integer proposals, rounded-probability
correction and complete spectral replay are implementation choices. We do not
implement the paper's reconnection-vertex mutation, or establish its reported
visual improvements for this renderer. The underlying Area-domain adaptation and
its limitations are described in [area-reuse.md](area-reuse.md).

## Replay state and proposal

[mutation_shared.h](../src/mutation_shared.h) is shared by CPU and Metal.
[replay_shared.h](../src/replay_shared.h) stores five explicit unsigned 32-bit
coordinates (film pair, lens pair and wavelength), a two-word indirect-tape key,
and a version. The declared normalized measure `mu` is the uniform product of
these finite coordinates and key. Coordinate evaluation uses the upper 24 bits;
unused bits and pinhole lens coordinates remain latent state. This is a finite
replay estimator, not an infinite primary-sample tape. Philox fresh/decision
streams assume pseudorandom uniform words; their finite run-seed ensemble is not
claimed to be an exact independent product ensemble.

Each proposal chooses one of four blocks with equal, state-independent
probability: film, lens, wavelength or tail key. Both coordinates of a pair move.
Independent signs and nonzero integer magnitudes make the transition symmetric
under unsigned modular addition. The default magnitude is `256` through `2^24`
in multiples of `256`, corresponding to normalized offsets `2^-24` through
`2^-8`. Scale validation requires `1 <= magnitude_bits <= 31` and
`magnitude_bits + shift <= 31`, so constructing a delta cannot wrap to zero.
The version is preserved.

Film and lens proposals wrap on their coordinate torus. A tail-key move
regenerates the whole indirect random tape; integer proximity of keys does not
mean proximity of paths. These block kernels need not be irreducible. Fresh
weight-one candidates provide coverage; there is no claim that mutations alone
explore the whole domain. A changed payload can also produce unchanged radiance.

Proposal and acceptance entropy have distinct Philox counter domains containing
pixel, full submission serial, mutation step and word block, keyed by the run
seed. They do not consume selected payload coordinates. The submission serial,
not `sample_index`, separates decisions when sample indices repeat. An active
history must never reuse its identity/step decision stream.

## GPU schedule and cached values

[area_reuse.metal](../shaders/area_reuse.metal) proposes from the selected temporal
payload, then the host schedules a complete deterministic wavefront replay at
the current destination pixel and camera. Wavelength initialization, packet
termination, material and homogeneous-medium choices, emission, NEE, MIS and
roulette are reevaluated through the existing transport estimator. No previous
frame or source-neighbor XYZ substitutes for this evaluation.

Acceptance replaces the complete payload, UCW, current unweighted XYZ and target
tuple. Rejection retains that tuple exactly. Every configured step runs, including
after rejection or equal targets. Counts are diagnostics, never an adaptive
stopping criterion. After all steps, the temporal buffer becomes immutable for
the five-slot spatial merge, including duplicated clamped neighbors.

Each mutation adds one complete path evaluation per pixel. Total evaluations are
`5+n` on a reset and `6+n` with history: six/seven at the default `n=1`. There is
still one fresh canonical path and one final weighted output sample per pixel.
Black candidates count normally. The host checks attempts equal `pixels*n`,
accepts do not exceed attempts, and evaluation counts match the schedule. Counter
products are validated before dispatch. Errors fail the whole frame; partial
mutation results cannot publish history. Raw weighted XYZ stays separate from
the unweighted evaluation cache. These are Area wire contract version 3 rules.

## Acceptance and contribution weight

For a properly weighted input, `E[w*h(U)] = integral h dmu` for every integrable
test function. With symmetric proposal `Q`, ideal MH acceptance and its accepted
weight are

```text
A(u,v) = min(1, t(v)/t(u))
w_accepted = w * t(u)/t(v)
w_rejected = w
```

The implementation instead uses its **actual represented** forward and reverse
acceptance probabilities. It divides normal mantissas and retains a separate
exponent for `q = min(t_old,t_new)/max(t_old,t_new)`. A dyadic comparison samples
the rounded `q` without forming `1-q` in binary32; even extreme exponent gaps
retain nonzero acceptance. At most nine independent 32-bit words are needed.
Uphill/equal proposals accept without entropy. The accepted correction is

```text
c(u,v) = A(v,u)/A(u,v)
w_accepted = w*c(u,v)
uphill: c=q; downhill: c=1/q; equal: c=1
```

In real weight arithmetic, incoming accepted weighted flow to `v` is
`Q(u,v)*A(u,v)*c(u,v) = Q(v,u)*A(v,u)`. Summing it with retained rejection flow
gives unit mass at `v`. Thus actual rounded probabilities preserve the uniform
UCW identity with this correction, even though their kernel only approximately
satisfies detailed balance for the exact target. Substituting an independently
rounded ideal target ratio would lose that exact flow cancellation. This proof
concerns a valid weighted input and independent entropy, not startup from an
arbitrary unweighted chain state.

All weights and targets must be positive finite normal floats. Correction splits
mantissas and exponents, avoiding an underflowed intermediate `q` when the final
weight is normal. There is one normal multiply/divide rounding, followed by exact
power-of-two scaling. Before reading acceptance entropy, the helper checks the
normalized corrected fraction's combined `frexp` exponent lies in `[-125,128]`.
It rejects subnormal/overflow scaling domains even if `ldexp` could round back to
`FLT_MIN` or return finite `FLT_MAX` under toward-zero overflow. Such failures
are arithmetic errors, never ordinary MH rejections. There is no UCW clamp.

## Numerical envelope and target policy

Under the normal-operation assumptions in
[the reservoir numerical bound](area-reuse.md#conditional-local-error-bound),
one accepted correction has relative operation error at most `u`; rejection and
equal targets preserve the weight exactly. Fixed `n` mutations add a local
weighted-measure coefficient factor at most `(1+u)^n`. CPU round-to-nearest uses
`u=2^-24`; the conservative correctly rounded non-fast Metal envelope uses
`u=2^-23`, conditional on the backend meeting the cited language guarantees and
operation sequence. This is not exact finite-arithmetic unbiasedness or a claim
of subnormal transport support. Every possible accepted outcome must pass the
normal-range preflight; conditioning away failed outcomes is outside the proof.

Including temporal `K=2` and spatial `K=5` reservoir coefficient bounds gives

```text
g_n = (1+3*u)^5 * (1+u)^(6+n)
E_next <= (g_n/2)*E_previous + (g_n-1)
E_steady <= (g_n-1)/(1-g_n/2), if g_n < 2
```

Here `E` bounds error for every test function relative to `integral abs(h) dmu`;
it is not relative image error. The recurrence requires fresh temporal input,
valid histories, fixed/exogenous schedules and no unaccounted errors. It does not
qualify history-only cycles or establish a variance bound.

All four mutation counts use the same bounded target
`1 + 0.1*L1/(target_scale+L1)` with default scale one and enforced range
`[1, binary32(1.1)]`. The separate
[realized UCW bounds](area-reuse.md#realized-ucw-magnitude-bounds) include mutation
probability rounding: the conditional global upper bound is below `1.990` for
one step and `7.756` for every legal count. Those magnitude bounds are distinct
from the coefficient-error recurrence above. They assume newly created
bounded-policy histories and the declared pipeline. The weak target contrast
sacrifices importance strength; it clamps neither physical radiance nor UCWs,
and does not bound raw path contributions or guarantee finite physical variance.

## Validation and remaining evidence

- [mutation_tests.cpp](../tests/mutation_tests.cpp) independently enumerates small
  proposal lattices and weighted indicator/signed-integral expectations, including
  nonstationary properly weighted starts and `0/1/2/4` steps. It checks balance,
  dyadic boundaries, counter separation, extreme-ratio cancellation and invalid
  state/range handling. Omitting the weight correction is a failing negative
  control. Higher-precision oracle rounding has a separate error allowance.
- [mutation_math_probe.mm](../tests/mutation_math_probe.mm) checks the shared
  Metal helper against independently written host proposal/counter arithmetic
  and higher-precision acceptance/weight expectations. GPU-returned dyadics also
  receive below/equal/above decision-boundary queries. This exercises the normal
  boundary regression and extreme nine-word decisions; sharing a helper between
  CPU and GPU alone would not establish correctness.
- [mutation_frame_tests.cpp](../tests/mutation_frame_tests.cpp) checks scheduling,
  counters and lifecycle behavior. Its single-pixel CPU replay uses the shared
  helper and establishes adapter agreement, not an independent estimator proof.
  Separate prespecified mean smoke gates compare all counts with an independent
  raw renderer for sky, DOF edge and indirect NEE scenes, over twelve independent
  session seeds with fixed cold/warm schedules. They are not a full-profile
  convergence proof.

Initial M4 Max Debug/API-validation runs in
`.cache/validation/mcmc-001/gpu.log` passed `metal.mutation_frame` in 19.74 seconds
and `metal.mutation_math` in 0.32 seconds. The same evidence directory records
passing portable mutation and CLI tests. The strengthened Debug arithmetic probe also passed in 0.31 seconds. Release
with API validation passed the retained zero-step Area frame test (32.52 s),
mutation frame test (7.16 s) and mutation math probe (0.31 s). ASan/UBSan passed
the same checks in 8.88 s, 9.28 s and 0.28 s, respectively. These runs used the
implementation committed as `6835be5`; parent restacking changed no renderer or
test source. Logs are `final-probe-gpu.log`, `release-gpu.log` and `asan-gpu.log`
in the same evidence directory. No equal-time image-quality improvement, visual correlation
reduction, or performance benefit has been established for this adaptation.
