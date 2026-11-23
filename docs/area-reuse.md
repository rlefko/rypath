# Area-domain reservoir math

This stage adds only the CPU/MSL-compatible streaming selection helper in
[`reservoir_shared.h`](../src/reservoir_shared.h). Renderer integration, replay
storage, film/lens and indirect-path reuse, temporal/spatial scheduling, and GPU
execution are pending. MCMC is a separate subsequent stage. No current renderer,
physical model, sampler stream, Film plane, or checkpoint contract changes here.

The intended integration targets the complete camera estimator over film, lens,
wavelength and transport random variables. It is not direct-light sampling
relabeled as Area ReSTIR. Full replay is the correctness baseline; it retraces
the complete destination path and does not yet amortize path suffix evaluation.

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
Each local merge accepts at most **32** inputs. Planned temporal and spatial
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

- `target_xyz(x, y, z, epsilon)` returns a checked target
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
  binary32 values. Nonzero subnormal XYZ inputs and a subnormal
  floor are rejected. There is no denormal-support claim on Metal devices.
- XYZ target additions and score multiplication must not overflow. The target
  proxy uses the stated binary32 addition order; it is not an exact real L1 sum.
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
conservative non-fast Metal formula. The planned two/five-input bounds are
tighter: below `2.504e-6` and `5.007e-6`, respectively.
Other pass counts or fresh/history mixing weights need their own recurrence.
This coefficient-error statement supplies neither a variance bound nor a
physical-model accuracy guarantee, and does not qualify an integrated pipeline
before its sampling, arithmetic and scheduling contracts are tested.

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

No build or GPU validation was run while introducing the header and this record.
Film/lens/path replay, temporal/spatial integration, MCMC, shader execution and
time-to-quality measurements are pending and must be reported separately.

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
  stage. This PR implements no mutation or adaptive history cap.
