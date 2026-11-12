# Architecture and product goals

## Implemented foundation

The current program is a dependency-free C++20 command-line executable. CMake,
Ninja, and CTest provide the build and test contract. It accepts `--help` and
`--version`; no arguments prints help, and invalid arguments fail with a diagnostic.
There is no rendering backend or scene representation yet.

## Planned boundaries

The offline renderer will separate scene ingestion, immutable render data,
transport, sampling, scattering, media, acceleration, accumulation, and image
presentation. Concrete APIs will be introduced with the first validated use case,
without a general plugin system or cross-platform GPU abstraction.

- **Execution:** C++20 host code, Metal Shading Language kernels, and Metal native
  triangle and instance acceleration structures. GPU resource ownership, buffer
  layouts, lifetimes, synchronization, and failure handling will be explicit.
  Small independent CPU reference paths will support differential validation.
- **Transport:** sampled-wavelength Monte Carlo paths with documented PDFs,
  wavelength/sensor weighting, light sampling, MIS, and compensated termination.
  Strict rendering will report conflicting or incomplete settings rather than
  silently clamp contributions, truncate paths, or discard unfinished samples.
- **Accumulation:** canonical raw radiance sums and counts remain separate from
  exposure, tone mapping, denoising, and preview approximations. Reproducible sample
  identities must survive scheduling changes, cancellation, and checkpoint/resume.
- **Scenes:** PBRT v4 is the scene-description standard. All loaders will target a
  versioned internal representation. Accepted syntax, faithful semantics,
  approximations, and unsupported features will be tracked separately. Missing
  materials, parameters, or geometry must produce actionable diagnostics.
- **Presentation:** linear HDR output precedes a native SwiftUI/AppKit application.
  Display-only edits will not invalidate raw samples; changes to the integrand
  will. Preview history and raw accumulation are distinct.

Metal API availability alone will not establish hardware ray-tracing support or
performance on a selected device. Custom acceleration structures are deferred
experiments requiring correctness and measured benefit. No Metal execution or
numerical estimator is implemented in this foundation.

## Offline MVP acceptance

The offline MVP must combine progressive Metal rendering, a named and pinned
supported PBRT profile, indexed geometry and instancing, textures and lighting,
spectral surface scattering, homogeneous media, linear OpenEXR output,
checkpoint/resume, and cancellation. It also includes documented polygonal
OBJ/MTL, PLY, glTF/GLB, and STL import profiles.

Acceptance requires independent numerical tests, unmodified supported upstream
scenes with recorded overrides, and reproducible M4 Max measurements. Parser
success does not establish rendering compatibility, and selected corpus coverage
does not establish full PBRT support. Full OBJ includes non-polygon constructs;
those remain explicit roadmap work until rendered faithfully.

## Reference starting points

These define future investigations, not algorithms already implemented here:

- [Apple Metal C++ interface](https://developer.apple.com/metal/cpp/)
- [Apple Metal ray tracing](https://developer.apple.com/videos/play/wwdc2023/10128/)
- [PBRT v4 input format](https://www.pbrt.org/fileformat-v4)
- [Physically Based Rendering, fourth edition](https://pbr-book.org/4ed/contents)

Implementation decisions will cite primary research or official platform sources,
identify project adaptations, and distinguish published results from measurements.
