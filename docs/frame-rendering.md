# Resident frame baseline

`rypath-frame-benchmark` measures a fixed camera scene with one new independent
camera sample per pixel per frame. Scene translation, native acceleration
structures, shading resources and pipelines are prepared once. The initial
control submits one frame and waits before submitting the next. The session API
has two retained frame slots; this command does not measure overlapping work.

```sh
rypath-frame-benchmark --demo --width 1920 --height 1080 \
  --warmup 8 --frames 120 --output /tmp/frame-baseline.json
rypath-frame-benchmark --scene /path/to/scene.pbrt \
  --width 1920 --height 1080 --warmup 8 --frames 120 \
  --seed 1 --material-seed 0 --output /tmp/scene-frame-baseline.json \
  --capture /tmp/scene-last-frame.exr --preview /tmp/scene-last-frame.png
```

All outputs and their `.pending` siblings must be fresh, distinct and separate
from the PBRT entry point and its inventoried input dependencies. An existing
file or symlink is rejected. Preflight runs before output reservation so asset
aliases can be checked. Failures after safe report reservation retain a failure
JSON with completed counts. A failed preflight retains its diagnostics and does
not attempt rendering. Exit 0 requires all warmup and measured frames to finish;
exit 2 means Metal/device/ray tracing is unavailable; other failures exit 1.

The defaults are 1920×1080, 120 measured frames, eight warmup frames, seed 1 and
material seed 0. `--scene` and `--demo` are mutually exclusive; `--output` is
required. The benchmark explicitly overrides authored resolution, sampler and
sample budget, stochastic seeds, and finite film clamping. Authored BDPT becomes
path tracing; authored volumetric path tracing retains volume transport. The
authored packet wavelength policy selected by BDPT or a finite film clamp is
retained for vacuum paths up to depth 64. Active media or greater depths
retain scalar transport with an explicit provenance note. Other unsupported
rendering semantics retain translation diagnostics. The report
records overrides and translation provenance. This is a live frame policy,
not an offline PBRT equivalence claim.

The independent sampler consumes distinct absolute sample indices: warmup uses
`[0, warmup)`, followed by measured indices `[warmup, warmup + frames)`. Texture
footprints always use a fixed one sample budget. Each raw frame replaces the
previous frame; frames are not progressively accumulated or denoised. Optional
capture exports the last successful frame, even when a later frame fails, as
unexposed FLOAT linear RGB EXR through the existing XYZ film conversion. Its
metadata identifies sample index, epoch, seeds, footprint budget, integrator,
spectral mode, source identity and build. PNG is a separate display preview
with the authored scene exposure. Export and readback are outside per-frame timings.

JSON schema 1 reports startup and warmup durations separately, every attempted
frame's sample index, completion/count/error, CPU submit-and-wait wall duration,
and actual command-buffer GPU duration. A missing or invalid GPU timestamp is
`null`, never substituted with CPU wall time. Median and p95 use nearest rank
over completed measured frames only; GPU summaries include only valid GPU
intervals and report missing timestamps separately. Startup includes preflight,
translation, reservation and resident session preparation. CPU and GPU durations
overlap and must not be added. Build, OS, device, scene dependencies and effective
settings identify the observation.

The native `FrameSession` API accepts complete camera snapshots without rebuilding
the scene. Translating a camera rebases texture graph and alpha definition
coordinates into completion-retained, per-slot buffers; `camera_upload_bytes`
reports that cost, and a repeated origin reuses its slot's cached mappings.
Callers provide matching texture camera transforms and explicitly identify the
camera medium after a boundary crossing. Non-vacuum camera overrides require
scalar PBRT volume transport. Failed submissions leave the session usable.

Live sessions default to `IntersectionProfile::native` in `FrameConfig`.
The benchmark exposes `--intersections native|compensated`, defaulting to
`native`; select `compensated` explicitly for a comparison baseline. This option
is immutable for a session. The existing offline renderer builders continue to
default to compensated intersections.

Native mode uses authoritative hardware triangle distances and barycentrics
while retaining alpha callbacks and procedural patch/analytic composition.
Near-edge candidate coverage, rounded distances and endpoint inclusion can differ
from compensated mode; native mode does not recover misses with a compensated
query. Both profiles retain unclamped raw samples and the same sampling policy.
See the [intersection profile and measured witnesses](geometry.md#explicit-native-triangle-profile).
Changing profiles requires a new session and separate sample history.

The selected profile is recorded as `settings.intersection_profile` in JSON and
`rypath.frame.intersections` in EXR metadata. Canonical `source_identity`
continues to identify the owned scene and sampling settings; it does not encode
this numerical execution choice. Compare or group results using both source
identity and intersection profile. Frame validation compares each profile's GPU
image against the direct sample kernel built with that same profile, including
empty geometry, demo geometry, scalar and packet paths; it does not require
native and compensated images to agree at numerical edge cases.

## Initial M4 Max evidence

On 2026-11-22, implementation `1c1d7bb` passed the frame-session tests in Release
and ASan/UBSan with Metal API validation. The checks cover independent host sensor
reduction, scalar and packet tiles, dielectric geometry, translated world/alpha
textures, fixed footprints beyond sample index 2³², camera media, retained-frame
lifetime, capacity rejection and nonfinite-output failure. The portable availability,
CLI and statistics checks pass. Focused LLVM 18 clang-tidy and the native Clang
analyzer pass. Full publication checks are reported on the PR.

The resident **compensated-intersection megakernel** baseline uses an M4 Max
(40 GPU cores, 128 GB), macOS 26.6 and Xcode 26.6. Each scene has three independent
seeds, four warmup frames and 32 measured frames per seed at native 1920×1080,
one new sample per pixel, with API validation off. Scene order alternates. All
216 frames complete with exact pixel/sample accounting and valid GPU timestamps.
Captures occur after timing. The table gives ranges across the three runs.

| Scene and transport | CPU submit/wait median | CPU submit/wait p95 |
| --- | ---: | ---: |
| Cornell adapted, scalar PT, depth 65 | 1156.3–1164.7 ms | 1181.3–1191.1 ms |
| Coated path fixture, packet PT, depth 5 | 228.0–229.2 ms | 231.8–232.3 ms |

These are small-scene baseline measurements, with no reuse or reconstruction.
They do **not** meet the interactive target or establish image-quality equivalence.
Native triangle and wavefront changes must be measured against this retained
baseline rather than assuming published speedups apply.

The measured Release source digest is
`00eb5d22587441c92b1b11431d108d0e3893db2286a6129fc0de69072b2970d8`.
Local JSON reports, command manifests, raw FLOAT EXRs, PNG previews and frozen
executables are retained under `.cache/validation/resident-frames-001/`.
