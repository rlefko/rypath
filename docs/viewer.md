# Native pipeline viewer

The macOS `rypath-viewer` executable presents completed GPU frames through AppKit
and a CAMetalLayer. It uses native Metal intersections, wavefront PT, Area-domain
full replay with one corrected mutation, and native-resolution MetalFX by default.
The first version has static scenes and camera interaction. It does not animate
geometry or interpolate frames. These defaults are experimental pipeline choices;
measured image quality and speed determine their eventual promotion.

```sh
cmake --preset release
cmake --build --preset release --target rypath_viewer
build/release/rypath-viewer.app/Contents/MacOS/rypath-viewer --scene tests/fixtures/pbrt-render/cornell-box-adapted.pbrt
```

Opening `build/release/rypath-viewer.app` without arguments selects the built-in
scene. `--demo` selects it explicitly. `--width`/`--height` choose the fixed internal
resolution (default 1920×1080); resizing the window letterboxes that image.
`--estimator independent` selects the unreused control. Area accepts
`--mutations 0|1|2|4`. `--mode temporal|metalfx|raw` selects reconstruction,
with MetalFX remaining the default until quality measurements select a default.
`--reconstruction temporal|metalfx|raw-rgb` retains the existing option spelling.
Temporal display checks the device's float32 filtering capability (available on
the tested M4 Max). Temporal mode preserves signed RGB in RGBA32Float storage and
uses separate pre-spatial temporal history plus spatial filtering. It is an
approximate reconstruction; it does not change raw Monte Carlo accumulation.
Raw selects the explicit unfiltered color-conversion control. Requested denoiser
unavailability or failure pauses with a visible diagnostic; it never silently
switches output modes. `--frames N` exits after N
completed pipeline frames and returns failure on a rendering/reconstruction error.

## Controls and output

- WASD / QE move; Shift increases speed.
- Drag the mouse or use arrow keys to rotate; Home restores the authored camera.
- R compares raw RGB with the selected denoiser for subsequent frames. Temporal
  and MetalFX selections are remembered across toggles. An initially raw viewer
  uses temporal as its comparison denoiser.
- Plus/minus changes display exposure in EV; this never edits raw radiance.
- Cmd+S saves a fresh directory containing paired raw and reconstructed FLOAT EXRs.
- Escape closes the viewer; the View menu provides fullscreen.

Capture directories are created only on request under `Pictures/rypath` by default.
`--capture-dir` selects a different parent. The raw EXR contains the unexposed
linear-RGB conversion of retained raw XYZ. The reconstructed EXR directly stores the separate presentation RGB, with
mode, sample, epoch, estimator and mutation metadata. Temporal captures are
`temporal.exr`, tagged as signed temporal denoised RGB from RGBA32Float storage;
MetalFX uses `denoised.exr`, and raw preview uses `raw-rgb.exr`. All are FLOAT EXRs.
Temporal captures preserve negative and high finite RGB values. MetalFX and raw
preview retain their presentation range clamps. Display tone mapping and EV are
not baked into either capture. The display clamps negatives for presentation,
applies an exposure-dependent Reinhard map without overflowing finite HDR through
an exposure multiplication, and performs one sRGB transfer, and explicitly tags the layer with the sRGB color space for
color matching on wide-gamut displays.

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
above 65,504. These input counts do not imply that temporal RGB was clamped. Completion-to-completion FPS includes
polling, camera classification, guide/reconstruction work and intervening
presentation delays. It is not a display-refresh or interpolation rate. Explicit
captures can stall interaction and are not steady-state performance measurements.

Smooth camera movement retains reconstruction reprojection history. Home cuts
advance the epoch; lens changes and reconstruction failures also reset the
appropriate history. Every frame explicitly carries its known camera medium.
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

The temporal option, comparison toggle, and signed-HDR presentation changes have
source coverage in the portable option tests. Their build and native viewer
runtime checks remain pending on the updated reconstruction parent; earlier
viewer checks above do not validate this extension.

The initial interactive image check exposes strong spectral background noise and
widespread per-sample negative RGB clipping in MetalFX input. This is a failed
quality observation, not a qualified denoising result; the reconstruction fallback
and reference-image study must resolve it before selecting a default.

See [reconstruction](reconstruction.md) for surface-guide approximations and
[the golden pipeline](golden-pipeline.md) for the measurement target.
