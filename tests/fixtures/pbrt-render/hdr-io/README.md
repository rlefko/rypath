# Focused linear HDR image I/O references

These six project-authored wrappers target the declared PBRT spectral diffuse
profile. They use 8×8 pixels, 4,096 samples per pixel, independent sampling,
CIE 1931, ISO 100, box filtering, finite depth one and `savefp16=false`.
The shared comparison uses rypath seeds 0–15 and PBRT seeds 16–31. Each full
frame and four 4×4 quadrants has three channel-mean gates, using the established
five combined standard errors plus `2e-5 * max(1, abs(reference))` threshold.
Thresholds and regions are fixed before measurement.

| Fixture | Isolated behavior |
| --- | --- |
| `hdr-half-diffuse` | Signed binary16 source values and above-one reflectance, final diffuse clamping |
| `hdr-float-graph` | Binary32 source above 65,504, scaled scalar imagemap feeding a spectrum mix, NPOT mip filtering |
| `hdr-rgbe-diffuse` | Linear RGBE input, point lookup and authored scale |
| `hdr-area` | Native binary32 emission above 65,504 with a small authored emission scale |
| `hdr-environment` | NPOT square binary32 equal-area environment above 65,504, native source sampling and diffuse reflection |
| `hdr-external` | Selected external ZIP RGBA HALF asset, nonopaque alpha discarded for diffuse lookup, NPOT mips |

All three owned image assets are generated without the production codec.
`scripts/hdr_io_reference.cpp` in its default mode uses installed OpenEXR to
write ZIP HALF and FLOAT EXRs. `scripts/compare_hdr_io.py --generate` regenerates
these assets and writes the explicit old-style RGBE bytes. Regeneration requires
reviewing the sizes and hashes in `assets/manifest.json` and `dependencies.json`.
The project-authored scenes, helper and owned assets use GPL-3.0-only.

`external-comp-zip.exr` is **cache-only** and must not be committed. The selected
asset comes from OpenEXR revision `f723c3940fff287c0a26b425b90a8e764823bfd4`, path
`src/test/OpenEXRTest/comp_zip.exr`: 587×675 RGBA HALF, 1,180,708 bytes, SHA-256
`b51491f94c30896e038ab13286922d75f36f65a42533a8c9acac72b81b1a84a8`.
Its BSD-3-Clause license is retained in `assets/OPENEXR-LICENSE.txt`.
The driver verifies this asset in the pinned PBRT dependency cache, then stages
it into an isolated temporary fixture directory. The dependency manifest
explicitly stages each scene's complete image closure into both backends'
relocated scene directories. This selected asset coverage does not establish
support for the full upstream test corpus.

Run after building the native `rypath_pbrt_reference` and
`rypath_exr_output_tests` targets:

```sh
python3 scripts/compare_hdr_io.py --build build/debug \
  --pbrt-build .cache/pbrt-reference/build \
  --output-dir .cache/hdr-io-reference
```

The driver separately compiles three helper binaries: a product publisher and
source/mip probe, a pinned PBRT source/mip probe, and an installed OpenEXR C++
reader/generator. It reuses the supplied Ninja build's compiler and link flags,
without editing either build graph. The installed SDK remains an oracle-only
dependency. `--skip-renders` runs the deterministic checks without GPU work.

The installed reader verifies 48 independently expected output channel values
across demo/PBRT projection and HALF/FLOAT. These include compensated XYZ means,
exposure, signed values, values above one and FLOAT values above 65,504. It
checks the data window, all RGB channel types, ZIP compression, an output string
attribute and sRGB/D65 chromaticities. It never calls the production decoder.
The PBRT probe compares 380 dimension/channel values for exact decoded source
samples and mips of HALF, FLOAT and RGBE, using repeat and clamp. Source samples
must match exactly; independently rounded mip arithmetic has a predeclared
`3e-6 * max(1, abs(reference))` tolerance. This is separate from the render gates.
