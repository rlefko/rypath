# Canonical Probe Compilation Boundary

The ordinary `metal.light_geometry_sampling` test uses a derived test library.
Its renderer input is the current production `render.metal` with exactly one
unrelated `reconstruction_guides.metal` include omitted. Its probe input differs
only in the include naming that derived renderer. Production MIS/scattering,
partition/finalizer bodies, geometry proposals, expected values, and failure
handling remain unchanged. Generation fails if either expected include is absent
or duplicated; `generation.json` records original and generated source hashes.

This isolates an observed hosted macOS 15.8.2 / Xcode 16.4 pipeline-compiler
failure after guide argument-buffer growth. The same canonical probe previously
ran on hosted devices reporting no compute ray tracing, so absence of that API
does not justify skipping it. No compiler, pipeline, or numerical failure becomes
an availability skip. The precise older compiler defect remains unresolved until
the changed test executes successfully in fresh hosted CI.

`metal.canonical_full_context` retains its original whole-production-TU library,
command, physical/material checks, and explicit geometry capability policy. It
provides the complementary integration check on supported hardware. This test
isolation is not a change to the renderer or evidence of production equivalence
on an unsupported device.

CPU checks:

```sh
python3 -B tests/test_generate_canonical_probe.py
ctest --preset debug --no-tests=error -R '^infrastructure\.canonical_probe_generation$'
```

The isolated shader can be compiled without dispatching it:

```sh
cmake --preset debug
cmake --build --preset debug --target rypath_canonical_probe_shader
```

Actual acceptance also requires `metal.light_geometry_sampling` on hosted Xcode
16.4 and both geometry/canonical tests on native hardware. Local compilation alone
does not establish that the hosted runtime pipeline compiler is fixed.

## Bounded probe threadgroups

Hosted PR #62 run `45506601551`, macOS job `138843304889`, failed creating
`rypath_probe_canonical_partition` with `CompilerError 2` after the 288 offset
and 32 density cases passed. The runner reported Apple Paravirtual `air64_v27`,
macOS 15.8.2 and Xcode 16.4. The same shader/probe source had passed predecessor
CI; #62 changed no shader code. The earlier include isolation therefore does
not by itself resolve this hosted compiler failure.

Only the partition and finalizer probe entrypoints now declare
`[[max_total_threads_per_threadgroup(32)]]`. Their fixtures contain eight and
11 independent rows, respectively, and each dispatch adds one bounds-guard
lane: nine and 12 threads total. They retain multiple large private `Vertex`
copies. Bounding the compiler's threadgroup sizing may avoid excessive private
state allocation on the virtual backend; that resource-allocation explanation
remains a hypothesis until fresh hosted validation. Apple's
[`maxTotalThreadsPerThreadgroup` documentation](https://developer.apple.com/documentation/metal/mtlcomputepipelinedescriptor/maxtotalthreadsperthreadgroup)
describes the explicit compiler bound and automatic resource-based default.
The host's existing `group_width` respects the resulting pipeline maximum.

No arithmetic, expected values, assertions, availability handling, fallback, or
renderer kernel changes accompany these two test-only annotations. The original
full-context command and coverage remain in place. Native focused validation
and a fresh hosted run are required; native success alone does not establish a
fix for the hosted compiler.

Native check on 2026-11-24, base `d08b85c` plus these annotations: normal Debug
configure/build passed on Apple M4 Max (`applegpu_g16s`), macOS 26.6 / Xcode 26.6.
With `RYPATH_ALLOW_NO_METAL_DEVICE=OFF`,
`RYPATH_ALLOW_NO_METAL_RAY_TRACING=OFF`, and `MTL_DEBUG_LAYER=1`, the generator,
ordinary geometry probe and unchanged full-context command all passed (3/3,
zero skips, 23.71 seconds):

```sh
cmake --preset debug
cmake --build --preset debug
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose \
  -R '^(infrastructure\.canonical_probe_generation|metal\.light_geometry_sampling|metal\.canonical_full_context)$'
```

This is native regression evidence for the patch, not hosted compiler acceptance
or a performance result. A fresh hosted run remains outstanding.
