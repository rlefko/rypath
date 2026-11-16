# Bounded fog numerical validation

## Numerical contract

The bounded fog demonstration exposed a valid null-interface crossing rejected
by a positive projected-displacement guard. Native intersection distance was
`1.67055617e-7`, but reconstructing and normal-offsetting the hit moved its
origin by `-1.14984331e-7` along the old ray. The origin nevertheless moved onto
the correct side of the interface. This failure predates spot lighting.

Null crossings now anchor the departure at the ray-evaluated intersection.
The normal-offset allowance includes the native-hit reconstruction error, the
disagreement between reconstructed and ray-evaluated points, and ray arithmetic.
Rounded normal length is accounted for, and coordinates are rounded outward.
A separate certificate compares the departure against the reconstructed
surface's uncertainty slab, including subtraction and dot-product rounding.
Nonnegative native hit distances, including a hit at the origin, are supported
only when the new origin changes representably and passes this certificate.

This is a numerical adaptation of the existing error-bounded ray offset, based
on [PBRT's rounding-error treatment](https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error).
It changes neither the scattering model nor PDFs, sample domains, depth rules,
or Russian roulette. Native traversal uncertainty remains an empirical allowance;
arbitrary coincident shells, tiny gaps, or extreme scene scales are not guaranteed.

Finite medium connections retain their original excluded endpoint. Each null
crossing rebuilds the direction and distance toward that same endpoint. The old
projected-distance subtraction shifted it sideways after a normal offset.
Infinite connections keep their direction and infinite extent. The outgoing
side must be numerically resolved both before and after finite re-aiming.
Unresolved side/endpoint arithmetic and boundary exhaustion fail the pending
batch without committing samples; they are not silently clamped or skipped.

Renderer checkpoint contract 5 rejects continuation under the older numerical
policy. Identity schema 4, sampling, accumulation and file format are unchanged.

## Reproduction and acceptance

The owned `bounded-fog/point-boundary.pbrt` fixture reduces the original failure
to twelve boundary triangles and lighting. The native regression retains the
original 768×512 camera, Sobol seed 17, target 512 spp, absolute pixel 45101
(`557,58`) and sample 41. The parent backend and library reject that single
sample; the candidate completes it. The ordinary medium suite also exercises
Beer attenuation, both boundary sides, shared edges, affine instances, finite
light endpoints, sample partitioning and atomic failure handling.

The original larger scenes remain reproducible without external assets:

```sh
python3 scripts/generate_spot_samples.py --output-dir build/fog-samples
build/release/rypath --render-pbrt build/fog-samples/bounded-fog-beams.pbrt \
  --output build/fog-samples/bounded-fog-beams.exr \
  --checkpoint build/fog-samples/bounded-fog-beams.checkpoint
build/release/rypath --render-pbrt build/fog-samples/bounded-fog-points.pbrt \
  --output build/fog-samples/bounded-fog-points.exr \
  --checkpoint build/fog-samples/bounded-fog-points.checkpoint
```

## Measured results

Implementation `bc4dacc` was validated on Apple M4 Max, macOS 26.6
(25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0, CMake 4.0.3 and
Ninja 1.13.2. Debug, Release and ASan/UBSan each pass 167/167 CTests.
All 40 Metal tests pass with API validation and no availability skips.
Metal-disabled LLVM 18.1.8 passes 129/129; all 119 Python tests, changed-unit
Clang-Tidy, native adapter analysis and repository hooks pass.

The reduced single-sample regression fails on the preserved parent backend
and library, and passes with this implementation. A separate actual shader
mutation retains the new offset helper but restores projected-distance
subtraction. It fails the oblique endpoint test: the mutated connection returns
zero instead of the unobstructed radiance `0.057015`. The fixture's blocker is
adjacent to the fixed endpoint, so an axial-only cutoff check cannot pass in its
place. The certified helper supports nonnegative native hits, including zero;
it does not accept unchanged or uncertified origins.

The point and spot volume fixtures each pass all 15 pinned PBRT region/channel
gates, using 16 independent seeds per backend and the predeclared thresholds.
Both original 768×512 bounded demonstrations complete at 512 spp, or
201,326,592 samples each. Raw FLOAT EXRs and completed checkpoints are retained;
the PNG previews are exported from those checkpoints without new sampling or
denoising. These are correctness demonstrations, not performance benchmarks.
