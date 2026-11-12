# Project status

## Current milestone

Milestone 1: project foundations implemented and awaiting maintainer review in
[PR #1](https://github.com/rlefko/rypath/pull/1). The executable only provides help
and version information. The PR has not been merged.

## Validation

Local Debug, Release, and ASan/UBSan builds each pass all five CLI tests on an
Apple M4 Max with macOS 26.6, Xcode 26.6 / Apple Clang 21, CMake 4.0.3, and Ninja
1.13.2. Tests verify exact output and exit status; an empty test selection fails.

Seven synthetic policy tests pass, including staged ZIP/TAR checks and detection
of prohibited paths added and later removed in PR history. The installed hooks
validate commit subjects and formatting. Independent review found no blocking
implementation issues. The final gate's success/failure/cancelled/skipped cases
were exercised locally.

The [initial hosted run](https://github.com/rlefko/rypath/actions/runs/43271220189)
passed in 93 seconds on November 12, 2026. Linux Clang 18.1.3 passed five tests in
each of Debug, Release, and ASan/UBSan; hosted macOS Arm64 with Apple Clang 17
passed five Debug tests. CI used CMake 4.0.3 and Ninja 1.13.0. Formatting, policy
fixtures, clang-tidy, and the required final gate all passed.

CodeQL compiled and analyzed the C++ source, processed 58 rules with no reported
findings, and passed its separate code-scanning check. The active `main` rules
were read back and verified: PRs, current required checks, resolved discussions,
CodeQL error/high-security thresholds, and no force pushes, deletion, or bypasses.
Formal required approvals are zero; maintainer review and manual merge remain
required. The PR shows the checks for its latest revision.

No renderer, scene importer, GPU numerical check, or performance benchmark has
been completed.

## Next bounded task

After review and merge of the foundation PR: Metal capability reporting and a
minimal validated GPU execution path. See the [ordered PR plan](roadmap.md).
