# Reconstruction diagnostics

The diagnostic driver sends each completed native frame to separate signed
temporal, temporal-only and MetalFX streams. They have independent history and
scratch resources. The raw frame remains retained through every consumer.
These instrumented runs establish input and state identity, not frame performance.

`ReconstructionConfig::capture_diagnostics` defaults off. Explicit opt-in allocates
copies of the signed filter's initial spatial state and every spatial pass.
`MetalReconstructionStreamProbe::capture_diagnostics` copies actual quantized
inputs, committed pre-spatial history, pass values and the retained last output.
It requires external serialization with submission and successful completion.
Snapshots own their CPU bytes; they do not expose mutable GPU resources. The
ordinary pipeline performs no snapshot dispatches or readbacks.

The current raw frame is one fresh estimate, not a progressive sample average.
The separate static `raw-online-mean` is an average over the recorded frames.
MetalFX receives clamped half RGB; the signed stream receives unclipped float RGB.
Guide and variance fields retain their actual half/float/integer representation.
Source identities, sample indices, resets and input/output hashes accompany results.

## Capture and replay

Build with a supported preset, then create a dedicated artifact root:

```sh
cmake --preset debug
cmake --build --preset debug --target rypath_reconstruction_diagnostics
mkdir -p .cache/reconstruction-review
MTL_DEBUG_LAYER=1 build/debug/rypath-reconstruction-diagnostics \
  --scene tests/fixtures/pbrt-render/multibounce.pbrt \
  --artifact-root .cache/reconstruction-review \
  --output-dir .cache/reconstruction-review/capture \
  --width 128 --height 128 --frames 4 --capture-mode replay
```

`--estimator independent|area1|area2|area4` selects independent sampling or
Area replay with exactly one, two or four mutation steps. The default remains
independent; its mutation count is zero. `configuration.json` records the exact
estimator label and actual configured mutation count. These choices use the
existing frame mutation-count contract and preserve replay semantics. Reference
batches still require independent sampling.

Area2 and Area4 image galleries are additional diagnostic comparisons. They do
not expand or replace the frozen three-seed Area1 qualification plan; their GPU
cost and evaluation counts require separate verification.

Replay mode archives every input of the signed spatial arm, including reset
decisions, for a complete cold-start sequence of at most 16 frames. A replay list
is a JSON object with a `snapshots` array naming ordered `temporal-0.json`,
`temporal-1.json`, and subsequent records relative to that list. Supply it with
`--replay-list PATH` and a fresh output directory. The driver creates one dummy
transport frame to acquire the device and pipelines, then filters captured
inputs directly. Matching-settings replay must reproduce captured output exactly;
`--replay-spatial 0..5` can instead identify a deliberate spatial-pass ablation.

Default `--capture-mode final` records per-frame hashes and region means, and
retains final full-resolution RGB planes plus diagnostic metadata. Internal
texture hashes without archived payloads are explicitly marked unavailable for
replay. An optional ROI file has `{"regions":{"name":[x0,y0,x1,y1]}}`, using
exclusive upper coordinates. Default whole-image means are not feature-quality
gates. Both modes enforce a shared 8 GiB artifact-root limit; an individual
internal snapshot is limited to 1 GiB. No historical artifacts are deleted.

Independent references require explicit seeds disjoint from candidate seeds.
`--reference-samples 1024` renders a static raw reference batch;
`--reference-samples 4096` is its single declared extension. Four independent
batches are analyzed separately. A fixed `--reference-offset` selects a translated
camera checkpoint; `--motion-offset` selects the four-phase moving sequence.
Reference uncertainty and temporal stability must be measured outside this
driver before selecting a denoiser.

## Interpretation

Negative-RGB rectification, primitive-dependent spatial support, reflection and
transmission guide approximations, and variance-guided halo propagation are
separate diagnostic questions. Matching only the final raw image does not establish
that earlier temporal inputs matched. Analyze API-boundary conversion separately
from nonlinear neural output, and preserve genuine spectral bands and small
reflections when choosing a repair. Do not use radiance clipping or saturation
compensation to disguise a failure.

The diagnostic addition changes observation and resource usage only. It does not
change transport, raw accumulation, denoiser weights, or the viewer default.
The repair and scene-quality qualification remain subsequent work.

## Validation record

On 2026-11-23, the diagnostic implementation through `045ee6e` was checked on
Apple M4 Max (128 GiB), macOS 26.6 / 25G5065a, Apple Clang 21.0.0:

- Debug and Release diagnostic builds succeeded. The full Debug suite passed
  261/261 tests at `92ead0b` (documentation-only changes after `045ee6e`). The CLI
  covers help and 14 failures before device acquisition.
- The raw, MetalFX, signed reconstruction and temporal adapter tests passed with
  `MTL_DEBUG_LAYER=1`. Opt-in snapshots preserve raw XYZ and exactly match outputs
  from streams without diagnostics; reset and history isolation are exercised.
- A four-frame, 128×128 Release capture and cold-start replay of `multibounce.pbrt`
  passed with API validation. All four replayed RGBA output hashes and history
  counts matched; independently compared RGB payloads were byte-identical.
- Apple Clang's static analyzer completed for the reconstruction stream, temporal
  adapter and diagnostic driver. Independent source review found no blockers.

These checks establish the diagnostic contract, not denoiser scene quality or
real-time performance. Large native-resolution qualification remains separate.

Area fanout uses contract 5 with source-target MIS and the default conditional-mean
output. `--area-output selected` retains the selected-output ablation. These
settings and the cold/warm full-replay evaluation counts are recorded explicitly;
independent references remain separate. Historical validation below predates this
adapted integration; current combined validation is pending.
