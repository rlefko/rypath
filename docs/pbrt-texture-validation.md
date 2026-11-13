# PBRT constant texture validation

This slice translates named constant textures into the existing diffuse spectral
material representation. It changes scene ingestion and binding, not the physical
model, estimator, shader ABI, random variables, PDFs, roulette, or raw accumulation.
The [render profile](pbrt-rendering.md#constant-textures) defines supported values;
the [structural profile](pbrt-profile.md#constant-texture-definitions) distinguishes
checked declarations from renderable values and consumers.

## Semantic contract

The reference is PBRT v4 at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`:

- [Constant factories](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/textures.cpp)
  obtain a scalar or spectrum `value`, defaulting to one. A constant cannot alias
  another texture through `texture value`, and a float is not coerced to a spectrum.
- [Scene construction](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/scene.cpp)
  uses distinct global float and spectrum namespaces and constructs all textures
  before materials. Definitions survive scopes; material forward references work.
- [Spectrum extraction](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/paramdict.cpp)
  uses the texture declaration's color space for explicit RGB and the captured
  attribute color space for inherited RGB. The consuming material does not recolor
  a named texture. Direct diffuse RGB retains its existing material-dictionary rule.

Preflight owns the selected parameters and their locations. Texture definitions
and diffuse material references are validated even when unused. Translation
compiles each spectrum definition once and reuses its material index across
inline/named materials and object instances. Omitted diffuse reflectance remains
0.5; omitted texture value is an exact constant 1, independent of color space.

RGB reflectance components must lie in [0,1]. Numeric spectra retain the existing
at-least-two-knot, binary32, increasing-wavelength and [0,1] rendering limits;
there is no new clamping. Float constants may be signed but must narrow to finite
binary32. Alpha and displacement consumers, image/procedural textures, other
spectral representations and textured emission remain unsupported.

The profile retains byte-exact identifiers and its existing last-compatible
parameter selection. Mixed explicit types can therefore be accepted where pinned
PBRT reports the losing value as unused. This syntax extension is documented;
the upstream comparison fixtures use upstream-valid parameter combinations.

## Permanent checks

Portable tests cover typed namespaces, duplicate/empty/missing/wrong-channel
references, forward and scoped Include bindings, parameter ownership, definition
defaults, explicit/inherited RGB provenance, fractional spectral knots, zero
outside spectral support, and eager rejection of unused unsupported values.
Errors retain the selected value's source location and withhold the render scene.
After an unread Include, only effective explicit input paths are inventoried;
shadowed file values and unknown inherited state create no false dependencies.

A 20-knot texture shared by three separately declared materials succeeds with a
40-value translation budget and uses one payload. A 39-value total or list budget
fails at the texture value. Separate preflight tests bound repeated inherited
validation work and retain unsupported diagnostics for images, alpha and displacement.

Two owned fixtures reuse existing direct-material controls:

| Texture fixture | Direct control | Coverage |
| --- | --- | --- |
| `texture-diffuse.pbrt` | `diffuse.pbrt` | Named material forward reference and sampled spectrum with a 503.5 nm knot |
| `texture-ply-external.pbrt` | `ply-external.pbrt` | Inherited sRGB value under a Rec.2020 texture declaration; attributed bilinear PLY |

The second wrapper uses the unchanged pinned 412-byte Sportscar asset with its
CC BY 2.0 attribution. The [fixture manifest](../tests/fixtures/pbrt-render/README.md)
and [asset record](../tests/fixtures/pbrt-render/ply-assets/README.md) retain hashes,
dependency closure and licenses. This establishes selected asset coverage, not
support for the upstream Sportscar scene or general texture/corpus compatibility.

`metal.pbrt_render` first compares the production spectral payloads, then requires
exact same-device raw compensated film and sample-count equality at 8x8, 64 spp,
32-sample batches and seed `0x1975bd6a6ea321cf`. A black-reflectance control must
change each image, proving the equality check exercises reflection. Exact equality
is appropriate for this shared execution path; it is not a CPU/GPU identity claim.

Independent PBRT comparisons keep 8x8, 4,096 spp and 16 seeds per renderer
(rypath 0-15; PBRT 16-31). Each declared region/channel must satisfy
`abs(delta) <= 5 * combined_SE + 2e-5 * max(1, abs(reference))`.
The two wrappers add 18 gates without changing prior seeds or thresholds.

## Measured evidence

Measured November 13, 2026 for implementation `b6d61b9` and the accompanying
validation fixtures in this PR, based on merged PLY revision `f0c6f66`.
Device: Apple M4 Max, `applegpu_g16s`; macOS 26.6 (25G5065a), Xcode 26.6
(17F113), CMake 4.0.3, Ninja 1.13.2. Both Metal availability allowances were off.

| Check | Result |
| --- | --- |
| Strict M4 Max Debug, Release, ASan/UBSan | 64/64 CTests each, no skips |
| Metal API and shader validation | 12/12 native CTests, no skips |
| Metal-disabled ASan/UBSan | 54/54 CTests |
| LLVM 18 clang-tidy | All five changed C++ units passed |
| Python policy/comparison tests | 25/25 passed |
| Pinned CPU PBRT comparison | 285/285 gates across 736 linear PFM outputs |
| Texture/direct PFM equality | All 64 same-backend, same-seed pairs were byte-identical |

The largest new gate error divided by its acceptance limit was 0.13813 for the
sampled fixture and 0.362 for the PLY wrapper. Independent code review and
repository hooks passed. Hosted Linux/macOS and compiled CodeQL checks will run
on the PR's final revision and be reported there. Existing milestone records
remain historical evidence. No performance improvement is claimed.

## Reproduction

Run the supported configure/build/test presets for `debug`, `release` and
`asan-ubsan`, with both Metal availability allowances disabled. Then run:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug \
  --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir .cache/pbrt-reference/texture-final
```

The opt-in CPU Release/float PBRT build uses the existing pinned source/submodules
and configuration from [render validation](pbrt-render-validation.md). Its binary
SHA-256 is `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.
Native tests and the rypath comparison executable require real GPU access;
a sandbox reporting no device is not a passing hardware check.
