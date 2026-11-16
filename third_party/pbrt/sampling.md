# PBRT Halton and ZSobol sampling

`src/qmc_shared.h` adapts the Halton radical inverse, digit permutation,
ZSobol Morton index permutation, randomizers and integer hashing from PBRT v4
at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. The original Apache-2.0
notice is retained in `LICENSE.txt`. Sources are `src/pbrt/samplers.h`,
`src/pbrt/samplers.cpp`, `src/pbrt/util/lowdiscrepancy.h`,
`src/pbrt/util/math.h`, `src/pbrt/util/hash.h`, and `src/pbrt/util/primes.cpp`.

`primes.inc` contains the original 1000-entry prime table, with an added notice.
Its SHA-256 is
`a34ecb94f6f5c678cc4ab632e06b2ed3e7481f28adfd1d40967809c94abf7c4e`.
The original `primes.cpp` SHA-256 is
`a72428786b36d30b68b901f91ca671880f8d6d77437bb24c3be31def47c20d92`.

The product does not link PBRT. Permuted Halton digits are computed on demand
using the original seed/digit hash and permutation rather than storing the
original per-seed permutation arrays. The mathematical Halton sample index uses
checked 64-bit arithmetic, avoiding upstream's signed-int multiplication
intermediate. Prime dimensions never wrap. Pixel variates are unrandomized;
Halton `none` and `owen` ignore the seed, following the pinned implementation.

The transport dimension allocation is rypath's, not PBRT's integrator ordering.
Halton uses primary dimensions 0/1, wavelength 2, and transport dimensions
`4+8b..11+8b`. Lens dimensions are 998/999. Without dielectrics the depth limit
is 124; with dielectrics it is 110 and lobe dimensions are `884+b`.
ZSobol uses wavelength dimension 0 and the primary pair starting at dimension 1,
matching PBRT's wavelength-before-camera ordering. Its transport allocation and
extension tails follow rypath's existing Sobol allocation. Scalar calls hash the
advanced dimension `d+1`; pairs share a shuffled index and hash `d+2`, using the
low/high hash words for axes zero/one. Effective target spp is immutable for a
sequence and is required even for untextured requests.

`scripts/pbrt_sampling_probe.cpp` is an independent adapter to the separately
built pinned library. The 28 checked-in known answers in
`tests/qmc_sampling_tests.cpp` record its binary32 output bits and ZSobol indices.
Routine tests require no network or reference renderer.
