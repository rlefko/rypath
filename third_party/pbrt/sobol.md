# PBRT v4 Sobol sampling

The checked-in [Sobol tables](sobol_tables.inc) transcribe the 1,024 by 52
direction words, the 25 forward interval rows, and the 26 inverse interval
rows from [sobolmatrices.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/sobolmatrices.cpp)
at PBRT v4 revision `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. Each interval
row is padded with zeros to 52 elements and the rows are flattened for the
host/Metal buffer layout. The normal build verifies the include's SHA-256
`349a336cd9dff8bdc06cd30490b1cf887c3f041c5fd00ff3bba5ea9e7f38aeab` and
compiles the data without PBRT or network dependencies. The unmodified
upstream `sobolmatrices.cpp` has SHA-256
`98be104d68f0a90b4043e457b4288d7fb5b3650aae4ec60d72edc07a8435fef6`.

The matrices are based on S. Joe and F. Y. Kuo,
[Constructing Sobol sequences with better two-dimensional projections](https://doi.org/10.1137/070709359).
The include retains the full Leonhard Gruenschloss copyright and permission
notice. The data and adapted routines retain the PBRT copyright, 1998–2020
Matt Pharr, Wenzel Jakob, and Greg Humphreys, and the Apache License, Version
2.0. See [LICENSE.txt](LICENSE.txt).

The shared CPU/Metal implementation in `src/sobol_shared.h` adapts the pinned
[low-discrepancy routines](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/lowdiscrepancy.h),
[32-bit reversal](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/math.h),
[hash routines](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/hash.h),
and [camera remapping](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/samplers.h).
It specializes MurmurHash64A to the eight-byte little-endian encoding of
the dimension and signed seed, uses the same binary32 conversion and clamps
as PBRT, and supports the fixed validated rypath render dimension layout.
