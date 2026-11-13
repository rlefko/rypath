# Contributing

Read [AGENTS.md](../AGENTS.md) for project invariants and the review workflow.
Keep each PR focused on one purpose. Use a short Title Cased title with an emoji,
and a branch named `rlefko/` followed by the title in kebab case without the emoji.
Commit subjects begin with an emoji, contain at most one sentence, and have no
ending punctuation. Preserve logical commits; the maintainer reviews and merges
manually. Never enable auto-merge on their behalf.

## Build and test

Install CMake 3.25+, Ninja, and a C++20 compiler. From the repository root:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
```

Repeat those three commands with `release` or `asan-ubsan` for an optimized build
or AddressSanitizer/UndefinedBehaviorSanitizer validation. The sanitizer build
requires a compatible Clang or GCC toolchain and fails on undefined behavior.
Each preset uses its own `build/<preset>` directory. To choose a compiler, set
`CXX` before the first configure, or use an untracked `CMakeUserPresets.json`.

On macOS, these builds enable the Metal adapter and compile its MSL library.
GPU tests require device access by default. See [Metal execution](metal-execution.md)
for toolchain setup, portable-only builds, availability handling, and the required
M4 Max validation record. [Geometry checks](geometry.md) additionally require
compute ray-tracing support and compare native queries with a portable oracle.
[Rendering tests](rendering.md) validate spectral sampling, atomic film batches,
analytic expectations, independent CPU/GPU agreement, convergence, and PNG output.

[PBRT preflight tests](pbrt-profile.md) run on every platform and remain offline.
To check a scene without rendering or writing outputs:

```sh
./build/debug/rypath --check-pbrt path/to/scene.pbrt
ctest --preset debug --no-tests=error -R pbrt
```

Exit 0 means analysis completed within the profile, 2 means recognized features
are outside the profile, and 1 means an error or resource limit. Errors take
precedence. The unchanged Cornell fixture returns 0 within the current profile.

To render the [supported spectral profile](pbrt-rendering.md):

```sh
./build/debug/rypath --render-pbrt tests/fixtures/pbrt-render/multibounce.pbrt \
  --output build/pbrt.png --batch-size 8
```

Resolution, sample count, seed, and depth come from the scene. Unsupported
render settings return 2 before GPU acquisition; structural success alone is
not sufficient. The pinned CPU PBRT comparison is opt-in and stays outside
routine offline CTest/CI; see [validation](pbrt-render-validation.md).

CTest checks exact stdout, stderr, and exit status for no arguments, help,
version, unsupported options, and excess arguments. All test presets fail when
no tests are selected.

## Development hooks

Use Python 3.10+ and pre-commit 4.5.1. A repository-local environment is sufficient:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install pre-commit==4.5.1
.venv/bin/pre-commit install --install-hooks
.venv/bin/pre-commit run --all-files
```

Installation configures pre-commit, commit-message, and pre-push hooks. Hook
dependencies, including clang-format 18.1.8, are isolated and pinned by the
configuration. Formatting hooks may update files; review and stage those changes
before committing again. The pre-push hook configures/builds the Debug preset and
runs its CTest suite. Run that check directly with:

```sh
bash scripts/check_debug.sh
```

Hooks check formatting, whitespace, YAML, commit subjects, and prohibited local
handoff/archive paths. Inspect staged paths and diffs before every commit; ignore
rules do not protect forced additions or copied content. CI remains authoritative.

The archive guard inspects member names (and TAR link targets) in ZIP and TAR,
including gzip, bzip2, and xz aliases. Decoder-reported errors and recognized 7z,
RAR, and Zstandard TAR formats fail with a path-only diagnostic; decoder errors do
not stop checks of later files. This is not a full archive-integrity or arbitrary
content scanner. Commit-message checks fold the first paragraph as Git's subject;
a blank line separates the body. The hook honors configured strip and scissors
cleanup with fixed comment markers when Git exposes the relevant editor state.
It preserves draft comment lines with automatic markers. The history check
validates Git's stored subject and remains authoritative for automatic markers,
an explicit no-op editor, and invocation-only cleanup options.

## CI and review protection

Every PR to `main` runs formatting/policy, focused clang-tidy, Linux Clang
sanitizers, macOS builds/tests, and CodeQL. The workflow has no path filters.
`Foundation Gate` requires all mandatory jobs to succeed, including CodeQL;
failure, cancellation, or an unexpected skip blocks it. Superseded runs are
cancelled. CI records tool versions and pins action revisions and tool installs.

CodeQL analyzes a clean actual C++ build with compiler caching disabled. Its
successful upload is separate from alert-based merge protection. The repository
rules require up-to-date checks, resolved discussions, and a PR, and prohibit
force pushes and bypasses. CodeQL error-level findings and high/critical security
findings block merge. Formal required approvals remain zero because a PR author
cannot approve their own PR; maintainer review and manual merge are still required.

Linux CI uses LLVM 18 for clang-tidy. On newer macOS SDKs, LLVM 18 may need its
matching libc++ headers to avoid unsupported SDK builtins; this is a tooling
compatibility issue, not a reason to suppress diagnostics in project code.

## Renderer and importer changes

Use the installed renderer-validation skill for numerical, transport, sampling,
intersection, accumulation, and GPU scheduling changes. Use the PBRT-conformance
skill for scene translation and importers. Record assumptions, primary references,
validation evidence, and unavailable checks with the change.

The macOS job compiles the Metal library, runs a native Clang analyzer pass on
the Objective-C++ adapters, and attempts GPU tests. An absent device may be
reported as an explicit skip there; geometry tests also have a separate opt-in
skip for an observed lack of compute ray-tracing support. Neither option hides
execution or comparison failures. CodeQL covers portable C++, not Objective-C++
or MSL. A real M4 Max pass is required before presenting execution changes for
review. Hosted macOS compilation is not M4 Max performance evidence. Keep long corpus,
convergence, and benchmark studies outside the fast PR checks. Do not run
untrusted external PR code automatically on a personal workstation.
