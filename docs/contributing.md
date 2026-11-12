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

Metal compilation and runtime checks will accompany actual backend code.
Hosted macOS compilation is not M4 Max performance evidence. Keep long corpus,
convergence, and benchmark studies outside the fast PR checks. Do not run
untrusted external PR code automatically on a personal workstation.
