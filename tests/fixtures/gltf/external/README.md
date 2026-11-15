# Selected upstream glTF fixtures

These small unmodified assets are from KhronosGroup/glTF-Sample-Assets at
`8f492390ddc7a2ae221a1369ef560589cf8c70fb`. `manifest.json` records every shipped
file, upstream path, size, hash, entry point and terms. No screenshot, full model
repository or large asset was acquired. Complete runtime payload: 2,745 bytes.

- Triangle: `Triangle/glTF/Triangle.gltf` and `Triangle.bin`, CC0-1.0 model,
  Marco Hutter, 2017. Its LICENSE and metadata retain CC-BY-4.0 terms.
- Box: `Box/glTF-Binary/Box.glb`, CC-BY-4.0, Cesium, 2017. Preserve its attribution
  when redistributing. LICENSE and metadata retain the upstream terms.

Retained upstream LICENSE files link complete legal texts. These licenses are
separate from GPL-3.0-only project code and wrappers. Assets are not relabeled as
project-authored. Tests establish selected parser/bundle acceptance, not full
Khronos corpus or rendering conformance. Triangle exercises external buffer
resolution and indexed positions. Box adds self-contained GLB, node composition,
normals, explicit stride and core metallic-roughness factors.
