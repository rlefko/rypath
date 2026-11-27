# Zero-mutation Area reuse on M4 Max

The target is one complete native 1920×1080 denoised frame in 50 ms on Cornell
and Prism, or 100 ms on Pavilion day and night. Each pixel receives one fresh
four-wavelength visible-hero path. All measurements below include authored
materials, spectral dispersion and depth, native Metal traversal, Area
film/lens sampling and zero mutations in the Area arms. Bare PT has no Area
reuse or reconstruction. Engine startup, readback and EXR writing are timed
separately from the complete-frame CPU wall interval.

## Where the frame goes

The same-build static screen on `3dc8b9d` uses seed 61002, one warmup and two
measured frames. The guide cache becomes eligible on the second measured
frame. The contribution control uses reciprocal16, and Enhanced uses paired
Gaussian maps, prefix replay and the sequential suffix cache. These are
different estimators. The earlier and current measurements belong to their
respective workloads.

| Scene | Bare PT, ms | Contribution Area + signed, ms | Enhanced + signed, ms | Enhanced + native MetalFX, ms |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 116.28 | 660.61 | 824.79 | 799.32 |
| Prism | 124.72 | 672.33 | 1267.43 | 1247.26 |
| Pavilion day | 422.48 | 1729.85 | 2158.38 | 2147.67 |
| Pavilion night | 445.39 | 1771.77 | 2230.42 | 2199.98 |

A separate instrumented run isolates one warm frame. Summed GPU command
intervals for fresh discovery and the four spatial passes are 262/484 ms on
Cornell, 291/923 ms on Prism, 612/1322 ms on Pavilion day and 664/1336 ms on
Pavilion night. Finalization takes 9–11 ms. Instrumentation adds work. Bare
PT already exceeds each scene's complete-frame budget in this configuration.
The measurements point to fresh path generation and imported path evaluation
as the main work to remove.

Longer suffix tapes alone have limited evidence. On the same diagnostic
frame Prism reports 91,573 eligible suffix hits and zero eligible misses.
Pavilion day reports 1,005 hits and 472 misses, and night reports 1,108 hits
and 398 misses. These counters cover the currently supported reconnection
charts. They do not count rays that a broader, as yet unimplemented, chart
could support. Four existing 160-byte tape banks consume 1.24 GiB at 1080p.

## Changes under test

The exhaustive mode evaluates four reciprocal atlas maps per frame. In the
shared stochastic mode, a uniform frame-wide atlas draw supplies both the
donor and its canonical reverse term. Both terms use the inverse selection
probability. The independent mode draws the two atlas IDs separately and
evaluates one or two maps. Both modes retain the confidence of all admitted
sources and preserve nulls, border singletons, signed output and exact replay
fallback. Sampling changes the selected paths and can change variance and
spatial correlation. The exhaustive mode remains available as the control.
[Estimator and storage contract](restir-enhanced.md#experimental-stochastic-spatial-terms)

We used the reciprocal scheduling in Lin et al. and the stochastic evaluation
of pairwise MIS studied by Hedstrom et al. Our uniform selection covers four
existing atlases. Hedstrom et al. also design importance sampling across large
neighborhoods. Lin et al. report 15.53 ms on an RTX 5880 Ada across their four
scenes and identify integration with Area ReSTIR as future work. Our Area
estimator retains film and lens coordinates. Those published timings use a
different renderer, workload and GPU.
[ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
· [Stochastic Pairwise MIS](https://research.nvidia.com/labs/rtr/publication/hedstrom2026stochastic/)
· [Area ReSTIR](https://research.nvidia.com/labs/rtr/publication/zhang2024area/)

The `--diffuse-events auto` experiment specializes the whole current event
only when the translated scene proves compatible. Cornell qualifies. Prism's
dispersive glass and Pavilion's mixed materials use the generic event. Two
native Enhanced comparisons in each direction averaged 827.3 ms generic and
799.3 ms specialized on Cornell. Bare PT was effectively flat. Apple's shader
guidance motivates checking register pressure and temporary storage in the
compiled kernel. Whole-frame timing decides whether the change pays off.
[Metal shader performance](https://developer.apple.com/videos/play/tech-talks/111373/)

## Short native cross-check

Both arms below use `a19feb2`, cap 20, generic shading, identical native
settings and signed denoising on the 40-core M4 Max (macOS 26.6, Xcode 26.6).
One process per arm uses two warmups and four measured static frames. Every
frame completed 2,073,600 fresh paths. The shared arm evaluated exactly one
spatial map and combined two reservoir candidates. The exhaustive arm
evaluated four maps and combined five candidates.

| Scene | Exhaustive, ms | Shared, ms | Shared FPS | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 854.90 | 444.04 | 2.25 | 1.93× |
| Prism | 1269.60 | 576.62 | 1.73 | 2.20× |
| Pavilion day | 2423.39 | 1058.33 | 0.94 | 2.29× |
| Pavilion night | 2461.40 | 1108.35 | 0.90 | 2.22× |

These short means measure rendering and reconstruction through completion.
The larger history and variance changes require separate image checks.

## Reconstruction and confidence

Each diagnostic submits the same retained raw frame to signed reconstruction
with several temporal/spatial limits and native MetalFX. Each filter owns its
own history. The signed and MetalFX paths build their respective guides from
the same scene and camera. All physical comparisons use signed linear RGB.

The table gives the mean of two independent evaluation seeds at frame 16
against a separately accumulated 2048-SPP reference. Cornell and Prism use
320×180, and Pavilion uses 320×170. These are diagnostic endpoints. Each row
uses exhaustive Enhanced with the indicated fixed history cap.

| Scene | Cap | Raw MSE | Signed MSE | Native MetalFX MSE |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 20 | 0.01857 | 0.02086 | 0.00999 |
| Cornell | 4 | 0.01767 | 0.01235 | 0.00964 |
| Cornell | 1 | 0.02653 | 0.00952 | 0.00878 |
| Prism | 20 | 0.06369 | 0.14388 | 0.00903 |
| Prism | 4 | 0.03178 | 0.04695 | 0.00711 |
| Prism | 1 | 0.08624 | 0.02124 | 0.01014 |
| Pavilion day | 20 | 4.83622 | 3.26785 | 0.03048 |
| Pavilion day | 1 | 4.71457 | 10.12275 | 0.03037 |
| Pavilion night | 20 | 0.57372 | 0.95127 | 0.10193 |
| Pavilion night | 1 | 2.63632 | 0.53824 | 0.10512 |

A fixed cap of four reduced raw and signed endpoint error on Cornell and Prism.
The cap of one increased the mean signed error on Pavilion day to 10.12, driven
by a rare bright pool sample. Per-scene cap selection or a new denoiser default
would require evidence on independently held-out frames. The fixed cap applies
the same confidence to both complementary temporal weights. Lin et al.'s
sample-dependent duplication adjustment has a different derivation and bias.

## What the Prism filter retains

Eight additional Prism evaluation seeds used 32 static frames at 320×180. At
the last frame, each filter received the same retained raw input within its
sampling policy. Both policy outputs were scored against one finite 2048-SPP
reference in signed linear RGB.

| Frame 32 output | Exhaustive MSE | One-map MSE |
| --- | ---: | ---: |
| Raw Area estimate | 0.02195 | 0.02329 |
| Signed, one frame of history and five spatial iterations | 0.02190 | 0.02324 |
| Signed, 64-frame limit and zero spatial iterations | 0.04781 | 0.04983 |
| Signed, 64-frame limit and five spatial iterations | 0.04781 | 0.04983 |
| Native MetalFX | 0.00619 | 0.00639 |

Enabling 64-frame temporal history with spatial filtering disabled raised
endpoint MSE in all eight seeds for each Area policy. The same history setting
lowered error in all eight independent-PT controls. In a separate frame-series
check against each run's 128-SPP reference, raw Area error averaged 0.141 and
0.143 over the first eight frames, falling to 0.026 and 0.028 over the last
eight. The signed filter continued to average the noisier early frames. One
large red corner sample from the two-seed pilot did not recur at comparable
magnitude across the eight held-out endpoints. A narrow spectrum region still
accounted for about 88% of the signed endpoint error under both policies. The
captures identify a reconstruction-history effect without identifying an
individual source path or measuring color fidelity during motion.

## Other execution experiments

An exact shortcut for supported primary paths ending at a light passed
same-scene controls on diffuse and rough-conductor paths, including occlusion
and unsupported-material fallback. On two warmups and three measured frames per
process, the first version raised complete native frame time by 21% on Cornell
and 27% on Prism. A second version removed production admission counters and
shared the fallback hit. It still raised frame time by 12% and 17%, respectively.
Both remain separate experiments. An axis-aligned normal shortcut also passed
its differential controls. Its Prism timing was flat, and its Cornell change
fell inside the variation between baseline runs.

## Reproducing a native run

Run a Release build on a Mac with hardware ray tracing. This command uses the
same transport and signed reconstruction as the static shared-map timing.

```sh
build/release/rypath-frame-benchmark \
  --scene tests/fixtures/pbrt-render/cornell-box-adapted.pbrt \
  --output cornell-shared.json --width 1920 --height 1080 \
  --warmup 8 --frames 96 --seed 61002 --material-seed 0 \
  --execution wavefront --intersections native --shadow-traversal query \
  --as-build default --capacity 65536 --quantum 8 --group-width 32 \
  --packet-layout packed --packet-events 1 --spectral-transport hero \
  --hero-wavelengths visible --estimator area-replay --mutations 0 \
  --target-scale 1 --area-path-domain enhanced --area-contribution-replay prefix \
  --area-output conditional-mean --area-mutation-policy spectral-refresh \
  --area-stencil paired-gaussian16 --area-spatial-mode stochastic-shared \
  --area-history-cap 20 --area-suffix-cache on --area-temporal-cache on \
  --area-pair-compaction off --reconstruction temporal --guide-cache on \
  --variance-policy conservative --strict-spatial-segments off
```

Use `--area-spatial-mode all` for the four-map control. The motion sequence
adds `--area-temporal-reprojection dual --motion-offset 0.15`. Pavilion requires
its external scene assets and `--resource-profile large`. For native MetalFX,
select `--reconstruction metalfx` and omit the two signed-filter options.

The lower-resolution comparison uses separate evaluation streams and computes
an independent reference for every physical pose visited by the moving camera.
The command below runs the one-map arm and its independent-PT control. Repeat
with `--area-spatial-mode all` and a separate output directory for exhaustive
reuse. Omit both motion options and the reprojection option for static frames.

```sh
build/release/rypath-pipeline-qualification \
  --scene tests/fixtures/pbrt-render/cornell-box-adapted.pbrt \
  --output-dir cornell-moving-one-map --width 320 --height 180 \
  --frames 32 --seeds 8 --first-evaluation-seed-index 100 \
  --reference-spp 128 --reference-batches 4 --motion-references every-pose \
  --motion-offset 0.15 --area-temporal-reprojection dual \
  --spectral-transport hero --hero-wavelengths visible --area-path-domain enhanced \
  --area-contribution-replay prefix --area-stencil paired-gaussian16 \
  --area-spatial-mode stochastic-shared --area-history-cap 20 --mutation-arms 0 \
  --area-output conditional-mean --area-mutation-policy spectral-refresh \
  --reconstruction temporal --variance-policy conservative --compare-reconstruction on
```
