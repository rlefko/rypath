---
name: rypath-pbrt-conformance
description: Implement or review rypath PBRT v4 scene compatibility, OBJ/MTL and other mesh importers, scene translation, and external test-corpus coverage. Use for semantic import changes, not ordinary renderer optimization.
---

Read the pinned format/profile and compatibility matrix before changing parsing or translation. If they do not exist, establish a small explicit profile as part of the change. Keep syntax acceptance, faithful rendering, approximations, and unsupported features distinct. Parser success is not rendering conformance.

Resolve semantics using the official format and pinned upstream implementation. PBRT directives represent stateful scene construction, not just a list of objects. Test nested state, transforms/handedness, named definitions, instance references, Include/Import distinctions, resource resolution, and diagnostic source locations. Follow PBRT's actual path rules rather than imposing a generic nested-file-relative convention. Verify environment-map projection and spectral/color conventions when relevant.

Use synthetic fixtures that isolate the changed behavior, then a small pinned external example that exercises it. Compare with the upstream parser or renderer only with matched semantics and explicitly recorded overrides. An image that looks plausible is not sufficient. Preflight should enumerate missing assets and unsupported render features instead of silently degrading them.

For OBJ/MTL, cover independently indexed attributes, negative indices, omitted components, concave polygon triangulation, smoothing and group boundaries, material libraries, and texture options. Define nonplanar/degenerate policies. Document conversion of legacy material models. Track points, lines, free-form curves and surfaces separately: parsing their records does not mean they are rendered. Do not call a polygon-only implementation full-spec OBJ support. Diagnose historical `csh` directives without executing commands; constrain and cycle-check `call` inclusion. Document these deliberate execution restrictions separately from geometric compatibility.

For other adapters, verify their real capabilities at the pinned version. Required glTF extensions must be supported or rejected clearly; distinguish geometry import from faithful materials, animation, and scene composition. PLY tests should exercise property ordering/types and supported ASCII/binary variants. Decode malformed inputs with bounded memory and useful errors.

Maintain a corpus manifest with upstream revision, entry point, complete dependency closure, hashes, acquisition size, feature profile, comparison settings, and asset terms. Keep large assets in an external cache; use small owned fixtures in routine CI. Reusing open-source loader code or test scenes requires preserving their own notices, not relabeling them as project-authored assets.

Before reporting completion, list newly supported semantics, intentionally deferred constructs, diagnostic behavior, tests actually run, and exact corpus coverage. Full pinned-corpus support requires every renderable entry point at the pinned upstream revision to be inventoried and validated, not just the compatible selected subset. Selected subsets are appropriate for milestone acceptance; a skipped unsupported scene is not a pass for full coverage.

Consult only the sources relevant to the change:

- PBRT v4 format: https://www.pbrt.org/fileformat-v4
- Scene processing: https://pbr-book.org/4ed/Processing_the_Scene_Description/Managing_the_Scene_Description
- Upstream source: https://github.com/mmp/pbrt-v4
- Scene collection: https://github.com/mmp/pbrt-v4-scenes
- Original OBJ specification: https://paulbourke.net/dataformats/obj/obj_spec.pdf
- TinyObjLoader coverage: https://github.com/tinyobjloader/tinyobjloader
- glTF 2.0: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- Assimp formats: https://github.com/assimp/assimp/blob/master/doc/Fileformats.md
