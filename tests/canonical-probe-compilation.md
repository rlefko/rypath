# Canonical Probe Compilation Boundary

The ordinary `metal.light_geometry_sampling` test uses a derived test library
with exactly five entrypoints: geometry proposals, offset rounding, density
conversion, canonical partition and canonical finalizer. All five executed
probe bodies remain byte-identical to the production-context probe source.

Generation removes the ten top-level kernel definitions from `render.metal`
and its four trailing entrypoint includes (`wavefront.metal`, `area_reuse.metal`,
`reconstruction_guides.metal`, `camera_medium.metal`). It retains all non-kernel
production text and the recursive helper includes, including actual packet
scattering, MIS, walk finalization, geometry and offset functions. No helper
math, `Resources`, or `Vertex` representation is copied or rewritten. The probe
input changes its renderer include and omits the separate full-context suffix
beginning at the uniquely checked original-retention-reproducer marker.

The bounded lexical scanner masks comments and quoted literals before balancing
kernel braces. It accepts only the exact reviewed kernel inventory and include
tail, rejects overlapping/malformed definitions and unexpected helper entrypoints,
and verifies that restoring the omitted slices reproduces both source files
byte-for-byte. Validation completes before output creation; derived files are
replaced atomically per file. Schema 2 `generation.json` records source,
retained-helper, generated-file and omitted/replacement-slice hashes. CMake
reruns generation when retained shader dependencies or the probe wire header
change. CPU tests cover actual-source reversibility, exact retained probe bodies,
lexical edge cases, malformed/missing/duplicate/unexpected layouts, recursive
helper rejection and failure without creating outputs.

The narrower entrypoint closure is a compiler-context isolation hypothesis,
not a claimed diagnosis or fix for the hosted backend. Ordinary probes still
use opaque production resource types but do not trace rays; previous successful
hosted dispatches mean absent compute ray tracing alone does not justify a skip.
Compiler, pipeline and numerical failures remain failures.

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
was not a complete remedy: hero run `45511242742` still failed ordinary
canonical-partition pipeline creation with `CompilerError 2` despite the bound,
while PR #62 at `abfde03` passed. The current change therefore isolates all
unrelated entrypoints rather than attributing the failure solely to private-state
allocation. Apple's
[`maxTotalThreadsPerThreadgroup` documentation](https://developer.apple.com/documentation/metal/mtlcomputepipelinedescriptor/maxtotalthreadsperthreadgroup)
describes the explicit compiler bound and automatic resource-based default.
The host's existing `group_width` respects the resulting pipeline maximum.

No arithmetic, expected values, assertions, availability handling, fallback, or
renderer kernel changes accompany the annotations or generated entrypoint isolation. The original
full-context command and coverage remain in place. Native focused validation
and a fresh hosted run are required; native success alone does not establish a
fix for the hosted compiler.

Historical native check on 2026-11-24, base `d08b85c` plus those annotations: normal Debug
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

That is historical native evidence for the annotations, not validation of the
new five-entrypoint generation, hosted compiler acceptance or a performance result.
Fresh hosted acceptance for the current generation remains outstanding.

## Five-entrypoint native validation

On 2026-11-24, base `daa494d` plus the generator/dependency changes passed a
fresh normal Debug configure/build on Apple M4 Max (`applegpu_g16s`), macOS 26.6 /
Xcode 26.6. Both Metal availability options remained OFF. The same focused
command above, with `MTL_DEBUG_LAYER=1`, passed all three tests with zero skips
in 25.02 seconds: generation (seven CPU contracts), ordinary geometry/canonical
probes (12.18 seconds), and the unchanged full-context command (12.25 seconds).
The ordinary probe checked 81,920 production-adapter proposals. The full-context
summary retained eight contexts and 24 MIS strategies, with zero bad-physical,
wrong-nested-reflectance or typed-field errors.

The schema 2 generated manifest records five entrypoints, 62 retained helper
files and 14 renderer / two probe edits. CPU tests independently restore all
omissions, compare all retained probe bodies and check source/helper/output
hashes. These are native regression and source-boundary checks; the virtual
Xcode 16.4 compiler failure still requires a fresh hosted run. No hosted success
or performance improvement is inferred from these results.
