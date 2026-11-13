---
name: rypath-renderer-validation
description: Validate rypath changes to transport, BSDFs, spectra, media, sampling, intersections, progressive accumulation, or GPU scheduling using estimator checks and measured numerical evidence. Skip prose-only and ordinary UI changes.
---

Read applicable project instructions and the implementation/validation records for the affected component. Keep the validation proportional to the change; do not build a complete CPU renderer before a small Metal rendering slice can be tested.

State whether the change affects the target physical model, the estimator, numerical behavior, or only execution. For estimator changes, identify random variables, support, PDFs and weights, delta events, and any stopping/selection rule. Tests supply evidence; they do not prove unbiasedness. Preserve independent raw accumulation and explicitly label denoised or approximate output.

Select checks that can reveal the actual failure:

- Intersections: compare closest-hit and occlusion against an independent brute-force or high-precision oracle on small geometry; include grazing/shared-edge/parallel-ray and transformed-instance cases. Conservative traversal and primitive intersection must agree.
- BSDFs: sampling/PDF consistency, energy and reciprocity under the correct convention, grazing incidence, near-smooth limits, and total internal reflection. A single-scattering microfacet model is not expected to pass a multiple-scattering white-furnace identity.
- Spectra/media: deterministic spectral quadrature or Beer–Lambert cases; wavelength weights, refraction dispersion, medium transitions, majorants, and phase/light sampling. Do not clamp broken majorants or invalid contributions into apparent success.
- Accumulation: stable sample indices, queue reordering, checkpoints/resume, cancellation, and invalidation; no duplicated, omitted, or selectively discarded expensive paths.
- GPU execution: verify resource lifetimes, completion and memory ordering, queue capacities, host/shader ABI, and actual backend capabilities. Overflow must fail visibly or recover without dropping work.

Compare CPU and GPU results statistically with justified tolerances, not universal bitwise pixel identity. Use independent seeds/randomizations, and select thresholds before observing the result. Compare matched models, camera/sensor settings, sample budgets, and output spaces. Diagnose failures rather than rerunning until one seed passes.

Use exact checks for deterministic contracts such as integer sampling, buffer layout, and sample accounting; use numerical or statistical tolerances for the floating-point quantities that need them. Follow the [Metal execution policy](../../../docs/metal-execution.md) for strict hardware validation and hosted availability skips. A sandbox reporting no device is not evidence of a hardware failure or a passing GPU test; keep that observation separate from actual device execution. Find the affected component's validation record through [architecture](../../../docs/architecture.md), and identify the revision and environment for newly measured evidence.

For performance changes, measure complete time to raw-image quality as well as build time, memory, and throughput. Include warm-up and repeated measurements, retain the baseline, and record scene/revision, hardware, OS/toolchain, resolution, and settings. State scene-specific regressions and experimental scope. A paper's speedup or an SAH score does not establish a speedup on M4 Max. Hardware-native structures remain the default until custom alternatives show reproducible benefit.

Return the changed mathematical/behavioral contract, relevant tests and commands actually run, concise results, and remaining validation gaps. Update public validation/benchmark records when they add reusable evidence. Do not publish private session instructions.

Primary references to consult when relevant:

- MIS and transport: https://www.graphics.stanford.edu/papers/veach_thesis/
- Microfacet geometry: https://jcgt.org/published/0003/02/03/paper.pdf
- GGX sampling: https://jcgt.org/published/0007/04/01/
- Spectral null scattering: https://cs.dartmouth.edu/~wjarosz/publications/miller19null.html
- Robust triangle tests: https://jcgt.org/published/0002/01/05/
- Metal structure construction: https://developer.apple.com/videos/play/wwdc2023/10128/
