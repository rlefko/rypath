# PBRT sRGB reconstruction validation

This record extends the [spectral rendering validation](pbrt-render-validation.md)
with linear sRGB diffuse reflectance and constant RGB/default-D65 environment
lighting. It covers selected owned scenes and the Cornell parameter inventory,
not general PBRT corpus rendering or a performance claim.

## Contract and acceptance criteria

The physical spectral model gains continuous sigmoid reflectance reconstructed
from the pinned PBRT sRGB table. Numeric knot spectra and the demo are unchanged.
Environment and CIE lookup remain nearest integer nanometer; sampling, PDFs,
MIS, roulette, finite depth, raw XYZ accounting, and PNG conversion are unchanged.
Only compact coefficients or constants reach Metal.

The canonical lookup payload is 9,437,440 bytes, SHA-256
`6adffc41eefcf064b6524606cb8734941aa69ac962ac268fd9276f440cef61d5`.
The [data record](../third_party/pbrt/README.md) documents its source, layout,
extraction and licensing. Offline CMake verifies the input before embedding it.

Pointwise reflectance comparisons to pinned PBRT use absolute tolerance `1e-5`.
Black/white constants are exact. Paired GPU runs compare chromatic diffuse
samples with the same-seed white-material samples multiplied by an independent
continuous-wavelength oracle, using `1e-7 + 2e-5 * abs(white radiance)`.
Independent integrated XYZ cases use 32,768 paths, six analytic standard errors
plus `2e-5 * max(1, abs(reference))`.

The six [fixture entries](../tests/fixtures/pbrt-render/README.md) each use
8×8 pixels, 4,096 spp, and 16 predefined independent seeds per renderer
(rypath 0–15, PBRT 16–31): 25,165,824 camera samples per renderer in total.
The new RGB background isolates tinted-light normalization with HDR, non-unit
scale, and positive illuminance. RGB diffuse isolates reconstruction under
omitted D65 lighting. Mixed-material multiple scattering exercises both spectral
representations in one scene. Every declared region/channel must pass five
combined standard errors plus `2e-5 * max(1, abs(reference))`. All comparisons
use unquantized linear PFM, retaining negative and above-one values.

Thresholds and seeds were fixed before measurements; failures are investigated
without seed rerolls or relaxed thresholds. The pinned CPU reference, dependency
versions and toolchain are documented in the preceding validation record.

## Evidence

Hardware: Apple M4 Max, 16 CPU cores, 40 GPU cores, 128 GB unified memory.
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0
(`clang-2100.1.1.101`), CMake 4.0.3, Ninja 1.13.2. Deployment remains
macOS 14 / MSL 3.1. The system-default device reported architecture
`applegpu_g16s`, unified memory, and compute ray-tracing support. The pinned CPU PBRT executable hash is unchanged from
the preceding reference record.

| Check | Result |
| --- | --- |
| Debug, Release, ASan/UBSan on M4 Max | 48/48 CTests per preset, no skips |
| Metal API validation | 6/6 native CTests, no skips |
| Metal-disabled ASan/UBSan | 44/44 portable/stub CTests |
| LLVM 18 clang-tidy | All 20 C++ sources, including generated data, passed |
| Apple static analyzer | All five Objective-C++ sources passed |
| Policy/PFM tool fixtures | 11 Python tests passed |
| PBRT linear PFM comparisons | All 42 region/channel gates passed |

The 126 chromatic golden samples had maximum absolute reflectance error
`1.19209e-7` against the fixed `1e-5` limit. All 471 extracted D65 samples
matched the pinned CPU reference bit for bit. The paired GPU comparison's
maximum absolute per-path radiance error was `5.59877e-8`; the independent
RGB diffuse XYZ comparison also passed its analytic confidence bounds.

Across the six image fixtures, the worst error-to-bound ratio was approximately
0.6254 (RGB multiple scattering, bottom-right red). The new full-frame means were:

| Scene | rypath linear RGB | PBRT linear RGB |
| --- | --- | --- |
| RGB background | (15.133590, 0.56090642, 4.5268396) | (15.135624, 0.56056168, 4.5298898) |
| RGB diffuse | (0.079639484, 0.70027787, 0.30956032) | (0.079406889, 0.70058057, 0.30982429) |
| RGB multiple scattering | (0.42234006, 0.19275105, 0.064415306) | (0.42154050, 0.19253142, 0.064526964) |

Local data checks reproduced both vendored artifacts, rejected deliberately
changed copies of each input, and regenerated a deleted embedded source using
plain `cmake --build`. The generated C++ is 37,768,643 bytes; the measured
standalone static archive is 9,530,944 bytes. Embedding takes about 4.9 seconds at
configure time; Ninja repeats it once on the first build to register its recovery
rule. Compilation/linking of that target took about 0.5 seconds excluding
generation; unchanged subsequent builds were no-ops (about 0.01 seconds). These
are local build observations, not renderer performance claims.

Independent review checked the pinned color-space rules, photometric calibration,
lookup layout and notices, host/shader ABI, resource ownership and numerical
bounds. It prompted error-precedence regressions for non-sRGB inputs that also
overflow or exceed translation limits. The Cornell scene remains untouched;
all eight material RGB parameters and its HDR emitter RGB are exercised in
isolated supported wrappers, without claiming Cornell lighting support.

Hosted checks, observed hosted GPU availability, CodeQL results, measured CI
duration and final revision identity are recorded on the PR.

## Limits

Neutral constants avoid PBRT float's infinite endpoint coefficients and tiny-gray
intermediate overflow; this numerical adaptation is documented explicitly.
Binary32 reconstruction-scale overflow is rejected even when later normalization
might reduce the value. No HDR clamping is applied. Wider color spaces, area
lights, textures, normals and general external-scene rendering remain unsupported.
The original CC0 Cornell fixture is unchanged and remains a negative compatibility
case. Hosted missing-capability skips are reported separately from mandatory
local M4 Max execution.
