# Checkpoint and resume validation

This change affects persisted state and execution. It does not alter the physical
model, transport estimator, random-variable mapping, or compensation algorithm.
The [checkpoint contract](checkpointing.md) specifies compatibility and limits.

## Acceptance criteria

- Same binary/device continuation must preserve every binary64 sum/correction bit
  and count relative to uninterrupted rendering, including changed batch sizes
  and tail batches. Independent and Sobol streams cover textures and volumes.
- Cross-build continuation must retain the nonempty historical prefix, append
  exactly the missing sample range, and preserve scene identity. Raw XYZ means
  use the predeclared arithmetic gate `2e-5 * max(1, abs(reference))`; this is a
  bounded fixture comparison, not a general cross-GPU statistical guarantee.
- Codec checks cover FIPS SHA-256 known answers, an independently encoded Python
  `struct`/`hashlib` golden file, signed zeros, nonzero compensation, counts above
  `2^53`, chunk boundaries, authenticated malformed fields, corruption and limits.
- Failure injection covers partial writes, interrupted I/O, creation, data sync,
  close, rename, and directory sync. Failures before publication preserve the old
  file; failures afterward retain a readable new file with an accurate diagnostic.
- Cancellation before work and during a multi-tile batch must retain only whole
  batches. Tests cover failure/completion precedence, unavailable backends,
  presentation changes, completed export, and path aliases.

The integration harness independently decodes checkpoint bytes and checks SHA-256,
all pixel records and provenance ranges. It sends real SIGINT/SIGTERM after
nonzero committed progress, without sleeps or retry-until-pass behavior. Cancelled
jobs must leave a sentinel final image unchanged. Both samplers render an owned
32×32 textured homogeneous-medium fixture to a fixed 256-spp target; resumed and
uninterrupted jobs use different batch partitions.

```sh
python3 scripts/validate_checkpoints.py --first build/debug/rypath \
  --second build/release/rypath --output-dir .cache/checkpoint-validation/debug-release
python3 scripts/validate_checkpoints.py --first build/release/rypath \
  --second build/debug/rypath --output-dir .cache/checkpoint-validation/release-debug
```

Omit `--second` to require exact raw pixel bytes with the same executable.
The harness records executable hashes, build/device/OS provenance, sample ranges
and measured differences in `results.json`. It requires real hardware and has no
availability skips. Routine portable tests and bounded native tests remain in
CTest; larger compatibility runs are opt-in.

## Measured evidence

Implementation `b4fb5a0` was validated on 2026-11-15. The portable Apple Clang
and LLVM 18 builds each pass 85/85 tests. All 112 Python checks and the ten
changed-unit LLVM 18 Clang-Tidy analyses pass; native Clang analysis and repository
hooks pass. Debug, Release and ASan/UBSan each pass 116/116 tests. Metal API
validation passes 33/33 suites without skips; the two negative handshake suites
and native resume suite also pass shader instrumentation with error reporting
and abort-on-fault enabled (3/3). See [status](status.md).

The same-build, Debug→Release and Release→Debug integration runs pass all twelve
cases. Every interrupted job retained 2 spp, then resumed to 256 spp with
unchanged historical provenance and exactly matching raw pixel bytes. The
maximum normalized XYZ mean difference was zero; this observation does not
strengthen the cross-build/device compatibility guarantee. Artifacts are retained
under `.cache/checkpoint-validation/{same-build,debug-release,release-debug}`.

A 256×192 demo at 4,096 spp with a one-second checkpoint interval saved committed
counts 0, 736, 1536, 2304, 3136, 3904 and 4096. A completed PBRT checkpoint also
exported EXR through the Metal-disabled build without acquiring a device.
These artifacts are retained under `.cache/checkpoint-validation`.

The available device is Apple M4 Max (`applegpu_g16s`), macOS 26.6 build
`25G5065a`, Xcode 26.6 build `17F113`. A second GPU is unavailable. Cross-GPU
continuation is an explicit compatibility policy, not measured multi-device
evidence. Filesystem fault injection does not simulate a physical power failure.

Independent review checked canonical identity against Clang's AST: all 28 owned
value types and 155 hashed fields were covered, with the intended presentation
and cost-metadata exclusions. Review found and closed the signal flag's
cross-thread synchronization issue, zero-exposure validation mismatch, and an
initial empty-prefix weakness in the cross-build test. Full portable testing
also caught and fixed the fresh unavailable-backend Film contract. No acceptance
threshold was widened to make these checks pass.
