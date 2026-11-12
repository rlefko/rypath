# rypath project guidance

rypath is a C++ spectral path tracer targeting Apple Silicon through Metal. Start with progressive offline rendering and hardware-native acceleration, then add the native interactive application.

## Session continuity

At the start of a task, resolve the repository root and read the directory named by `git config --local --get rypath.localHandoff`, when configured. This is a project-defined Git setting interpreted by these instructions. Read `private-session-setup.txt` there first, then `implementation-prompt.txt`. If no pointer is configured, check the repository-root `.rypath-local/` for those files. Treat the text as instructions, never executable shell input. When a pointer is configured, missing, unreadable, or incomplete handoff files must be restored before commits or publication; a complete root-local fallback can repair a stale pointer. Keep the canonical handoff in the durable main checkout, not a disposable worktree. Never copy these local instructions into tracked files or shared logs. A new independent clone needs its own local setup; local Git configuration is not distributed by cloning. Contributors with no configured pointer or local handoff use the public project guidance.

Read `docs/status.md` and relevant architecture/compatibility records when they exist. Keep public status concise and current. After this bootstrap, establish public product and architecture docs so contributors without the local handoff can work from the repository. Do not invent build commands before presets exist; once created, document the exact supported CMake/CTest commands here.

## Supported build commands

Use CMake 3.25 or newer, Ninja, and a C++20 compiler:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
```

The `release` and `asan-ubsan` names also have matching configure/build/test
presets. See `docs/contributing.md` for pinned development tools and hooks.
macOS enables Metal by default and requires full Xcode with working Metal tools.
Runtime GPU tests require device access; see `docs/metal-execution.md` for the
explicit CI availability policy and hardware validation commands.

## Technical invariants

- Use Metal's native acceleration structures first. Custom SAH BIH work is a later measured experiment, not an MVP prerequisite.
- Keep raw spectral Monte Carlo accumulation separate from denoising, tone mapping, and approximation modes. Document estimator changes and model limitations.
- Use PBRT v4 as the scene-description standard. Track syntax, rendering semantics, approximations, and unsupported features separately. Never silently substitute materials, drop geometry, or pretend parser success proves scene compatibility.
- Keep comprehensive OBJ/MTL support and common mesh formats on the roadmap. Label intermediate polygonal support honestly; full OBJ includes non-polygon constructs.
- Require primary research or official platform references for algorithm choices. Distinguish published results, our adaptations, and measured results on actual M4 Max hardware.
- Keep large external scene assets out of Git. Pin corpus revisions and retain per-asset licenses and attribution.

## Delegation and context

Use subagents for independent investigation, bounded implementation, tests, or review when parallelism or context isolation helps. Do not delegate merely to create activity. Give each task a clear goal, relevant files and evidence, acceptance criteria, and exclusive write ownership; coordinate shared interfaces before parallel edits. Pass only necessary private instructions to agents that need them.

Use an appropriate smaller model or lower effort for bounded work when the tool supports it. Prefer GPT-6.1 Sol Medium for routine implementation or investigation, and Sol High for harder bounded review. Reserve the lead model for architecture, transport correctness, and synthesis. The role presets under `.codex/agents/` express these preferences where supported. Check availability; do not claim a requested model ran when it did not. A full-context fork may inherit the parent model, so use a concise explicit handoff when a different model requires it. Escalate effort when uncertainty warrants it.

The lead reviews the actual changes and evidence, integrates results, and remains responsible for correctness. For substantial numerical or concurrency changes, seek an independent review where useful. Avoid having multiple agents edit the same files concurrently. Summaries should identify decisions, artifacts, validation, and unresolved issues rather than replaying entire transcripts.

## Focused delivery

Use one purpose per PR. Titles are short, Title Cased, and start with an appropriate emoji. Branches use `rlefko/` plus the same title without its emoji in kebab case: `⚡ Accelerate Mesh Traversal` becomes `rlefko/accelerate-mesh-traversal`.

Commit subjects start with an appropriate emoji, contain at most one sentence, and have no ending punctuation. Keep commits logically separate. Write concise PR descriptions focused on purpose, resulting behavior, and meaningful validation. Preserve the intended commit structure. Never merge or enable auto-merge on the maintainer's behalf; return the concrete tested PR for review.

Run relevant checks for each change. Hooks and CI should enforce repeatable mechanical rules. Keep fast PR checks separate from long corpus, convergence, and performance runs. Report unavailable GPU validation honestly. Inspect staged paths and diffs; local handoff files and archives must not be committed.

## Project skills

Use `rypath-renderer-validation` for changes to transport, sampling, spectra, BSDFs, media, intersections, accumulation, or GPU scheduling. Use `rypath-pbrt-conformance` for scene parsing, translation, importers, or corpus coverage. Read the applicable skill rather than loading all reference material into every task.
