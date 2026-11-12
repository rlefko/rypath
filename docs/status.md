# Project status

## Current milestone

Milestone 1: project foundations. The first foundation PR is in progress and has
not been merged. The executable only provides help and version information.

## Validation

Local Debug, Release, and ASan/UBSan builds each pass all five CLI tests on an
Apple M4 Max with macOS 26.6, Xcode 26.6 / Apple Clang 21, CMake 4.0.3, and Ninja
1.13.2. Tests verify exact output and exit status; an empty test selection fails.

Seven synthetic policy tests pass, including staged ZIP/TAR checks and detection
of prohibited paths added and later removed in PR history. The installed hooks
validate commit subjects and formatting. Independent review found no blocking
implementation issues. The final gate's success/failure/cancelled/skipped cases
were exercised locally.

Hosted CI, CodeQL, and repository-protection evidence will be recorded after
verification. No renderer, scene importer, GPU numerical check, or performance
benchmark has been completed.

## Next bounded task

After review and merge of the foundation PR: Metal capability reporting and a
minimal validated GPU execution path. See the [ordered PR plan](roadmap.md).
