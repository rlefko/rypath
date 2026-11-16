# Goniometric light comparisons

Nine focused PBRT v4 inputs exercise constant-map point equivalence, asymmetric
nearest equal-area lookup with rotation/reflection and PBRT Y/Z exchange,
spectral power normalization, additive mixed lighting, rough reflection and
transmission, bounded fog, geometry-free fog, and an external RGB map.
The gray constant and asymmetric PNGs are original GPL-3.0-only fixtures.
`assets/z03n2c08.png` is unchanged PNGSuite RGB8 by Willem van Schaik;
its pinned source, size and hash are in `provenance.json`, and its original
terms are retained in `assets/PngSuite.LICENSE`. This is an owned wrapper,
not conformance to an upstream complete scene.

`manifest.json` pins entry points; `dependencies.json` pins complete staged
asset closure including the external license. Each scene uses 8×8, 4096 spp,
a CIE1931 sensor and independent sampling. The comparison script fixes seeds
0–15 for rypath and 16–31 for PBRT and gates full-frame plus four quadrant RGB
means at five combined standard errors plus `2e-5 * max(1, abs(reference))`.
Reference execution explicitly uses world coordinates and two CPU threads.
Inputs are shared unchanged; no measured comparison is claimed by these assets.
