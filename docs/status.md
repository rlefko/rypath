# Project status

## Current milestone

[Homogeneous spectral media (PR #32)](https://github.com/rlefko/rypath/pull/32)
is merged at `21f6667`.

The current resumable-rendering PR adds durable raw checkpoints, compatible-build
and supported-GPU continuation, and complete-batch cancellation. The fixed sample
target remains part of render identity. Presentation settings and batch sizes may
change; completed checkpoints can export without a GPU. See the
[checkpoint contract](checkpointing.md) and [validation](checkpoint-validation.md).

The next bounded task is a polygonal OBJ/MTL import profile, beginning with loader
coverage and conversion policies. Further importers, broader PBRT support,
long-run accumulation/convergence studies, licensed corpus acceptance and measured
M4 Max benchmarks remain on the [roadmap](roadmap.md). This does not establish
completion of the offline MVP or full pinned-corpus support.

## Validation

Implementation `b4fb5a0` passes Debug, Release and ASan/UBSan (116/116 each) on
Apple M4 Max with macOS 26.6 and Xcode 26.6. Metal-disabled Apple Clang and LLVM 18
builds pass 85/85 each; all 112 Python checks pass.

Twelve real-signal checkpoint/resume cases cover both samplers, SIGINT/SIGTERM,
same-build continuation and both Debug/Release directions. Each restores a
nonempty prefix and reaches its fixed target with exact raw pixel-byte equality
in these fixtures. Periodic publication and completed export without Metal pass.
Metal API validation passes 33/33 suites without skips; all three affected
shader-instrumented suites pass. Changed-unit Clang-Tidy, native Clang analysis,
repository hooks and independent review pass. A second GPU is unavailable;
cross-device compatibility is a policy with explicit numerical limits, not
measured multi-device evidence.

Earlier surface, media, spectrum, texture and HDR records retain their historical
validation evidence. Their counts describe those earlier revisions.
