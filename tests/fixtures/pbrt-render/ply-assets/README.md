# PLY comparison assets

`sportscar-quad.ply` is the unmodified
[`sportscar/geometry/Plane_0000_m000.ply`](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/sportscar/geometry/Plane_0000_m000.ply)
from PBRT scenes revision `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`.
The [upstream notice](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/README.md#sportscar)
credits Yasutoshi Mori and licenses the Sportscar model under
[Creative Commons Attribution 2.0](https://creativecommons.org/licenses/by/2.0/).
These asset terms remain separate from rypath's GPL license. The upstream
exporter comment and all file bytes are preserved.

| Property | Value |
| --- | --- |
| Acquisition and expanded size | 412 bytes; no compression |
| SHA-256 | `91fa58d0b64fb8f5d2a49844cd5a1e64e7f71d52f904dd3e20aa76c866b5ba62` |
| Encoding | PLY 1.0, binary little-endian |
| Geometry | Four vertices, four normals, one cyclic quad, no UVs |
| Further input dependencies | None |

`../ply-external.pbrt` is an original GPL-3.0-only wrapper. It applies translation,
rotation and nonuniform scale to the narrow planar asset, with a pinhole camera,
constant environment and diffuse material. This establishes selected asset
coverage, not rendering support for the original Sportscar scene. The original
scene and its other assets are not downloaded.

`mixed.ply` and `curved-light.ply` are original GPL-3.0-only fixtures.
The first combines a nonplanar quad with unequal-length vertex normals and UVs
and a separate triangle. The second is a nonplanar emitting quad. Their wrappers
isolate mixed geometry and curved area emission, respectively.

All three wrappers use 8×8 pixels, 4,096 spp and the existing fixed independent
seed comparison protocol. The [dependency manifest](../dependencies.json)
pins each wrapper's complete input closure, sizes and hashes. The comparison
script verifies and stages these files under each relocated entry directory.
Configure, build and CTest perform no downloads.
