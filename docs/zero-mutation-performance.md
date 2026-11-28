# Zero-mutation Area reuse on M4 Max

The target is one complete native 1920×1080 denoised frame in 50 ms on Cornell
and Prism, or 100 ms on Pavilion day and night. Each pixel receives one fresh
four-wavelength visible-hero path. All measurements below include authored
materials, spectral dispersion and depth, native Metal traversal, Area
film/lens sampling and zero mutations in the Area arms. Bare PT has no Area
reuse or reconstruction. Engine startup, readback and EXR writing are timed
separately from the complete-frame CPU wall interval.

## Where the frame goes

On `d9ffd80`, six static arms each used seed 61002, two warmups and three
measured frames. All entries are mean complete wall time in milliseconds.
Bare PT and fresh-only Enhanced return the raw estimator. Contribution Area
uses reciprocal16. The three full Enhanced arms use paired Gaussian maps,
prefix replay and the sequential suffix cache.

| Scene | Bare PT | Enhanced fresh only | Contribution + signed | Four maps + signed | One map + signed | One map + native MetalFX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cornell | 117.2 | 260.8 | 663.5 | 841.5 | 440.7 | 418.4 |
| Prism | 126.8 | 293.2 | 667.1 | 1272.1 | 577.9 | 557.2 |
| Pavilion day | 417.6 | 617.7 | 1727.5 | 2387.1 | 1049.1 | 1035.1 |
| Pavilion night | 442.5 | 667.1 | 1755.4 | 2434.0 | 1095.2 | 1088.4 |

These short runs isolate large costs. Fresh selection and path evaluation
already exceed the complete-frame budget on every scene, before spatial reuse
or filtering. The sustained comparison below uses a longer temporal history.

An earlier instrumented run on `3dc8b9d` isolates one warm frame. Summed GPU command
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

## Sustained native frame time

The paired Release runs used `a19feb2`, generic shading and signed temporal
reconstruction on the 40-core M4 Max (macOS 26.6, Xcode 26.6). For each arm
and scene, seeds 61002–61004 each supplied 96 measured frames after eight
warmups. Every frame completed 2,073,600 fresh paths. The static columns
use all 288 measured frames per arm. FPS is the reciprocal of mean wall time.
The pooled P95 is descriptive because successive frames share history.

| Scene | Four maps, static ms | One map, static ms | One map P95, ms | One map FPS | Static speedup | One map, translation only ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cornell | 884.9 | 453.9 | 461.4 | 2.20 | 1.95× | 742.5 |
| Prism | 1303.1 | 584.0 | 591.7 | 1.71 | 2.23× | 1039.4 |
| Pavilion day | 2585.4 | 1144.6 | 1161.2 | 0.87 | 2.26× | 1850.5 |
| Pavilion night | 2635.7 | 1188.9 | 1205.3 | 0.84 | 2.22× | 1874.6 |

The motion runs used the same seeds, warmup and frame count. The last column
includes only the 24 translating frames per seed. Both arms use fixed cap 20
and zero mutations. One map combines two candidates and four maps combine
five. The speedup compares equal fresh sample counts. The held-out quality
study below measures the effect on error.

A second Release build on `fe1b28c` repeated the static one-map pipeline
for 96 measured frames on seed 61002. Mean complete frame times were
453.75 ms on Cornell, 582.31 ms on Prism, 1150.69 ms on Pavilion day and
1186.58 ms on Pavilion night.

## Spatial cost and remaining fresh work

A separate Release run on `d9ffd80` split each GPU stage for timing. Each arm
used four warmups and one measured static frame at native 1080p. The columns
below sum GPU command intervals in each phase. The two spatial columns use
the exhaustive and one-map estimators respectively.

| Scene | Fresh, one map (ms) | Four spatial maps (ms) | One spatial map (ms) | Fresh path-step calls |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 256.9 | 542.9 | 139.3 | 4,215,343 |
| Prism | 291.7 | 917.5 | 237.4 | 5,099,167 |
| Pavilion day | 628.2 | 1,819.4 | 415.9 | 5,258,782 |
| Pavilion night | 657.5 | 1,804.3 | 425.3 | 5,258,290 |

Both estimators executed the same number of fresh path steps on each scene.
The measured one-map spatial phase took 23–26% of the four-map phase.
Even fresh discovery alone exceeded the complete-frame budgets by 5.1–6.6×.
The dominant kernel includes ray queries, scattering, spectral evaluation
and reservoir state updates. Its duration cannot separate those costs.
An uninstrumented short Cornell run measured 433.8 ms of GPU time inside a
440.7 ms complete frame. Readback and file encoding occur after that interval.

These runs used the native triangle path on the M4 Max. Host Release code
uses `-O3`, and the production Metal transport shader uses strict floating
point and `-O1` to preserve nested coating evaluations. Metal shader validation
and capture were disabled for the budget runs. A further execution experiment
would specialize the current shading event for a mixed scene's supported
materials and measure native register allocation, spill storage, and full
frame time. The whole-scene diffuse specialization above only covers Cornell.

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

## Pavilion texture and reflection detail

At native 1080p, one additional daylight run fed 104 identical source frames
to each filter within each of its two arms, independent PT and one-map Area.
Each filter used its own input conversion, guides and history. We compared
frame 104 with a separate 256-SPP reference in signed linear luminance.
Four independent 64-SPP reference batches allowed an estimate of repeatable
texture contrast. Regions were chosen on a reference before scoring.

| Daylight region, medium-scale contrast | Temporal only | Signed with five spatial passes | Native MetalFX |
| --- | ---: | ---: | ---: |
| Left concrete wall | 1.00 | 0.92 | 0.92 |
| Central green marble | 1.33 | 0.87 | 3.14 |
| Stone floor | 0.99 | 0.99 | 1.07 |

Values are reference-aligned gains in the difference between Gaussian bands
at 2 and 6 pixels. A gain of one matches estimated reference contrast. On the
left wall, signed and MetalFX had similar gains, yet their correlations to
reference detail were 0.64 and 0.96 respectively. Residual grain obscures the
wall in the signed output. Applying the signed spatial passes reduced both
contrast and band error on the marble and wall. Disabling the passes would
restore some contrast at the expense of more noise.

The nighttime pool-light reflection showed a different effect. In the fine
and medium bands, signed retained 95% and 96% of reference contrast, while
MetalFX retained 12% and 30%. The signed output had lower band error against
all four reference batches. The two nighttime filters ran in separate
processes. Their final raw pixels agreed exactly inside this crop, although
the comparison does not establish equal histories. In the same-input
daylight run, toggling the signed spatial passes left the water crop
byte-identical in both arms. These measurements cover fixed regions and
single native endpoints. Primary pinhole guides approximate the finite lens.

The signed filter uses albedo to guide filtering of combined radiance.
Schied et al. remove visible material modulation before filtering illumination
and restore it afterward. A focused follow-up is to retain the opaque diffuse
substrate contribution separately through reconstruction, then test the wall
and marble against the same references while protecting coated and reflected
light. This requires a suitable signal from transport rather than dividing
the combined spectral image by a primary RGB guide.
[Spatiotemporal Variance-Guided Filtering](https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/)

## Held-out spatial-reuse quality

Eight paired seeds (indices 100–107) ran both spatial estimators for 32 frames
with a fixed cap of 20. Static endpoints use one common 2048-SPP reference.
Motion uses four independent reference batches at each of nine camera poses,
128 SPP per pose in total. Cornell and Prism were 320×180 and Pavilion
was 320×170. Each raw frame was also given to signed reconstruction and
native MetalFX so filter comparisons within a run share the same input.

| Scene | Signed MSE, four maps | Signed MSE, one map | MetalFX MSE, four maps | MetalFX MSE, one map |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 0.01136 | 0.01421 | 0.01238 | 0.01177 |
| Prism | 0.04781 | 0.04983 | 0.00618 | 0.00639 |
| Pavilion day | 6.0904 | 6.0899 | 0.03043 | 0.03045 |
| Pavilion night | 9.0388 | 9.0151 | 0.09246 | 0.09299 |

All eight static Cornell signed endpoints favored four maps. Seven of eight
Prism signed endpoints did so. One-map signed MSE was also higher during
translation on all four scenes. On Pavilion night, the direction reversed
while the camera held still. As a descriptive time-to-quality check, each
seed's four-map final-four-frame mean supplied a target for the one-map
rolling mean. By frame 32 the signed one-map arm reached that target on
0/8 Cornell, 1/8 Prism, 6/8 Pavilion day and 4/8 Pavilion night seeds.
These measurements support an opt-in sampling tradeoff. A shorter frame
interval alone does not establish a speedup at equal error.

## Pavilion night highlight persistence

We traced a fixed screen-space grid on the roof, stone and reflected lights in
a 104-frame static sequence at native 1080p, seed 61002. The instrumented run
captured signed linear RGB from the retained raw and reconstructed frame. It
uses one shared spatial map, fixed Area cap 20 and a signed denoiser with a
64-frame history and five spatial passes. Each reference pixel averages four
independent 64-SPP batches. The trace readbacks are excluded from throughput
comparisons.

In the first roof patch, mean raw luminance jumped from 0.0273 to 1.1007 on
frame 5. By frame 104, mean raw luminance was 0.0307 against a 0.0292
reference. The signed reconstruction still measured 0.1223. Applying the
denoiser's nominal temporal weights directly to the recorded raw samples gave
0.1232. A second roof patch showed a similar decay after frame 59. These two
traces identify a large signal already present before reconstruction and
quantify how long the temporal filter retained it. They do not identify the
individual path that first produced it.

At one separate stone pixel, the final raw RGB was approximately
`(19.9, 8.4, -1.4)`, and the denoised result was `(534.7, 225.2, -38.6)`.
The independent reference at that pixel was near black. Whole-frame averages
conceal these localized errors. The reflected pool light remains a separate
quality check because suppressing bright samples can also remove authored
detail.

On this same native seed and frame, we varied only the fixed Area cap with
the signed denoiser held at 64 frames. The table uses full rectangular crops.
The trajectory above used a sparse fixed grid inside those crops. The dark
outlier count includes pixels whose maximum absolute RGB channel exceeds 10
while all three reference channels have magnitude below one.

| Output | Roof crop mean Y | Reflected lantern mean Y | Dark outlier pixels | Full-frame RGB MSE |
| --- | ---: | ---: | ---: | ---: |
| Fixed 20, signed | 0.0894 | 0.3324 | 205 | 0.3625 |
| Fixed 4, signed | 0.0471 | 0.3414 | 244 | 0.5281 |
| Fixed 1, signed | 0.0375 | 0.3414 | 253 | 0.5575 |
| Independent PT, signed | 0.0300 | 0.3461 | 247 | 0.6572 |
| Fixed 20, native MetalFX | 0.0315 | 0.2764 | 0 | 0.1869 |

The four reference batches place roof mean Y between 0.0290 and 0.0358 and
reflected lantern mean Y between 0.3247 and 0.3758. Shortening the fixed cap
helps the roof and increases the total number of dark outliers at this
threshold. MetalFX suppresses the outliers and attenuates the reflection.
These are separate-process captures. The two denoisers' inputs match exactly
inside the measured reflection crop. Global raw equality is scene dependent.

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

An additional experiment executed a second eligible fresh path step within
one shade dispatch. Counted and counter-free replay fixtures passed their
same-acceleration-structure checks. At 1080p on Cornell, an uninstrumented
off/on/on/off run gave median complete frame times of 439.60 ms off and
467.30 ms on, a 6.30% regression. Baseline endpoint drift was 0.31%.
This failed the prespecified limit of a 2% complete-frame regression.
The experiment was retained separately. Prism confirmed second-step
admission. We stopped before Prism warm timing or a fresh-stage speed claim.

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
