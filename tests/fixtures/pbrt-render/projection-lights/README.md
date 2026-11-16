# Projection light comparisons

Eleven owned PBRT v4 entry points exercise constant image/point equivalence,
landscape and portrait nearest lookup, affine scale/shear, photometric power,
mixed lighting, rough reflection/transmission, bounded fog and geometry-free fog.
The square, landscape and portrait RGB PNGs are original GPL-3.0-only fixtures.
`assets/z03n2c08.png` is unchanged PNGSuite RGB8 by Willem van Schaik; its pinned
source, size and checksum are in `provenance.json`, with original terms in
`assets/PngSuite.LICENSE`. The external-map scene is an owned wrapper, not an
upstream complete-scene acceptance claim.

`manifest.json` pins entry points, while `dependencies.json` pins the complete
staged asset closure, including the external license. Every input uses 8x8 pixels,
4,096 spp, a CIE1931 sensor and independent sampling. The comparison script uses
rypath seeds 0–15 and PBRT seeds 16–31, with full-frame and four-quadrant RGB means
gated by five combined standard errors plus `2e-5 * max(1, abs(reference))`.
PBRT runs in world coordinates with two CPU threads. Other inputs remain shared
without overrides. See the [validation record](../../../../docs/projection-light-validation.md)
for measured evidence and numerical limits.
