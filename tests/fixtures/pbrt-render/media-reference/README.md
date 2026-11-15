# Owned homogeneous-medium references

Eight original triangle-mesh scenes isolate absorption, emission, isotropic and
forward scattering, nested explicit interfaces, a dielectric boundary, the pinned
Apple preset, and wavelength-dependent coefficients. No external asset is needed.
The preset provenance and exact source RGB coefficients are in `provenance.json`.
All geometry is project-authored under GPL-3.0-only. PBRT preset data retain the
upstream attribution recorded there and in the project dependency record.

The comparison uses the same sources with pinned PBRT v4 `volpath`, a linear CIE
sensor, box filtering, 8 by 8 pixels and 4096 samples per pixel. Sixteen independent
seeds per backend use the predeclared five combined standard errors plus
`2e-5 * max(1, abs(reference))` regional RGB gate. No tone mapping or denoising.
`manifest.json` pins every entry and `dependencies.json` records empty closures.
These wrappers do not establish compatibility with an original external scene.
