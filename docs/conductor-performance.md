# Conductor Integration Performance

Measured November 15, 2026 with Release baseline `edaeb6da6744cb50bff759f4db3d964b7f1e7825`
and conductor implementation `6be5a4b530a92ab6f93f1cedcce51177c97520bc` on an Apple M4 Max.
This measures existing diffuse workloads and two existing feature controls.
It makes no speedup claim and does not measure conductor rendering cost or
conductor convergence.

The MacBook Pro has 16 CPU cores, 40 GPU cores and 128 GiB unified memory. It ran
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0
(clang-2100.1.1.101), CMake 4.0.3 and Ninja 1.13.2. The machine was on battery
power, discharging from 41% to 37% during the 05:05:48–05:09:36 UTC run.
`pmset -g therm` reported no thermal or performance warning before or after.
Other project builds and GPU checks were paused. Three background
`SimMetalHost` services were present before and after; their presence does not
establish GPU activity. GPU utilization and clock frequencies were not sampled.

Each measurement starts a new `rypath_pbrt_reference` process. Complete wall
time includes parsing, checked translation, texture decoding, Metal pipeline
and acceleration structure creation, rendering, host accumulation and linear
RGB PFM output. Peak RSS is the process maximum from macOS `/usr/bin/time -l`;
it does not isolate GPU allocations. This run uses the final implementation's
Release artifacts, with documentation and reference-harness changes still
uncommitted at measurement time; their paths are retained in `environment.json`.

## Protocol

The unchanged `scripts/benchmark_core_textures.py` stages dependencies and
alternates pair order. It runs three separate 1×1/1-spp startup probes, then one
full warm-up and five measured full renders per variant. Startup statistics
include all three probes. Full statistics exclude the first warm-up; every
subsequent repeat is retained. Driver caches were warm following validation;
clean-cache compilation is unmeasured.

```sh
python3 scripts/benchmark_core_textures.py \
  --baseline .cache/conductor-baseline/build/release/rypath_pbrt_reference \
  --candidate build/release/rypath_pbrt_reference \
  --work-dir .cache/conductor-performance-final-6be5a4b-20261115T050548Z \
  --repeats 5 --sample-scale 16
```

Both versions use the same independent seed zero, sensor, camera, scene,
transport depth and sample budget for each regression pair. The reference
harness uses 1,024-sample batches. EWA retains its 8×8 raster to keep the
texture footprint fixed. The two feature controls use the final candidate for
both sides: direct neutral RGB reflectance 0.375 versus an equivalent mix graph,
and opaque geometry versus an all-one scalar point-image alpha mask.

## Baseline Regression Pairs

Wall times give median and minimum–maximum seconds across five measured runs.
RSS gives the median process peak in MiB, baseline/candidate. Delta compares
wall-time medians.

| Scene | Raster / spp / depth | Baseline seconds | Candidate seconds | Delta | Baseline / candidate RSS |
| --- | --- | --- | --- | --- | --- |
| Diffuse | 64×64 / 65,536 / 1 | 3.301 (3.256–3.363) | 3.311 (3.284–3.970) | +0.29% | 33.34 / 32.89 |
| Image EWA | 8×8 / 2,097,152 / 1 | 2.560 (2.530–2.605) | 2.521 (2.512–3.056) | −1.56% | 60.61 / 60.44 |
| Adapted Cornell | 64×64 / 32,768 / 65 | 4.497 (4.477–4.509) | 4.513 (4.499–4.531) | +0.34% | 33.66 / 33.36 |

The medians are close and measured ranges overlap. The slower candidate
outliers in diffuse and EWA are retained. Five repeats, battery operation and
unmeasured background GPU activity limit interpretation of these small deltas.
They do not establish a general performance improvement or an equal-error
rendering advantage.

## Same-Implementation Feature Controls

Both sides below use implementation `6be5a4b`, at 64×64 pixels, 65,536 spp and
depth one. These isolate existing feature overhead within the candidate; they
are separate from the baseline regression pairs above.

| Control → feature | Control seconds | Feature seconds | Delta | Control / feature RSS MiB |
| --- | --- | --- | --- | --- |
| Direct RGB → mix graph | 3.321 (3.281–3.329) | 3.717 (3.696–3.742) | +11.94% | 33.14 / 60.25 |
| Opaque → all-one image alpha | 3.499 (3.418–3.914) | 6.226 (5.679–6.526) | +77.91% | 33.13 / 64.72 |

Both controls have higher feature costs and disjoint measured time ranges.
The alpha feature's 5.679–6.526-second range is retained in full; this run does
not isolate its source of variation. These overheads are not attributed to
adding conductor materials.

## Startup, Memory and Raw Output

Startup times give median and minimum–maximum milliseconds for all three
1×1/1-spp probes. Labels follow the pairs above.

| Pair | Baseline/control ms | Candidate/feature ms |
| --- | --- | --- |
| Diffuse | 54.4 (53.5–69.9) | 57.2 (55.6–60.2) |
| Image EWA | 72.3 (67.5–75.1) | 68.3 (66.2–69.2) |
| Adapted Cornell | 67.9 (59.8–76.7) | 65.6 (62.7–66.1) |
| Direct / mix | 58.5 (57.7–59.2) | 67.2 (65.1–67.9) |
| Opaque / image alpha | 59.9 (56.6–67.8) | 76.4 (75.7–77.9) |

Measured full-run RSS ranges, in MiB, are retained separately from the medians.

| Pair | Baseline/control RSS range | Candidate/feature RSS range |
| --- | --- | --- |
| Diffuse | 33.09–33.52 | 32.83–33.11 |
| Image EWA | 60.52–60.66 | 60.39–61.20 |
| Adapted Cornell | 33.48–33.69 | 33.13–33.59 |
| Direct / mix | 33.11–33.59 | 60.16–60.50 |
| Opaque / image alpha | 33.11–33.48 | 64.59–65.31 |

All 90 render processes succeeded. Every variant's full warm-up and five
repeats produced identical PFM hashes. All five paired raw outputs matched
byte for byte: maximum channel difference and RMSE were both zero. Thus the
reported pairs retain the same configured raw output at the timing budgets;
this does not replace convergence testing.

## Artifact Identity and Limits

All staged wrappers, 90 PFMs, process logs, timings, RSS values and output
hashes are retained in
`.cache/conductor-performance-final-6be5a4b-20261115T050548Z/`.
`results.json` retains scene hashes and individual measurements;
`environment.json` retains the exact command, revisions, host versions,
pre/post power and thermal observations, load averages and process snapshots.

| Artifact | Baseline | Candidate |
| --- | --- | --- |
| Reference executable SHA-256 | `51f2608200464995f840612f50e290a2578e612f8390db8a82531e01a5abd601` | `2ac4a8f838dd3de52c6c3258ec7fe11ff8e922467e52ea709f729d5fe1cb575a` |
| Metal library SHA-256 | `30d0d325bfca8a412e7ca78c7db570d7ee2019f73ac768925c9b5950f54d2d56` | `cc44696c064ee4182f0822b74556ae2e0f3a45a4c8760d63a7f1458dedb5e481` |

The results index SHA-256 is
`80bddd9b5ba9d6acb572fab8c8e9d3945e493c78879918dc05247dc890f04924`;
the environment record SHA-256 is
`8b78d06dab3e6565af7d7d0c561f88b9ed297b791b39e94d1b38c198d2a2370b`.
The earlier `.cache/conductor-performance/` preliminary run used shorter sample
budgets and earlier candidate hashes. It is retained for diagnosis and is not
the final implementation measurement.

This experiment covers three small legacy scenes and two existing feature
controls on one host, with fixed output budgets and one seed. It leaves
conductor scenes, convergence curves, equal-error timing, high-resolution
throughput, complex geometry, other seeds, clean-cache specialization, build
time and isolated GPU-memory accounting unmeasured. Battery operation also
precludes direct comparison with the earlier AC-powered core-texture record.
