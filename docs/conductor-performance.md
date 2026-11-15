# Conductor Integration Performance

The November 15, 2026 grazing-fix run is **confounded**. The host changed from
battery power at 18% to AC power at 21% charging. UTC observations span
05:44:33–05:57:02, while the 90 recorded process intervals sum to 221.39 seconds;
the gap is unexplained. These timings support no speedup, regression or other
performance attribution. All raw-output checks remain valid.

The Release candidate was built from `91452929fe76292bb9119e036eba98218eddda0a`
plus the uncommitted grazing-frame fix. Its exact public shader/test patch is
retained as `working-tree.patch`, SHA-256
`57b9782a5c1b336636d24df6610e710bacff442aef86e6f62466024641f993d0`.
The baseline is `edaeb6da6744cb50bff759f4db3d964b7f1e7825`.

The host was a MacBook Pro with Apple M4 Max, 16 CPU cores, 40 GPU cores and
128 GiB unified memory, running macOS 26.6 (25G5065a), Xcode 26.6 (17F113),
Apple Clang 21.0.0, CMake 4.0.3 and Ninja 1.13.2. No thermal or performance
warning was recorded before or after. Other project builds and GPU checks were
paused. Three background `SimMetalHost` services were present; their GPU
activity was not measured. GPU clock frequencies and the time of the power transition were
not recorded.

## Protocol and Observed Timings

The unchanged harness alternates pair order, takes three 1×1/1-spp startup
probes, then one full warm-up and five measured full renders per variant.
All measured repeats are retained. Independent seed zero, camera, sensor,
transport depth and sample budget match within each pair. The reference
executable uses 1,024-sample batches. Driver caches were warm from validation.

```sh
python3 scripts/benchmark_core_textures.py \
  --baseline .cache/conductor-baseline/build/release/rypath_pbrt_reference \
  --candidate build/release/rypath_pbrt_reference \
  --work-dir .cache/conductor-performance-grazing-fix-20261115T054433Z \
  --repeats 5 --sample-scale 16
```

Wall times include parsing, translation, input decoding, Metal pipeline and
acceleration structure creation, rendering, accumulation and linear PFM output.
Times below are median (minimum–maximum) seconds after warm-up. RSS is median
process peak in MiB from `/usr/bin/time -l`; it does not isolate GPU allocations.
These are descriptive mixed-power observations.

| Legacy pair | Raster / spp / depth | Baseline seconds | Candidate seconds | Baseline / candidate RSS |
| --- | --- | --- | --- | --- |
| Diffuse | 64×64 / 65,536 / 1 | 3.260 (3.162–3.277) | 3.208 (3.196–3.251) | 33.36 / 33.09 |
| Image EWA | 8×8 / 2,097,152 / 1 | 2.683 (2.374–2.839) | 2.446 (2.345–3.337) | 60.61 / 60.50 |
| Adapted Cornell | 64×64 / 32,768 / 65 | 4.630 (4.215–4.840) | 4.821 (4.274–5.108) | 33.48 / 33.70 |

The following feature controls use the same candidate on both sides at 64×64,
65,536 spp and depth one. They are separate from the baseline/candidate pairs.
The graph reproduces direct neutral reflectance 0.375; image alpha is all one.

| Candidate-only control | Control seconds | Feature seconds | Control / feature RSS MiB |
| --- | --- | --- | --- |
| Direct RGB → mix graph | 3.240 (3.199–3.681) | 3.814 (3.631–3.984) | 33.23 / 60.69 |
| Opaque → image alpha | 3.212 (3.188–3.262) | 5.213 (5.155–5.230) | 32.86 / 64.75 |

Startup median (minimum–maximum) milliseconds include all three probes.

| Pair | Baseline/control ms | Candidate/feature ms |
| --- | --- | --- |
| Diffuse | 51.7 (51.5–52.9) | 51.6 (50.7–51.7) |
| Image EWA | 61.8 (60.5–61.9) | 63.4 (59.2–66.1) |
| Adapted Cornell | 55.0 (52.9–55.1) | 54.4 (52.6–56.9) |
| Direct / mix | 56.2 (55.0–57.9) | 69.6 (68.3–71.9) |
| Opaque / image alpha | 51.0 (50.1–52.8) | 64.7 (63.9–64.7) |

## Output Evidence and Artifact Identity

All 90 render processes succeeded. Every variant's full warm-up and five
repeats produced identical PFM hashes. Each of the five paired outputs also
matched byte for byte: maximum channel difference and RMSE were zero.

All wrappers, PFMs, process logs, individual timing/RSS records, scene/output
hashes and environment observations are retained in
`.cache/conductor-performance-grazing-fix-20261115T054433Z/`.

| Artifact SHA-256 | Baseline | Grazing-fix candidate |
| --- | --- | --- |
| Reference executable | `51f2608200464995f840612f50e290a2578e612f8390db8a82531e01a5abd601` | `2ac4a8f838dd3de52c6c3258ec7fe11ff8e922467e52ea709f729d5fe1cb575a` |
| Metal library | `30d0d325bfca8a412e7ca78c7db570d7ee2019f73ac768925c9b5950f54d2d56` | `9b0941743e38a7acfd6a6c27c63880e5014a2630e42d9625e8c6a02edef49858` |

`results.json` SHA-256 is
`f999f2ed19e53935b541a5b337b366f4033bf14d1d6fe2ce4e04c0b8bfc217c9`;
`environment.json` SHA-256 is
`0eec501f3c24c598f2e4f23f3bea4b51d0921fe18a79488a9d2bdc8e7454526c`.
The environment record includes source hashes and the exact dirty-tree paths.

## Historical Measurements and Scope

The earlier `6be5a4b` run used battery power throughout, from 41% to 37%, with
the same budgets and repeat count. Its medians are historical evidence for
that earlier shader, not the grazing fix:

| Historical pair | Baseline/control seconds | Candidate/feature seconds |
| --- | --- | --- |
| Diffuse | 3.301 | 3.311 |
| Image EWA | 2.560 | 2.521 |
| Adapted Cornell | 4.497 | 4.513 |
| Direct / mix | 3.321 | 3.717 |
| Opaque / image alpha | 3.499 | 6.226 |

Its complete ranges, startup/RSS data and hashes remain in
`.cache/conductor-performance-final-6be5a4b-20261115T050548Z/`. Its candidate
Metal library SHA-256 is
`cc44696c064ee4182f0822b74556ae2e0f3a45a4c8760d63a7f1458dedb5e481`.
The still earlier `.cache/conductor-performance/` run is preliminary.

These experiments cover three small legacy scenes and two existing feature
controls on one host and one seed. Conductor rendering cost, convergence,
equal-error timing, high-resolution throughput, complex scenes, clean-cache
specialization, build time and isolated GPU-memory use remain unmeasured.
