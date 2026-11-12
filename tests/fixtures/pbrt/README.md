# PBRT preflight fixture manifest

| Field | Value |
| --- | --- |
| Upstream repository | `windingwind/web-falcor` |
| Revision | `e969701f3ebc0238003150dc0c0e7c3cc93aa1fc` |
| Entry point | `tests/gpu/assets/pbrt/cornell-box.pbrt` |
| Scene | 7,253 bytes, SHA-256 `b2fc6654c966468119187d169a76ecec82e169a1058f3344e27e5103189d4e43` |
| Adjacent notice | 370 bytes, SHA-256 `520b18c04d27fd9abca81364746178a9a3d52cff3c6204de05be5b68e547ee43` |
| Acquisition and expanded sizes | 7,623 bytes with notice, 7,253 scene bytes parsed |
| Scene input dependencies | None |
| Asset terms | CC0, as recorded in the preserved upstream notice |
| Comparison settings | Byte-for-byte upstream input, structural preflight with default limits, zero rendering overrides |
| Expected inventory | 8 named materials, 8 indexed triangle meshes, 36 triangles, 0 object placements, 0 input dependencies |
| Expected result | Complete syntax and dependency inventory, incomplete checked structure, 17 unsupported diagnostics, exit 2 |
| Unsupported features | Sobol sampler, eight mesh normal arrays, eight mesh UV arrays |

The web-falcor notice and Rendering Resources attribute the scene to Benedikt
Bitterli. Rendering Resources credits Matt Pharr for converting the PBRT scenes
to v4. Both vendored files retain their upstream bytes. [Pinned scene](https://raw.githubusercontent.com/windingwind/web-falcor/e969701f3ebc0238003150dc0c0e7c3cc93aa1fc/tests/gpu/assets/pbrt/cornell-box.pbrt),
[pinned notice](https://raw.githubusercontent.com/windingwind/web-falcor/e969701f3ebc0238003150dc0c0e7c3cc93aa1fc/tests/gpu/assets/pbrt/LICENSE.txt),
[Rendering Resources](https://benedikt-bitterli.me/resources/).

The [render fixture manifest](../pbrt-render/README.md) records the edits to
`cornell-box-adapted.pbrt` for matched triangle-area-light comparisons.
