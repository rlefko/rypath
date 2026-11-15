# Dielectric validation

This slice targets the smooth and single-scattering anisotropic GGX dielectric
model at PBRT v4 `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. Its input and
transport contract is in the [render profile](pbrt-rendering.md#spectral-ggx-dielectrics).
The [fixture manifest](../tests/fixtures/pbrt-render/dielectric/README.md) identifies
eight owned wrappers and one unchanged, licensed pinned PLY asset. This is
selected asset coverage, not support for the original Sportscar scene or full
external corpus.

## Estimator and numerical contract

Each path retains its scalar sampled wavelength. At a rough interface the
visible-normal proposal uses the existing anisotropic GGX distribution and
height-correlated Smith model. An independent Fresnel choice selects reflection
or refraction. The reflection PDF includes Fresnel probability; the transmission
PDF includes its complementary probability and the refraction Jacobian. Delta
probabilities cancel from sampled throughput; reflection has weight one and
transmission has reciprocal squared relative eta. Rough weights additionally
include `(1 + lambda_o) / (1 + lambda_o + lambda_i)`. The existing fixed 0.75
compensated roulette remains unchanged.

Raw XYZ sums, compensation and counts stay separate from output conversion.
Valid microfacet null events retain earlier contributions. Negative evaluated
roughness and invalid arithmetic reject the pending Film batch. Transmission
throughput is checked immediately after multiplication and after roulette
compensation, so roulette cannot hide overflow. The source review identified
this ordering requirement; a native accumulated-throughput-overflow regression
has not been synthesized. Invalid optical inputs and whole-batch rejection for
invalid roughness are tested.

Checked arithmetic retains PBRT's model, including its smooth/axis thresholds
and grazing distribution cutoff. Critical-angle discriminants preserve authored
eta on both sides: entering uses `eta^2 + c^2 - 1`; exiting uses
`1 - eta^2 + eta^2*c^2`, with floating-point residual compensation. Refraction and
sampled transmission Jacobians rationalize subtraction of nearly equal Snell
cosines. Exiting evaluation forms the positively rescaled half vector
`wi + eta*wo`, avoiding a prematurely rounded reciprocal. Its denominator uses
the length/projection of that compensated half vector.

This remains binary32 geometry and direction transport. Independent review
characterized 32 near-unity cases: continuous evaluation at supplied rounded
directions agrees with double equations within `5e-7` relative, while the PDF
evaluated at a rounded sampled direction can differ from the PDF of the original
microfacet sample. At eta 0.9999/1.0001, measured evaluated/sample ratios span
1.00017–1.00383; at the adjacent floats around one they span 0.00142–0.806.
Direction quantization is comparable to the entire lobe width there. These are
explicit numerical limitations, not passing ordinary sample/PDF consistency
comparisons or a reason to loosen those gates. Eta exactly one has an explicit
delta path. No general near-unity precision guarantee is claimed.

## Independent checks

The Metal probe uses independently written double Fresnel/Snell/GGX equations,
with actual pinned PBRT calls as an additional ordinary-domain oracle:

- 5,144 point/sample cases, including front/back incidence, both lobes, eta below,
  above and near one, exact zero/unity, roughness endpoints, anisotropy, critical
  angles, total internal reflection and grazing cases.
- 4,096 ordinary cases against the pinned binary32 PBRT BxDF.
- 128 adjoint-reciprocity/axis pairs, ordinary sample/evaluate consistency and
  24 invalid/null contracts.
- 393,216 fixed-seed energy/probability samples across three roughness cases,
  compared with independent full-sphere quadrature refined from 4096×4096 to
  8192×8192. Refinement must differ by less than `2e-5`; Monte Carlo agreement
  uses five standard errors plus `2e-4`.

Fresnel tolerance is `2e-6 + 2e-5*abs(reference)`; f, PDF and weight use
`2e-6 + 3e-4*abs(reference)`; sampled direction components use `3e-5` absolute.
Flags and sample identities are exact. These gates preceded measurement.
The single-scattering model is not expected to satisfy a multiple-scattering
white-furnace identity. Independent reviewer checks additionally covered 615
critical-angle float neighbors against long-double optics: zero TIR disagreements
and maximum Fresnel absolute error `1.383e-6`.

Native transport checks isolate entering/exiting eta factors, Fresnel branch
frequencies and exact dedicated lobe words, internal reflection, index matching,
parallel-sided slab factors, reflected/transmitted finite emission, image skies,
rough reflection/transmission NEE, mapped frames, mixed paths, depth cutoffs,
invalid roughness and whole-batch failure. All five sampler configurations check
sample/Film partitioning. Translation, driver and direct Metal boundaries check
glass Sobol 113/114 and legacy 127/128 limits. Direct scene validation requires
authored definition/placement frames for glass. CLI failures preserve an existing
EXR and report source locations before device acquisition.

## Reproduce

The high-resolution quadrature is opt-in, separate from routine CTest:

```sh
python3 - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, 'scripts')
from compare_hdr_io import build_probe
root = Path.cwd()
build_probe(root/'.cache/pbrt-reference/build', 'pbrt',
            root/'scripts/pbrt_dielectric_probe.cpp',
            root/'.cache/pbrt-reference/build/rypath_dielectric_probe',
            'RYPATH_DIELECTRIC_REFERENCE')
PY
build/release/rypath_dielectric_probe build/release/rypath.metallib \
  --extended --pbrt-probe "$PWD/.cache/pbrt-reference/build/rypath_dielectric_probe"
python3 scripts/compare_dielectrics.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --output-dir .cache/dielectric-reference
```

Image gates use 8×8 pixels, 4,096 spp, rypath seeds 0–15 and PBRT seeds 16–31.
Full-image and four quadrant RGB means must agree within five combined standard
errors plus `2e-5*max(1, abs(reference))`. Seeds and thresholds are fixed; all
PFMs, logs and diagnostic outcomes are retained. The roughness-checker comparison
explicitly substitutes 0.01 for its zero endpoint in both backends to avoid the
pinned PBRT cancellation/invalid-sample behavior. Native zero roughness remains
tested. Each override and source/dependency hash is recorded in provenance.

## Revision evidence

Implementation revision is `6bc504d`, based on merged main `8bc47ef`. The validation
host is Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang
21.0.0, CMake 4.0.3 and Ninja 1.13.2. The standalone final BSDF run passes all
checks above in 4.84 seconds; its transcript is `build/dielectric-bsdf-extended.log`.
The integrated Release executable repeats those checks successfully in
`build/dielectric-bsdf-integrated.log`.
Independent review sources are retained in `.cache/dielectric-review/`.
The integrated Debug, Release and ASan/UBSan presets pass 95/95 tests each.
Metal-disabled Apple Clang and LLVM 18 builds pass 72/72 each. All 84 Python
checks, repository hooks, changed-unit Clang-Tidy and native adapter analysis
pass. Initial runs caught the historical test that classified dielectric as
unsupported; that test now uses deferred thin dielectric. Initial static-analysis
findings in the new tests were fixed before the passing reruns.
Logs are retained as `build/dielectric-{debug,release,asan}-tests*.log`,
`build/dielectric-portable-*-tests-final.log`, `build/dielectric-tidy-*.log`,
`build/dielectric-native-analysis.log` and `build/dielectric-python.log`.

All 25 Metal API-validation tests pass at the implementation revision, with no
availability skips (`build/dielectric-metal-api.log`). All four affected
shader-instrumented suites (surface detail, conductor transport, dielectric
transport and dielectric BSDF) pass with `MTL_SHADER_VALIDATION=1` and the normal
CTest deadlines (`build/dielectric-metal-shader.log`). Instrumented conductor
and dielectric transport take 167.37 and 178.08 seconds, respectively, including
pipeline compilation; these are not render-performance measurements.
The eight-scene Release
comparison passes all 120 gates across 256 diagnostic-clean render processes.
Independent PFM decoding recomputes the stored means and all 120 gates exactly.
Artifacts and provenance are retained in
`.cache/dielectric-reference/dielectrics-n7b4nbt_/`; the transcript is
`build/dielectric-reference.log`. Its dirty state consists only of the validation
and profile documentation, recorded in `build/dielectric-artifacts.json`.

The Release reference executable SHA-256 is
`ece0384a9be0814456d87e0244c7d1f19ae83cad2a76e0fd5400a51ab524b9c2`;
the Metal library is
`b521f3cab4bc496257a12f2e2f49a9dec78409cc96968fe21702294130f093b3`.
The pinned PBRT executable is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.
These are correctness measurements, not performance or convergence claims.

## Showcase face-frame correction

The first frosted showcase exposed a scene-authoring seam: the generator used
PBRT's per-triangle default UVs on triangulated polygon faces. Their anisotropy
axes changed by up to 48 degrees across coplanar fan triangles. The generator
now authors one affine planar UV mapping per original face and splits vertices
at actual face boundaries. Independent review confirms identical ordered
triangle positions and winding before and after the correction (28 block
triangles and 8 prism triangles); renderer semantics are unchanged.

Two new regressions independently reconstruct the emitted tangent frames and
check nondegenerate UVs, consistent face directions, geometric edge adjacency,
and outward winding. Both pass and reject the original generator. The full
Python suite now passes 86/86 (`build/dielectric-python-showcase-fix.log`), and
repository hooks pass. A native 240×150, 1,024-spp preview confirms the corrected
frame appearance; it is visual evidence, not a numerical convergence gate.

The first hosted Clang-Tidy run reached the existing ten-minute job deadline
while processing unit 66 of 73, without emitted source diagnostics. CI now
analyzes two independent translation units concurrently. The file selection,
checks and deadline are unchanged; NUL-delimited paths and nonzero analyzer
exit propagation remain intact. The original transcript is retained in
`build/dielectric-ci-tidy-first.log`.
