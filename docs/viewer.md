# Native pipeline viewer

The macOS `rypath-viewer` executable presents completed GPU frames through AppKit
and a CAMetalLayer. Its default is native Metal intersections, wavefront PT,
independent sampling and signed temporal reconstruction at native resolution.
Area-domain full replay with corrected MCMC mutations and MetalFX remain explicit
options. The first version has static scenes and camera interaction; geometry
animation and frame interpolation are not implemented.

```sh
cmake --preset release
cmake --build --preset release --target rypath_viewer
build/release/rypath-viewer.app/Contents/MacOS/rypath-viewer --scene tests/fixtures/pbrt-render/cornell-box-adapted.pbrt
```

The target is built on macOS with the Metal backend enabled. It requires full
Xcode with working Metal tools and a device meeting the renderer's compute ray
tracing requirements; see [Metal execution](metal-execution.md). The backend's
minimum deployment target is macOS 14. MetalFX denoising additionally requires
an SDK exposing its temporal denoiser API, macOS 26 or newer, and device support.
Signed temporal display requires float32 texture filtering, checked at runtime.
Requested capabilities that are unavailable produce a visible diagnostic rather
than selecting another mode.

Opening `build/release/rypath-viewer.app` without arguments selects the built-in
scene. From the command line, choose `--scene PATH` or `--demo`; `--help` lists
options. PBRT scenes must pass the existing preflight and render translation.

| Option | Behavior |
| --- | --- |
| `--width W --height H` | Fixed internal and reconstruction resolution; defaults to 1920×1080 |
| `--mode temporal\|metalfx\|raw` | Explicit output mode; temporal is the default |
| `--estimator area-replay\|independent` | Independent by default; Area replay selects the experimental reuse pipeline |
| `--area-path-domain summed\|contribution\|enhanced` | Opt-in contribution/Enhanced representation; summed remains default |
| `--area-contribution-replay full\|prefix` | Full or selected-prefix replay in contribution domains |
| `--area-stencil auto\|adjacent\|reciprocal16\|paired-gaussian16` | Explicit spatial source domains |
| `--area-spatial-mode all\|stochastic-pairs\|stochastic-shared` | All four spatial terms by default; opt-in independent or shared stochastic terms require Enhanced paired atlas |
| `--area-history-cap N` | Fixed Enhanced prior-frame confidence cap, 1–20; default 20 |
| `--resource-profile standard\|large` | Existing PBRT scene loading budgets; Pavilion requires large |
| `--variance-policy paired\|center\|conservative` | Temporal reconstruction policy; paired remains default |
| `--mutations 0\|1\|2\|4` | Area replay uses one corrected mutation by default; independent uses zero |
| `--frames N` | Exit after N completed pipeline frames; omitted means unlimited |
| `--seed N` | Render seed, default 1 |
| `--exposure EV` | Display EV, default 0; range −24 to +24 |
| `--near-plane D` | Positive normal-float guide near plane, default .001; raw rays are unchanged |
| `--capture-dir DIR` | Parent for requested captures; defaults to `Pictures/rypath` |

The full [golden pipeline candidate](golden-pipeline.md) is selected explicitly:

```sh
build/release/rypath-viewer.app/Contents/MacOS/rypath-viewer --demo \
  --estimator area-replay --mutations 1 --mode temporal
```

The bounded Enhanced controls are also available explicitly, for example:

```sh
build/release/rypath-viewer.app/Contents/MacOS/rypath-viewer \
  --scene tests/fixtures/pbrt-render/cornell-box-adapted.pbrt \
  --estimator area-replay --spectral-transport hero --hero-wavelengths visible \
  --area-path-domain enhanced --area-contribution-replay prefix \
  --area-stencil paired-gaussian16 --area-spatial-mode stochastic-shared \
  --area-history-cap 20 --mutations 0 --variance-policy conservative
```

Use `--resource-profile large` for Pavilion's existing scene-loading limits.
The controls preserve the renderer's explicit unsupported-combination diagnostics;
no preset or automatic quality/performance selection is introduced. Stochastic
policies and confidence caps change the estimator, so capture metadata records
the actual path domain, replay policy, spatial mode/atlas IDs and fixed cap.
These experimental paths remain subject to the [Enhanced validation scope](restir-enhanced.md).

`--reconstruction temporal|metalfx|raw-rgb` is the alternate mode spelling;
specify the mode only once. Dimensions must be positive and no greater than
16,384 per axis, with additional allocation budgets enforced. Resizing or entering
fullscreen letterboxes the fixed-resolution image; it does not increase render
resolution or enable upscaling. `--frames` counts completed raw-plus-reconstruction
work, and a pipeline failure returns failure in this bounded mode.

Temporal mode keeps signed RGB in RGBA32Float storage and uses pre-spatial history
plus spatial filtering. It is an approximate reconstruction, not raw Monte Carlo
accumulation. MetalFX uses its separate presentation input and output. Raw mode is
an unfiltered RGB conversion preview with presentation range clamps; the raw EXR
capture below preserves the underlying unexposed estimate. Denoiser errors pause
with a diagnostic; there is no silent MetalFX or temporal substitution.

## Controls and output

- WASD moves forward/back and left/right; Q moves down and E moves up.
  Shift increases speed.
- Drag the mouse or use arrow keys to rotate; Home restores the authored camera.
- R compares raw RGB with the selected denoiser for subsequent frames. Temporal
  and MetalFX selections are remembered across toggles. An initially raw viewer
  uses temporal as its comparison denoiser.
- Plus/minus changes display exposure by 0.5 EV; equals also increases it.
  This never edits raw radiance.
- Cmd+S saves a fresh directory containing paired raw and reconstructed FLOAT EXRs.
- Escape closes the viewer; the View menu provides fullscreen.

Capture directories are created only on request under `Pictures/rypath` by default.
`--capture-dir` selects a different parent. The raw EXR contains the unexposed
linear-RGB conversion of retained raw XYZ. The reconstructed EXR directly stores
the separate presentation RGB, with
mode, sample, epoch, estimator and mutation metadata. Temporal captures are
`temporal.exr`, tagged as signed temporal denoised RGB from RGBA32Float storage;
MetalFX uses `denoised.exr`, and raw preview uses `raw-rgb.exr`. All are FLOAT EXRs.
Temporal captures preserve negative and high finite RGB values. MetalFX and raw
preview retain their presentation range clamps. Display tone mapping and EV are
not baked into either capture. The display clamps negatives for presentation,
applies an exposure-dependent Reinhard map without overflowing finite HDR through
an exposure multiplication, and performs one sRGB transfer. The layer is tagged
with the sRGB color space for
color matching on wide-gamut displays. This is an SDR preview, not an HDR display
output mode.

PBRT preflight and translation remain mandatory. The viewer logs authored
settings and its explicit live overrides: PT/volpath, independent one-path frame
sampling, fixed texture footprint one, requested dimensions, and unclamped raw
XYZ. The authored vacuum packet-wavelength policy remains available when the live
PT profile supports it. Offline accumulation and authored output behavior are
unchanged.

## Completion and camera state

A timer polls one pending raw/reconstruction pipeline without waiting for each
render on the AppKit event loop. A completed image, its source raw frame and its
camera pose publish together. Presentation uses a separate Metal queue and a
completion lease that pins the image until the GPU finishes reading it. Two
retained output slots apply backpressure. Redraws and exposure edits do not count
as new rendered frames.

The title reports completed pipeline FPS, dimensions, mode, estimator, mutation
count, guide approximation counts, and observed input RGB counts below zero or
above 65,504. These input counts do not imply that temporal RGB was clamped.
Completion-to-completion FPS includes
polling, camera classification, guide/reconstruction work and intervening
presentation delays. It is not a display-refresh or interpolation rate. Explicit
captures can stall interaction and are not steady-state performance measurements.

Ordinary smooth camera movement can retain reconstruction reprojection history.
Signed temporal reconstruction conservatively resets on camera changes when its
guides report defocus, media, transmission or mixed-material ambiguity; a single
surface cannot identify all such path correspondences. Home cuts advance the
epoch; lens changes and reconstruction failures also reset the appropriate
history. Every frame explicitly carries its known camera medium.
Sessions do not infer it from the preceding request.

For translation through distinct medium interfaces, the tracker queries ordered
boundaries independently of BSDF visibility, opacity lobes and null flags. It
keeps the original movement ray and never offsets past an unverified region.
Distinct equal-distance hits and uncertain endpoints reject the move; they do not
silently assign a medium. Querying beyond the endpoint is necessary to observe
native triangles that omit a hit exactly at a finite ray maximum. The nearest
future boundary must certify that the destination remains before it. This is a
bounded numerical classifier for consistent, opaque interfaces, not a proof of
watertightness or support for arbitrary overlapping volumes. Ambiguous shared-edge
or tangent configurations may reject otherwise plausible motion.

## Validation boundary

The following historical checks cover the earlier MetalFX/raw viewer and its
camera tracker, before signed temporal display was integrated.

Portable option parsing, strict host/Metal compilation, Debug linking and focused
LLVM 18 static analysis pass. Independent source review covered resource leases,
camera/history state, output spaces and completed-frame accounting. The Debug
camera-medium GPU suite passes with Metal API validation (1.97 s), including
thin slabs, exact native entry/exit endpoints, later roots of a single sphere,
transformed interfaces, label consistency, budget handling, raw preservation and
lifetime checks. Separate 128² launch/auto-exit smoke runs complete three native
MetalFX/independent frames and three raw-RGB/Area-one-mutation frames. These are
functional checks, not 1080p performance measurements. The same classifier suite
also passes Release (27.10 s) and ASan/UBSan (.82 s) with API validation. A bundled
Release application check exercises translation, rotation, Home reset, raw/MetalFX
switching, exposure, paired capture under Pictures/rypath, and Escape termination.

The signed-temporal extension and measured default selection passed Debug/Release
viewer builds and portable option tests (0.24/0.31 s), focused LLVM 18 analysis and
formatting. A Release application check for that extension at native 1920×1080
exercised raw/temporal toggling, translation and rotation, Home reset, exposure,
window zoom, paired capture and Escape termination. Resizing preserved internal
dimensions. Two three-frame 128² runs with Metal API validation also completed:
independent sampling and explicit Area-one, both with temporal reconstruction.

An independent OpenEXR/NumPy decoder verifies all four native-1080p FLOAT captures
and paired sample/epoch/mode metadata. Temporal values remain signed (observed
minima −2.63 immediately after reset and −0.0456 after settling); display EV 0.5
is metadata, with film exposure one. These checks establish functionality and
capture behavior, not sustained FPS or ghost-free motion. Evidence is retained in
`.cache/validation/signed-viewer-ui-001`. The title's frame rate is not evidence
that the native-1080p performance target is met.

The initial MetalFX interactive check exposed strong spectral background noise
and widespread per-sample negative RGB clipping in its input. A separate frozen
qualification snapshot `0ef51ba` then compared both reconstruction modes on six
scenes at 320×180, 96 frames, four independent seed blocks and 2,048-SPP references
in eight batches. All twelve static datasets and five camera-motion datasets pass
an independent pixel/metadata audit. The study preserves signed raw and filtered
RGB, finite-reference uncertainty and the different timing scopes.

Across the six static fixtures, independent sampling with temporal reconstruction
has lower warm physical MSE than either independent/MetalFX or Area-one/temporal.
Area-one/independent temporal MSE ratios are 1.79–3.86, while Area-one evaluates
seven full paths per pixel with eligible history instead of one. Those measured
comparisons select independent plus temporal as the usable default. They do not
establish universal estimator superiority, motion quality or native-1080p FPS.
The mostly uniform coated fixture supplies limited spatial-quality coverage.
The measured snapshot also contains the subsequent packet-state storage
optimization; the default change itself does not change transport or filtering.
Full-replay Area/MCMC remains the explicit research candidate until its added cost
and correlation earn a measured quality benefit.

The separate [audited pipeline qualification](pipeline-qualification.md) reports
post-cap quality and native-1080p timing at historical `0ef51ba`. Across three
seeds, independent sampling spans 2.93–21.97 complete-pipeline FPS and Area-one
0.48–6.66 FPS. Only two of 90 primary runs meet p95 ≤50 ms, none meet approximately
33.33 ms, and no scene/arm passes all three seeds. The suite therefore misses its
20–30 rendered FPS target. Four audited sustained controls also miss 20 FPS;
the UI checks above remain functional evidence rather than timing qualification.

See [reconstruction](reconstruction.md) for surface-guide approximations and
[the golden pipeline](golden-pipeline.md) for the measurement target.
