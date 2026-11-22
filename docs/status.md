# Project status

Pavilion scene support merged in [PR #50](https://github.com/rlefko/rypath/pull/50).
[PR #51](https://github.com/rlefko/rypath/pull/51) remains a draft while transport,
performance and image-quality closure continue. It is not completed PBRT parity.

The selected scene slice supports spectral packet transport, native geometry,
PT/BDPT connections and splats, raw FLOAT EXR output and separately accumulated
authored film clamping. See the [model record](pavilion-validation.md),
[architecture](architecture.md) and [closure evidence](pavilion-closure-validation.md)
for scope and historical validation.

## Published renderer 26

Revision `9db2b5e` passes all **228 mandatory Debug tests**, **13 focused Release
tests**, the separate CLI raw-output test and **12 independent decoder tests**.
All eight jobs in [CI run 44954199191](https://github.com/rlefko/rypath/actions/runs/44954199191)
pass, including the Foundation Gate. Its generated-path check covers 37 fixtures:
313/313 canonical strategy records agree, versus 33/313 for the inconsistent-density
control. These are bounded correctness checks, not an unbiasedness proof.

The fixed daytime 64-spp pilot measures 6.47094 seconds in render/accumulation
versus frozen fast-v5's 7.09821 seconds, about **8.8% faster**. All five measured
pairs and twelve image controls pass. This is a daytime pilot, not the complete
48-capture performance acceptance or time to a shared quality target. O1 remains
in use; the rejected O2 compilation timeout is retained in the closure history.

The fresh day/night study completed all 384 native jobs and 256 records, retaining
64 runs per renderer per scene. All **36 raw-mean safeguards pass**; **0/48**
simultaneous variance/tail upper bounds pass. One shared bootstrap uncertainty
margin is driven by sparse raw outliers, so this does not mean 48 independent
regressions. Descriptive full-image clamped variance ratios are 1.048 by day and
0.960 by night. Day pool and glass clamped tail ratios are 4.59 and 3.49.
The failed result and all original samples remain retained.

## Current work

Exact GPU replays traced selected extreme contributions through smooth water,
a submerged coated surface, water again and the environment. The observed
throughput and MIS weight do not show an unexplained amplification. A controlled
modified-PBRT replay then confirmed that outer roulette amplified one raw
contribution by about 6.64 times while the authored clamp reduced both versions
to essentially the same value. The subsequent 64-run reference control disables
only outer roulette: the day pool variance ratio falls from 1.323 to 0.998 and
the tail-energy ratio from 4.594 to 0.972. All renders and replay controls pass.
This accounts for the observed pool gap in the fixed population; other regions,
night-time behavior and fresh quality admission remain separate.

The local renderer-27 candidate adds eta-compensated outer roulette to packet PT
and reserves the existing unused continuation variate. BDPT keeps its existing
walk. Contracts are **3/12/27/9/2**; the Vertex remains 1,008 bytes in a 1,024-byte
slot. All 17 focused Release tests pass with Metal API validation, including
native analytic energy, later dispersion and exact scheduling/checkpoint tests.
The separate CLI check and 12 decoder tests pass; selected shallow PT and BDPT
captures match renderer26 exactly. Current CI and fresh scene qualification
remain pending. See [roulette validation](packet-roulette-validation.md) for
the estimator, numerical fallbacks and measured scope.

The specular texture-footprint policy difference remains open and has no measured
variance attribution yet. Fresh candidate quality, complete performance acceptance
and final **1600×850 day-256/night-512** renders remain outstanding. No parity,
convergence or time-to-quality claim follows from the diagnostic replays.

Full pinned-corpus coverage and offline MVP acceptance remain unestablished.
Unsupported geometry/media receive explicit diagnostics; broader materials,
cameras, media and mesh support remain on the [roadmap](roadmap.md).
