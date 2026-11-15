# Earcut polygon triangulation

Upstream: https://github.com/mapbox/earcut.hpp
Revision: `2fbafada15dbe5350c606426e17111b91a964e55` (2026-10-31).
`include/mapbox/earcut.hpp` is unmodified and ISC licensed; retain `LICENSE`.
`SHA256SUMS` records the shipped header and license.

The OBJ adapter uses the pinned single-ring `mapbox::detail::Earcut<uint32_t>`
object so its bounded node/sort buffers are reused across faces. It never uses
holes or Delaunay refinement. Input is validated as a simple planar polygon and
projected onto its dominant plane; repeated, reversed, degenerate and crossed
boundaries fail. Original collinear boundary corners are reinserted by splitting
boundary triangles so their normal/UV samples survive. Winding, triangle area,
centroid containment, edge incidence and original boundary coverage are checked
before and after binary32 narrowing. Uncertain orientation estimates fail; exact
binary64 determinant signs use project-owned error-free expansion arithmetic
following Shewchuk's robust-predicate method (https://www.cs.cmu.edu/~quake/robust.html).
The area-weighted normal synthesis is a declared importer choice over original
polygon planes, independent of the triangulation diagonal.

Earcut's documentation explicitly does not guarantee valid triangulation of
arbitrary malformed polygons. Parser success or an index count alone is never
accepted as a geometric certificate. The wrapper enforces face-arity and aggregate
quadratic work bounds and conservatively precharges this pinned implementation's
node-pool/index/sort workspace. No library is fetched during configuration.
