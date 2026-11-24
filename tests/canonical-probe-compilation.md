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
