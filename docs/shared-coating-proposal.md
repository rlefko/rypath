# Shared coating proposal policy

The retained original Pavilion-night highlight trace identifies a
nondispersive coated diffuse surface that unnecessarily loses three wavelength
lanes under the current hero fallback. This experiment changes the sampling law,
not authored materials, finite coating depth, illumination, exposure, raw
accumulation, reconstruction or history policy.

`--coating-proposal shared-diffuse` explicitly selects the policy for frame
benchmark sessions. `hero-only` remains the default. The frame API uses
`FrameCoatingProposal` and `FrameConfig::coating_proposal`. Admission requires
explicit vacuum PBRT hero transport, packed one-event wavefront execution, and
either independent or Enhanced Area frames. Enhanced full/fresh extraction and
full/prefix evaluation retain their existing exact replay contracts. Other
configurations fail before rendering.

Function constant **32** declares the selected shade kernel's capability v1.
The host verifies that declaration before supplying the opt-in constant and
building the pipeline; an older library rejects the request. Unspecified
constant32 means the legacy law, including offline rendering. Sessions keep their
own specialization and history resources even when a test shares an acceleration
structure. Offline sampling and checkpoint identities are unchanged.

Completed `FrameInfo` records distinguish requested/effective policy, capability
verification and contract version. Benchmark settings, per-frame timing records,
raw EXR and reconstruction EXR metadata retain actual execution provenance.
`physical_source_identity` retains the unchanged physical render identity; an
admitted shared policy derives a separate sampling `source_identity`. A failed
admission never claims an effective shared policy. The source label is
`legacy-default` or `explicit-frame-session-v1`.

Physical reference, motion, tail and complete-pipeline cost gates remain required
before any default adoption. A local variance result alone is insufficient.
Coated conductors and unsupported coated diffuse layers retain the existing
compensated hero-only fallback.

## Restricted common proposal

All active evaluated lanes must have exactly equal prepared interface eta,
roughness, smoothness, thickness, g, internal depth/sample count and material
seed. Every lane must be a valid opaque coated diffuse layer with zero medium
albedo. Diffuse reflectance may differ, including a black hero and nonblack
companions. Equality is evaluated on the actual sampled packet; a different
cyclic hero uses the same set of wavelengths and therefore the same test.

The existing packet coating sampler draws a shared hidden walk. Its diffuse-base
proposal uses unit reflectance, so a black lane cannot discard a direction needed
by another lane. The top interface and Beer attenuation share geometric controls.
Its internal roulette uses the maximum active vector weight, and compensates all
lanes by the common survival probability. The proposal is invariant under
permuting the active lanes, including the roulette stopping law. It may survive a
walk that the scalar hero would terminate; that is a different proposal, not a
change to the declared finite-depth target.

If incoming hero likelihoods are P[k], with S=sum(P), throughput is represented
as beta[j]=4 F[j]/S. The common hidden proposal Q multiplies every P[k] equally.
Thus its common likelihood factor cancels from relative hero likelihoods and
subsequent balance-MIS ratios. The sampler returns the actual joint-walk
f*cos/Q weight for each lane, including internal-roulette compensation, and each
beta[j] is multiplied by its own weight. It does not divide each lane by an
unavailable directional marginal or reset a nonuniform incoming hero posterior.

NEE evaluation uses the existing packet finite-depth coating f estimator. Its
common represented directional MIS proxy is broadcast only to active lanes;
that proxy is not substituted for the sampled joint-walk density. Full and
compact hero paths use the same restricted law. Compact/frozen storage retains
three companion reflectances and a proof tag in previously unused payload words;
the existing 104-byte material and path-state sizes do not grow. Unsupported
coatings still collapse using 1/posterior(hero), preserving their original support.

Enhanced interior charts still reject coating roots. The sampler remains a
fallback joint operation, so coating continuation is not recorded as a reusable
ordinary suffix operation. No wavelength-dependent suffix radiance is treated as
unchanged after mutation.

## Bounded measurements

The first M4 Max probe compares shared packet weights with four separate scalar
walks at the original grazing angle and normal incidence. It includes positive
spectra and a black-hero spectrum, eight seed clusters and 32,768 paired walks
per seed/case. All **16 predeclared lane mean gates pass**; each permits five
cluster standard errors plus 0.5% of the pooled mean and 1e-7. No sample is
excluded or rerun for acceptance. The retained four populations contain
1,048,576 paired shared/scalar evaluations.

Under fixed warm illumination and equal hero posterior, observed local XYZ
variance ratios are **0.256–0.299** and opponent-chroma ratios **0.016–0.076**.
For posterior (16,8,2,1)/27 they are **0.064–0.111** and **0.0044–0.020**.
The old collapse's hero choice is integrated analytically over all four
probabilities. These describe a local BSDF experiment with hypothetical companion
reflectances; they do not include scene visibility, resampling, reconstruction,
reference uncertainty or native frame cost.

The subsequent actual-material population uses prepared reflectances
(0.52414638, 0.56152672, 0.41252500, 0.48088554), plus a separate black-hero
stress spectrum. Both the sampled hidden walk and cosine-light NEE with ordinary
complementary proxy MIS pass all **32 predeclared lane mean gates** across the
same eight seed clusters and two angles. The actual positive spectrum's local
equal-posterior XYZ variance ratios are 0.248–0.251 for sampling and 0.097–0.138
for NEE; opponent-chroma ratios are 0.007–0.013 and 0.009–0.010 respectively.
These remain fixed-illumination BSDF measurements, with hero choice integrated
analytically, rather than scene-convergence results. The retained protocol and
reductions are `actual-probe-protocol.json`, `actual-sample-analysis.json` and
`actual-nee-analysis.json`.

The experiment retains 10,704–18,622 lane outcomes per case where the common walk
survives and the corresponding scalar walk is zero. With positive reflectance and
shared geometric/RNG controls, differing compensated internal roulette supplies
these complementary outcomes; preserving scalar stopping decisions is not
required for equality of the means.

At policy revision `14c2ed1`, eight strict native controls pass on the owned
16x16 coating fixture: old/default/explicit-off raw and signed pixels are exact,
the opt-in changes the proposal, Enhanced full/fresh sessions complete, and the
older selected kernel rejects opt-in. All six current-host runs also pass the
completed timing-record and raw/reconstruction source-identity audit. These are
API and provenance controls, not a scene-quality qualification.

The first matched 320x170 Pavilion-night independent tuning run uses seed 702201,
eight submitted frames and the frozen physical 2048-SPP reference. Raw last-fresh
RGB MSE is 0.32852 for the baseline and 0.36409 for shared coating; final signed
RGB MSE is 0.28788 and 0.12576. The raw regression and signed improvement are both
retained. One tuning seed at equal frame count does not establish a speed or
quality decision. Area, additional seeds, other scenes, motion and held-out
comparisons remain separate gates.

The first 64-case full/current probe failed eight partial-mask cases because its
path mask remained 15 while its material payload was prepared for mask 1 or 3.
Component captures show bit-identical values on every originally prepared lane;
the mismatches arise on reactivated unprepared lanes. Production callers pass
the path mask through preparation, and imported expansions prepare the needed
superset first. Failed receipts remain retained. A corrected 96-case probe adds
noncontiguous masks 5 and 9; its hardware result is pending. The shader-only removal
of duplicate preparation validation at `43e5a46` has a separately frozen runtime
and pending critical controls; previously measured binaries are unchanged.

Evidence is outside Git in `.cache/highlight-origin-001/`: the predeclared
`shared-probe-protocol.json`, four FLOAT populations, `shared-probe-analysis.json`,
commands and completion logs. The no-diagnostic runtime is separately bound in
`nodiagnostic-final/`. The original illustrative native trace is retained beside
these artifacts. These measurements do not establish default adoption or scene
acceptance.

The local tests are [the scalar/shared population probe](../tests/shared_coating_probe.metal),
[the full/current/frozen contract probe](../tests/shared_coating_current_probe.metal)
and [the independent reduction](../scripts/analyze_shared_coating_probe.py).
Their runners require actual Metal and fail when no device is available.
`frame.coating_policy` exercises parameter and actual-receipt admission.
`metal.frame_coating_sessions` exercises both policy construction orders on a
shared immutable renderer with separate histories and owned records.
`tests/frame_coating_policy_native.py` additionally compares default/off pixels
with a frozen matched legacy executable/library, tests an actual proposal change,
checks raw/reconstruction provenance and rejects opt-in against the old library.
Its caller must hold the project's GPU lease and provide fresh output paths.
Primary foundations remain [hero wavelength sampling](hero-wavelength.md), the
[pinned PBRT layered model](https://pbr-book.org/4ed/Reflection_Models/Layered_BxDFs)
and the existing packet coating model/limitations in [Pavilion validation](pavilion-validation.md).
