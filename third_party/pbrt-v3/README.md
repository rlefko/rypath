# PBRT v3 Fourier interpolation

`shaders/table_bsdf.h` adapts the nonuniform Catmull–Rom weights, conditional
zenith-angle inversion, cosine Fourier series inversion, angular RGB evaluation,
and radiance-mode transmission scaling from PBRT v3 revision
[`13d871faae88233b327d04cda24022b8bb0093ee`](https://github.com/mmp/pbrt-v3/tree/13d871faae88233b327d04cda24022b8bb0093ee):

- `src/core/interpolation.cpp`
- `src/core/reflection.cpp`
- `src/materials/fourier.cpp`

The original BSD terms and copyright are retained in [LICENSE.txt](LICENSE.txt).
The bounded reader, packed relative-offset representation and Metal adaptation
are rypath changes. Fourier RGB is reconstructed after angular evaluation and
passed to the renderer's unbounded sRGB spectral reconstruction. A one-channel
file is achromatic. Inverse CDF residuals are normalized by density amplitude: the upstream absolute
`1e-6` stopping rule can collapse samples of valid dim tables. Tests compare
independent double CDF integrals and unmodified upstream angular values/PDFs
at identical directions, rather than requiring identical legacy solver directions.
Canonical tables retain finite data and reject malformed
ranges, decreasing CDFs and unsupported basis/channel formats. A duplicated
zero zenith knot represents the two sides of the interface; inactive rows may
have zero probability mass. Optional trailing SCATFUN metadata is inert.

The measured-tensor mappings in that header follow the separately attributed
[PBRT v4](../pbrt/README.md) Apache-2.0 source, specifically `src/pbrt/bxdfs.cpp`
and `src/pbrt/util/sampling.h` at
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. The implementation computes equivalent
piecewise bilinear integrals directly, uses stable polar angles near the normal,
and bounds every input allocation. No external material dataset is vendored here.
