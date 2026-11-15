# Core PBRT Texture Performance

Measured November 14, 2026 using Release builds of the pre-texturing revision
`966aa2b` and runtime implementation `9e1789b` on a MacBook Pro with an Apple M4 Max
(16 CPU cores, 40 GPU cores) and 128 GiB of unified memory. The host ran
macOS 26.6 (25G5065a), Xcode 26.6 (17F113),
Apple Clang 21.0.0 (clang-2100.1.1.101), CMake 4.0.3, and
Ninja 1.13.2.

Each measurement starts a new `rypath_pbrt_reference` process. Wall time
includes PBRT preflight and translation, any input decoding, Metal pipeline
and acceleration structure creation, rendering, host accumulation, and
complete linear RGB PFM output. Peak RSS is the macOS `/usr/bin/time -l`
maximum resident set size for the process. It gives no separate accounting
for GPU allocations.

The machine was on AC power. No thermal or performance warning had been
recorded by `pmset -g therm`. Other project builds and GPU checks were paused.

Each pair uses the same sample budget, camera, independent sampler seed 0,
spectral sensor, and display-independent output. The reference executable
uses 1,024-sample batches. The three legacy scenes retain their transport
settings. The EWA case retains its 8×8 raster so that the rotated fixture's
texture footprint stays fixed as the sample count increases. Both feature
controls use the diffuse fixture's depth-one environment-lit plane. The
graph mixes two neutral RGB reflectances at weight 0.5 to reproduce a
direct 0.375 reflectance. The alpha case uses a 1×1 all-white grayscale
point-filtered image so every masked candidate hit should survive.

## Reproduction

From the repository root, prepare separate Release builds with Metal enabled
and run the benchmark on a quiet host.

```sh
git worktree add --detach .cache/core-texture-baseline 966aa2b
(cd .cache/core-texture-baseline && cmake --preset release && cmake --build --preset release)
cmake --preset release
cmake --build --preset release
python3 scripts/benchmark_core_textures.py \
  --baseline .cache/core-texture-baseline/build/release/rypath_pbrt_reference \
  --candidate build/release/rypath_pbrt_reference \
  --work-dir .cache/core-texture-performance-scaled --sample-scale 16
```

The harness retains staged scenes, all PFM files, binary and Metal library
hashes, and per-run wall time, RSS, and output hashes in `results.json`.
The setup probe runs three 1×1, 1-spp subprocesses per variant. Its first
measurement can include pipeline specialization, depending on the driver's
existing cache. This setup time includes parsing, construction, a minimal
render dispatch, and PFM writing. One full warm-up per variant precedes
the three timed full renders, and pair order alternates each round.
The measured run used warm OS/Metal caches following validation work.
Clean-cache specialization cost and build time were outside this run.

## Complete PFM results

Times below give the median and minimum–maximum of three measured runs
after one warm-up. RSS gives the median process peak in MiB. The delta
compares medians. Legacy pairs compare the baseline with core textures.
The last two pairs compare controls within the core-texturing build.

| Scene or pair | Raster / spp / depth | Control seconds | Compared seconds | Delta | Control / compared RSS |
| --- | --- | --- | --- | --- | --- |
| Diffuse | 64×64 / 65,536 / 1 | 3.076 (3.033–3.183) | 3.145 (3.052–3.659) | +2.25% | 32.86 / 33.27 |
| Image EWA | 8×8 / 2,097,152 / 1 | 2.420 (2.410–2.933) | 2.432 (2.422–2.859) | +0.51% | 59.61 / 60.44 |
| Adapted Cornell | 64×64 / 32,768 / 65 | 4.130 (4.083–4.447) | 4.283 (4.206–4.905) | +3.72% | 32.77 / 33.27 |
| Direct RGB → mix graph | 64×64 / 65,536 / 1 | 3.224 (3.194–3.245) | 3.584 (3.583–3.647) | +11.17% | 33.02 / 60.33 |
| Opaque → all-one image alpha | 64×64 / 65,536 / 1 | 3.241 (3.230–3.247) | 5.166 (5.052–5.200) | +59.38% | 32.97 / 64.45 |

All legacy cases have higher measured candidate medians. Their overlapping
ranges and the three-repeat sample size limit conclusions about these
small regressions. The graph and alpha controls show larger overhead
with disjoint measured ranges and increased process RSS.

Every variant produced identical PFM bytes across its warm-up and three
repeats. Cross-variant Cornell, graph, and alpha outputs matched byte for
byte. Diffuse's maximum channel difference was `1.19209e-7`, with RMSE
`2.95735e-9`. EWA's maximum was `1.86265e-9`, with RMSE `1.34425e-10`.
Those paired outputs retain the same settings and raw-image model at
the compared timing budgets.

## Setup probe and scope

The three warm-cache 1×1/1-spp startup probes had median complete-process
times of 48.9/47.6 ms for diffuse, 51.1/57.2 ms for EWA, 55.2/55.8 ms
for Cornell, 51.3/58.9 ms for direct/mix, and 54.7/64.2 ms for
opaque/image alpha. These include all setup and minimal rendering.
The initial calibration retained at `.cache/core-texture-performance/`
used shorter 0.2–0.4-second workloads. Its first diffuse process took
380/429 ms with uncontrolled cache state. The reported table uses the
subsequent scaled run and retains every measured repeat.

This experiment covers three small legacy scenes and two controlled
feature costs on one M4 Max. It measures time to the same configured
raw output budget, with matched-output evidence. It leaves convergence
curves, other seeds, complex scenes, high-resolution throughput, varying
alpha coverage, image lighting, detail mapping, displacement, clean-cache
specialization, and isolated GPU-memory accounting unmeasured.

## Measured artifacts

The measured artifacts have the following SHA-256 identifiers.

| Artifact | Baseline | Core textures |
| --- | --- | --- |
| Reference executable | `d4aaf5551ec449bb5758ae5a3a717a77084b76df061c0f4188fef93773b73295` | `c963e6da22e1d04959b74d974d58e0aa0a2146a956c03721381fdfccaf5f351d` |
| Metal library | `d9881411cbb683b298873961fce5a7684ed3e9f1c9612a060ba7b0ce6de60897` | `b0bba779a206cd50178a677d7d3cf4a9e1727755f97e0786e58f13a9baf24e84` |

The candidate binaries were built before the runtime commit was recorded.
The lead confirmed the same runtime content, with subsequent formatting.
These hashes identify the artifacts actually measured.
