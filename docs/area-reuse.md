# Area-domain reservoir math

This is the historical contract-3 record. Contract 4 replaces its bounded target,
reuse weights, output policy and warm evaluation count; see the current
[importance sampling repair](restir-importance.md). The retained failures and
validation below describe their named revisions, not the new estimator.

The opt-in `FrameEstimator::area_replay` implementation reuses full camera paths
across film, lens, wavelength and indirect transport variables. It is an
area-domain full-replay GRIS adaptation, with a correctness baseline that
retraces destination paths. It does not yet implement the Area ReSTIR paper's
fractional footprint matching or primary-hit lens reconnection.
[Weight-corrected MCMC mutations](mcmc-mutations.md) run between temporal and spatial reuse. Independent rendering remains the default; offline
transport and checkpoint contracts remain separate.

The benchmark enables it with `--estimator area-replay` and requires wavefront
execution. `--target-scale` selects the positive normal scale (default `1`) in
the bounded-contrast target described below. It does not clamp radiance or UCWs.
Reports distinguish fresh paths, complete replay
evaluations, reservoir pool sizes, submission serials and history generations.
Native and compensated intersections remain separate numerical choices.

## GPU schedule and history

A fresh payload is evaluated once per destination pixel. Temporal reuse merges
that result with the previous reservoir's payload, fully reevaluated at the
current camera; reset frames use the fresh input alone. The selected temporal
result undergoes the configured fixed mutation steps, then is frozen before spatial reuse. Each pixel then merges its cached center
with fully replayed left, right, upper and lower neighbors. Edge clamping keeps
all five scheduled slots, including duplicates. Cached source-pixel or
previous-frame XYZ never substitutes for destination evaluation.

Each frame has one fresh path per pixel and **five plus `n` complete path
evaluations after a reset, six plus `n` with history**, for `n` configured
mutation steps (default one; ablations zero, two and four). Output is the selected unweighted XYZ times
its normalized contribution weight, counted once. All replay and reservoir work
is included in command counts and timing. Raw output is separate from the
recyclable leaf and temporal caches.

One Area frame may be pending per session. Two history banks are independent of
retained frame captures. A successful final callback publishes history; failed
frames cannot publish partial output and preserve eligible prior history.
Changing `FrameRequest::epoch` permanently invalidates eligibility, including
when that submission fails. A monotonic internal submission serial separates
selection decisions even when output sample indices repeat. Only successful
publication advances the global history generation. Camera cuts should change
epoch; smooth motion uses complete current-camera replay without primary-hit
reprojection in this version.

## Bounded importance contrast

The current renderer policy is a positive, bounded importance proxy:

```text
L = abs(X) + abs(Y) + abs(Z)
t(F) = 1 + 0.1 * L / (target_scale + L)
1 <= t(F) <= R, where R = binary32(1.1) = 1.100000023841858...
```

`bounded_target_xyz` divides the three magnitudes and scale by their maximum
before forming the ratio. The normalized denominator lies in `[1,4]`; it never
forms the potentially overflowing unscaled HDR L1 sum. Output is checked against
`[1,R]`. Tiny importance-proxy terms may underflow or flush without losing
support, since the target remains at least one. This is not a promise of
subnormal transport support. Raw XYZ is untouched, including its sign and
magnitude. The target is not a radiance clamp, nor is the finalized UCW clamped.

This deliberately limits importance contrast to ten percent. It sacrifices much
of the importance-sampling strength of a radiance-like target to prevent the
reuse-specific weight explosion described below. No variance or speedup benefit
is established by this choice. Keep the same `R` for later `0/1/2/4`-mutation
ablations; raising contrast only in lower-mutation arms would confound their
quality comparison. The scale controls the transition within this common range,
not its endpoints.

### Failed linear-target evidence

The earlier linear target `epsilon + L`, with `epsilon = 1e-6`, passed the other
expanded NEE, indirect, homogeneous-media and constant-radiance mean smoke
checks but failed the Gaussian/DOF half-occluder case. Its retained
`.cache/validation/area-expanded-gpu.log` reports an X-channel mean residual of
**-2.747978** against tolerance **2.213187** over 16 independent sessions, each
with a cold frame followed by two warm frames. The diagnostic rerun in
`.cache/validation/area-diagnostic-gpu.log` preserves the same failure; the
aggregates are retained in `.cache/validation/area-linear-edge-summary.json`.

| X-channel mean over the 16 sessions | Value |
| --- | ---: |
| Cold Area output | 37.030404 |
| First warm output | 35.064811 |
| Second warm output | 34.356766 |
| Independent raw reference | 37.458766 |
| Analytic half-emitter | 37.402891 |

The deficit grows in the partially visible neighboring columns while bright
columns are more stable. This supports a rare-weight mechanism; it does not
prove the adapter free of bugs or turn the failed gate into passing evidence.
These are historical working-tree results for the linear policy, not GPU
qualification of the bounded policy.

A two-state example shows why sixteen empirical standard errors can miss a
large contribution. Let the source states have `F_s = (L,0)` and target
`(L+epsilon,epsilon)`. Two fresh uniform candidates form a mixed lit/dark pool
with probability `1/2`. Conditional on that pool, the dark state is selected
with probability `epsilon/(L+2*epsilon)` and UCW
`(L+2*epsilon)/(2*epsilon)`. That rare branch supplies one quarter of the dark
indicator's required total weighted mass of one half. If the destination lights
that state instead, half its expected signal lies in this rare tail. With
`L=100` and `epsilon=1e-6`, the branch occurs about once in 200 million pools.
An entirely black source pixel has a constant target and is not this mechanism.

Positive support alone therefore does not make a finite-seed mean/SE gate a
reliable bias detector for this policy. The linear helper remains available for
math diagnostics; the renderer uses bounded contrast. Target changes require
new history and provenance rather than importing linear-policy reservoirs into
the bounded-weight guarantee.

## Normalized contribution weights

Let `mu` be a common normalized replay-domain measure and `U` a replay state.
The complete destination estimator `F_i(U)` includes the camera/filter, spectral,
emission, NEE, medium, MIS and roulette factors required by the chosen model:

```text
I_i = integral F_i(u) dmu(u)
E[w * h(U)] = integral h(u) dmu(u)
```

The second identity defines the required normalized unbiased contribution weight
(UCW) for every integrable test function `h`. Correctness for the source pixel's
color alone is insufficient. A fresh uniform-domain sample has `w = 1`.

For `K` scheduled, properly weighted inputs, choose a positive target `t` and set

```text
s_k = w_k * t(U_k)
S = sum s_k
Pr(selected = k | pool) = s_k / S
w_selected = S / (K * t(U_selected))
```

Conditioning on the complete candidate pool gives

```text
E[w_selected * h(U_selected) | pool] = (1/K) * sum w_k * h(U_k)
```

Taking expectations proves proper weighting in real arithmetic. Correlated
inputs, including two copies of the same reservoir, satisfy this algebra when
each imported sample satisfies the UCW contract. Duplicates provide no additional
independent information. This proof does not establish a variance reduction or
convergence rate for persistent histories.

`K` is the number of candidates in this merge, not their historical ancestry,
confidence, or an effective independent sample count. Importing a normalized
reservoir contributes one input with its stored UCW. Multiplying by historical
`M` and using this same normalization would implement a different estimator.
Each local merge accepts at most **32** inputs. Temporal and spatial
passes use two and five inputs respectively; a reservoir's earlier ancestry
does not consume this local budget. A 33rd input fails visibly rather than being
ignored or starting an implicit second merge.

The schedule and `K` must be fixed or exogenous to candidate values. Every valid
scheduled candidate counts, including black `F_i(U) = 0`. Skipping black samples
and dividing by the surviving count changes the expectation. Neighbor selection
must preserve proper weighting conditional on its selection metadata; the helper
cannot establish this for a caller's sampling-dependent rejection policy.

## Shared interface

`rypath_reservoir::State` holds the score sum, selected target, candidate count,
zero-based selected stream index, and a sticky failure status. It holds no path
payload. `no_selection` identifies the empty state.

- `bounded_target_xyz(x, y, z, target_scale)` implements the renderer's bounded
  proxy. XYZ must be finite; scale must be positive, finite and normal. Finite
  subnormal XYZ can affect the proxy differently when a backend flushes terms;
  no exact subnormal L1 agreement is claimed. All returned targets lie in `[1,R]`.
- `target_xyz(x, y, z, epsilon)` retains the earlier diagnostic linear target
  `epsilon + ((abs(x) + abs(y)) + abs(z))`. The caller supplies the support floor;
  there is no default shader floor. Negative XYZ components retain their
  magnitude in this proxy; the helper does not clamp the contribution.
- `prepare_update(state, normalized_weight, target)` validates one input and
  prepares its decision without changing the reservoir. The first valid input
  is immediately complete and selects the candidate with probability one.
- `feed_random_word(pending, word)` consumes an independent uniform 32-bit word,
  most-significant bit first, until the decision is complete. A prepared
  `rare_probability` contains an integer significand and denominator bit count,
  representing `significand * 2^(-bit_count)`. `select_new_on_success` distinguishes
  selecting a smaller new score from retaining a smaller old aggregate.
- `commit_update(state, pending)` commits a complete decision against its
  unchanged source reservoir. The returned `selected` flag controls payload
  copying. Committing an incomplete, failed or stale preparation fails the pool.
- `finalize(state)` returns `S / (K * selected_target)` or an explicit error.
  It reports an empty pool separately from a failed pool.

The caller copies the candidate payload only after a successful commit with
`selected` true. Preparation and word-feed errors are explicit in the pending
result; committing them leaves a sticky reservoir error. The caller must
propagate every error even when it elects not to commit. A failed finalization
also invalidates the output. Error-result `value = 0` is a placeholder, never a
usable zero UCW. Never drop a failed candidate, keep rendering with a partial
pool, clamp a score, or substitute a new path.

The smaller Bernoulli event is sampled directly. When the new candidate has the
larger score, failure of the smaller event selects the new candidate; the code
never rounds `1 - p_small` into a binary32 probability of one. In particular,
a black target of `1e-6` competing with `100` retains its nonzero event in either
candidate order, despite rounding of the score sum. Scaled mantissa division
keeps probabilities with extreme exponent differences from underflowing.

The represented dyadic is compared lexicographically with the random bits.
Equality after its final bit belongs to the failure interval. Its 24-bit
significand and at most 277 denominator bits require at most nine input words;
most decisions terminate much earlier. This is exact sampling of the represented
dyadic from independent uniform words, not a claim that its rounded probability
or the accumulated normalizer equals the ideal real-arithmetic value.

## Support and replay-domain requirements

A strictly positive support floor allows each reservoir to remain properly
weighted for the entire common domain, including states black at its source
pixel but nonblack at a destination. Under these assumptions, equal `1/K`
resampling MIS weights are valid even though source sample distributions differ.
Without full source support, equal weights generally fail; an appropriate
support-aware MIS partition and canonical coverage are required instead.

The proposed normalized replay coordinates include film and lens variates, so
selection can importance-sample the measurement equation over their footprints.
The destination evaluator must include its filter sampling ratio and lens
normalization. Identity replay of normalized random coordinates has unit
Jacobian; it does not imply identity geometry. A lens coordinate can remain
latent for a pinhole camera. Film/camera changes require current reevaluation,
not reuse of cached source radiance.

A finite PRNG key is not literally a continuous, independent random number for
every path dimension. Integration must declare its actual measure. One usable
product-domain design combines a uniform discrete tail key with independently
sampled explicit film/lens/wavelength/path-prefix coordinates. Coordinates later
mutated must already belong to that canonical domain. Adding arbitrary overrides
to a tape derived solely from a finite key does not establish this contract.
The RNG identity must remain distinct from the destination pixel/camera and
output owner. This header implements no replay representation.

Current spectral packets may derive multiple wavelengths from one variate;
integration must preserve that joint law. Destination replay reconstructs the
initial packet and recomputes dispersive lane termination, medium transitions,
eta state and roulette. A source path's final active-lane mask is not destination
initial state. Existing finite layered and homogeneous-medium model limitations
remain limitations of `F_i`; reservoir algebra does not remove them.

The normalized pixel estimator eventually accumulates `w * F_i` with one owner
sample count. Dividing by accumulated UCWs would instead create self-normalized
importance sampling. Authored camera-sample clamping, if preserved, is a separate
integrand, not a clamp applied to a resampled weighted leaf.

## Binary32 limits

The identities above are real-arithmetic statements, not claims of exact
floating-point unbiasedness or universal CPU/GPU bitwise agreement. The shared
implementation deliberately has a narrower, checked arithmetic envelope:

- Weights, targets, scores and final UCWs must be positive, finite normal
  binary32 values. The linear diagnostic helper rejects nonzero subnormal XYZ
  and a subnormal floor; the bounded proxy accepts finite XYZ and a positive
  normal scale. There is no denormal-support claim for physical transport.
- Score multiplication and accumulation must not overflow. The bounded target
  avoids unscaled HDR L1 overflow and uses checked binary32 arithmetic; it is
  not an exact real L1 sum.
- Score-sum rounding can absorb a small input. This is normalizer error; it does
  not eliminate the input's event because the smaller probability is represented
  with a separate exponent. The mantissa ratio also rounds to binary32. No
  contribution or score is clamped to repair this error.
- Counts stop at 32 to bound local numerical error, not merely to keep their
  binary32 conversion exact. A further update reports count overflow without
  wrapping. A numerically exact count does not prevent loss from a long stream
  of small scores absorbed into a much larger sum.
- Finalization separates binary exponents to avoid an unnecessary overflow of
  `K * target` or `S / target`. A nonnormal final result still fails explicitly.

Ordinary binary32 rounding of accepted sums and divisions remains numerical
error relative to the ideal UCW proof. The helper supplies no RNG: the separate
selection stream must provide independent uniform 32-bit words, including any
requested continuation words. Repeating or synthesizing missing words from a
24-bit selector does not satisfy this contract. GPU compiler behavior and the
final resource/error propagation require their own execution validation before
renderer use.

### Conditional local error bound

The following bound assumes correctly rounded binary32 multiplication, addition
and division with relative error at most `u`, exact binary exponent
extraction/scaling when the result is normal, and the independent uniform-word
selection contract. For CPU round-to-nearest, `u = 2^-24`. A conservative value
for either correctly rounded ties-to-even or toward-zero is `u = 2^-23`.

It applies to normal operands/results, a fixed or exogenous pool with
`1 <= K <= 32`, and consistent use of each stored binary32 target. Every possible
selected outcome must have a representable normal final UCW. The pool starts
empty and is advanced through the helper; structural state checks alone cannot
certify its sampling history. Relaxed division, reassociation, malformed states
or failures do not inherit this bound. In particular, conditioning only on
successful selected outcomes when another selection would fail is not covered.

The [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
dated June 4, 2026, Section 8.2 permits ties-to-even or toward-zero. Section 8.4,
Table 8.1 lists correctly rounded non-fast addition, multiplication and division,
zero-ULP `frexp`, and correctly rounded `ldexp`; Table 8.2 separately allows
2.5-ULP fast division. The conservative non-fast bound below is conditional on
the actual backend/version meeting Table 8.1 and preserving the stated operation
sequence. Disabling fast math alone is not evidence of ties-to-even behavior.
Older SDK/language guarantees and actual helper conformance require qualification
before integration; no GPU arithmetic qualification is claimed here.

Let `w_i` and `t_i` denote the supplied floats interpreted as exact real values.
Write `s_i = fl(w_i*t_i)` and `c_j = fl(c_(j-1) + s_j)`. At one update, set
`a = c_(j-1)`, `b = s_j`, `m = min(a,b)` and `L = max(a,b)`. Then

```text
c = (a+b) * (1 + delta_sum),            |delta_sum| <= u
q_small = (m/c) * (1 + delta_div),      |delta_div| <= u
```

The word comparison samples this represented `q_small` exactly. For its smaller
branch, the error relative to `m/c` is at most `u`. For the complementary branch,

```text
(1-q_small) * c/L
    = 1 + delta_sum*(1+m/L) - delta_div*(m/L)
```

Since `m/L <= 1`, either branch can be written as its corresponding `a/c` or
`b/c` factor times `1+e`, with `|e| <= 3u`. Multiplying the selected candidate's
arrival and subsequent retention probabilities telescopes the intermediate
`c_j` factors. There are at most `K-1` such rounded decisions. Its score product
adds one relative rounding factor; finalization adds two divisions. Thus, for
each candidate's selection probability `P_i` and its finalized UCW `W_i`,

```text
(1-3u)^(K-1) * (1-u)^3
    <= (P_i * W_i) / (w_i/K)
    <= (1+3u)^(K-1) * (1+u)^3
```

At `K=32`, the absolute relative coefficient error `eta_32` is less than
`5.723e-6` for CPU round-to-nearest, or `1.145e-5` under the conservative non-fast
Metal assumptions above.
For any fixed vector or scalar test values `h_i`, this yields the local
conditional absolute-error envelope

```text
abs(E[W_selected*h_selected | pool] - (1/K)*sum(w_i*h_i))
    <= eta_32 * (1/K)*sum(abs(w_i*h_i))
```

Apply this componentwise for a vector. Cancellation can make relative error in
the final signed sum arbitrarily large; the bound is relative to the absolute
input sum. The rounded positive XYZ target is a sampling proxy, so its own L1
approximation does not add a target-accuracy term when the same target is used
consistently. These are local numerical bounds relative to the ideal empirical
average, not a claim of exact finite-arithmetic unbiasedness.

### Histories require fresh input

A local bound alone does not qualify an arbitrarily long history. Future
integration must include one fresh weight-one sample in each temporal merge,
with one old normalized reservoir, and a fixed bounded spatial pass afterward.
History-only cycles are not permitted. Correlation does not change the local
coefficient bound, but it still affects variance and convergence evidence.

Here `E` bounds weighted-measure error for every test function by
`abs(E[w*h(U)] - integral h dmu) <= E * integral abs(h) dmu`; it is not a relative
RGB error metric. Ideal temporal averaging with a fresh input halves the
inherited measure error. If the temporal and spatial local upper bounds are
`eta_t` and `eta_s`, including rounding gives

```text
g = (1+eta_t)*(1+eta_s)
E_next <= (g/2)*E_previous + (g-1)
E_steady <= (g-1)/(1-g/2), provided g < 2
```

This assumes the fresh measure is exact in the declared replay-domain model,
valid imports, exogenous selection, consistent targets, and no additional
unaccounted numerical error. Using the 32-input envelope for both passes gives
a steady bound below `2.29e-5` for CPU round-to-nearest, or `4.58e-5` using the
conservative non-fast Metal formula. The two/five-input bounds are
tighter: below `2.504e-6` and `5.007e-6`, respectively.
Other pass counts or fresh/history mixing weights need their own recurrence.
This coefficient-error statement supplies neither a variance bound nor a
physical-model accuracy guarantee, and does not qualify an integrated pipeline
before its sampling, arithmetic and scheduling contracts are tested.

### Realized UCW magnitude bounds

These are bounds on the weight of **every realized outcome**, separate from the
weighted-measure coefficient-error bounds above. They use the target's enforced
range `[1,R]`, one fresh weight-one temporal candidate, immutable spatial inputs,
and correctly normalized two/five-input merges. Let `B` be the maximum UCW over
the previous published history. In real arithmetic,

```text
temporal: W_t <= R * (1+B)/2
after n fixed mutation steps: W_m <= R^n * W_t
spatial: W_new <= R * max_neighbor(W_m)
g = R^(n+2)
B_next <= g * (1+B)/2
```

The spatial inequality holds for all five slots, including repeated clamped
neighbors: an average of bounded incoming weights remains bounded. The mutation
inequality covers the implemented fixed schedule with `n` in `0/1/2/4`.
The original PR4 measurements use `n=0`. No history-only stage or additional resampling pass is implicit here.

If `g<2`, the warm recurrence has fixed point `g/(2-g)`. A cold reset is a
different case: temporal `K=1` returns weight one, so
`B_cold <= R^(n+1)`, not `g/2`. The global bound is the maximum of this cold bound
and the warm fixed point. Freshly created bounded-policy histories satisfy it;
arbitrary imported linear-policy weights do not inherit it. A fresh temporal
candidate also gives a warm lower bound `1/(2*R^(n+2))`, independently of how
small an old weight is. Cold lower bounds are larger.

For finite arithmetic, use the same conditional operation-error assumption `u`
as above. A direct magnitude bound for a `K`-candidate merge follows from one
rounded score multiplication, `K-1` positive additions, and two final divisions:

```text
W_out <= R * mean(incoming_W) * (1+u)^(K+2)
```

This gives factors `(1+u)^4` and `(1+u)^7` for the temporal and spatial stages.
Unlike the earlier coefficient bound, it does not contain selection-probability
errors: the magnitude of a realized finalized weight depends on the sum and
selected target, not on the probability that selected it.

For the implemented actual-forward/reverse dyadic MH correction, a represented
downhill probability is at least its ideal ratio times `1-u`; the accepted UCW
operation adds at most `1+u`. A conservative maximum mutation factor is thus
`R*(1+u)/(1-u)` per step. Including all stages,

```text
G = R^(n+2) * (1+u)^(11+n) / (1-u)^n
B_next <= G * (1+B)/2
B_warm <= G/(2-G), provided G < 2
B_cold <= R^(n+1) * (1+u)^(7+n) / (1-u)^n
W_warm >= (1-u)^(9+2*n) / (2*R^(n+2))
```

The lower bound uses the fresh temporal score of at least one, then the minimum
mutation and spatial factors. With the actual binary32 `R` above and conservative
`u=2^-23`, the resulting outward-rounded bounds are:

| Mutation steps | Cold upper | Global upper | Warm lower |
| ---: | ---: | ---: | ---: |
| 0 (ablation) | 1.100001 | 1.531651 | 0.413222 |
| 1 (default) | 1.210002 | 1.989547 | 0.375656 |
| 2 (ablation) | 1.331002 | 2.732059 | 0.341506 |
| 4 (ablation) | 1.610514 | 7.755236 | 0.282236 |

The common contrast keeps `G` below two for every legal step count. Changing the
schedule, target contrast, mutation correction or arithmetic assumptions requires
a new bound. These limits prevent reuse-induced explosive UCWs; they do not
bound raw physical path contributions, establish finite physical variance, prove
the implementation correct, or predict time to image quality. They are derived
envelopes, not measured GPU outcomes.

## Validation boundary

Independent portable tests should enumerate small uniform domains using a
separate higher-precision or exact probability oracle. Useful cases include
indicator-function UCW expectations for fresh and imported reservoirs, changing
targets between passes, duplicate/correlated inputs, and black inputs with a
positive floor. Negative controls should expose historical-count scaling and
black-sample omission. Deterministic tests should cover threshold boundaries,
selected indices, invalid values, sticky failure and count limits. Dyadic
thresholds need below/equal/above tests, including small events in either
candidate order and extreme exponent gaps. These checks supply evidence for
this helper, not a renderer bias study.

The portable finite-domain and replay-identity tests pass, including the capped
HDR absorption case, malformed random-bit counters and five generations of
correlated reuse. The integrated host and strict Metal shader compile, and the
benchmark argument/provenance tests pass. The linear-policy runtime failure is
retained above.

On the M4 Max, the initial bounded-policy Debug run with Metal API validation
passed the full frame test (42.56 s), CPU/MSL arithmetic probe (0.39 s), queue
reservation probe (0.31 s), and portable bounded-target oracle (0.35 s). Frame
coverage includes camera retracing, epochs and failed-frame recovery, retained
lifetimes, scheduling variants, and 16 independent session-seed comparisons
against native direct Monte Carlo. The unchanged Gaussian/defocus gate now passes,
along with point NEE, an independently witnessed indirect bounce and bounded
scattering/emission cases. Point-lit volume scheduling coverage is separate from
the mean gates because inverse-square point sources can give heavy-tailed volume
estimators. The test records cold/warm/warm observations per seed rather than
treating correlated pixels or history frames as independent samples.

The focused Release suite also passed with Metal API validation: area frame
26.97 s, arithmetic probe 0.23 s and bounded-target oracle 0.21 s. The same three
checks passed under ASan/UBSan in 9.69 s (frame 8.69 s, probe 0.29 s, oracle
0.71 s). These runs used implementation `c918492`, before documentation and
parent-history restacking. Portable arithmetic oracles explicitly disable fast
math and contraction.

These are mean smoke checks and arithmetic/lifetime evidence. Longer
convergence and time-to-quality qualification remain separate; passing a small
mean study does not establish a visual or performance benefit. The separate
[MCMC record](mcmc-mutations.md) describes the subsequent mutation implementation.

Packet replay state is passed explicitly through the live PT sampling helpers,
separately from the resource aggregate containing native acceleration structures.
Offline callers retain null defaults. The resource declaration and complete
non-inlined MIS body match the pre-Area implementation; random addresses, draw
order and numerical operations are unchanged. The replay pointer refers to a
validated thread-local payload for one dispatch and is never persisted in path
state. Scalar replay remains unchanged.

This targets a hosted Xcode 16.4 pipeline-creation failure in
`rypath_probe_canonical_partition`. The earlier `e4c00b8` implementation passes
241 mandatory Debug tests on M4 Max but fails that hosted check; local execution
does not establish older-backend compiler admission. The exact compiler defect
is not diagnosed. Error reports now retain a bounded underlying NSError chain
and distinct failure reasons when the platform supplies them. Hosted compilation
must be checked separately from M4 Max numerical validation.

At `87bcee5`, four focused tests pass in each of Debug, Release and ASan/UBSan
with Metal API validation: light geometry, canonical full context, wavefront
frames and Area frames. Total durations are 130.87 s, 76.18 s and 78.33 s;
there are no skips, failures or retries. Canonical physical/typed mismatch
counts remain zero across eight contexts, 24 MIS cases and 8,800 typed fields.
Independent review checks all five replay-forwarding edges and resource
constructions; token comparisons preserve the original MIS and all non-forwarding
sampler operations. Focused LLVM 18 analysis passes for the error diagnostics.
These are correctness checks, not performance measurements or proof of the
hosted compiler fix.

## Primary references

- Lin et al., [Generalized Resampled Importance Sampling: Foundations of ReSTIR](https://graphics.cs.utah.edu/research/projects/gris/sig22_GRIS.pdf),
  Sections 4-5 on UCWs, supports and resampling MIS, and Section 6 on chained
  reuse. This stage specializes the real-arithmetic contract to one normalized
  domain with full support; it does not copy the paper's optimized path shifts.
- Zhang et al., [Area ReSTIR: Resampling for Real-Time Defocus and Antialiasing](https://graphics.cs.utah.edu/research/projects/area-restir/AreaReSTIR-SIGGRAPH2024.pdf),
  Section 4 on measurement-equation sampling over film/lens footprints. Its
  fractional matching and primary-hit reconnection are not implemented here.
- Sawhney et al., [Decorrelating ReSTIR Samplers via MCMC Mutations](https://arxiv.org/html/2211.00166),
  Section 3 and Equation 16 define the subsequent weight-corrected mutation
  stage. This implementation uses the explicit finite replay-domain adaptation
  in the MCMC record, with no adaptive history cap.
