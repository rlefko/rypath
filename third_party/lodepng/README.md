# LodePNG

The sources and license are from [LodePNG revision
`7b2893e35891c0c85bee5595caafaf3c2044b409`](https://github.com/lvandeve/lodepng/tree/7b2893e35891c0c85bee5595caafaf3c2044b409),
version 20261102. Copyright (c) 2005-2026 Lode Vandevenne. See [LICENSE](LICENSE)
for the zlib license.

| Upstream file | SHA-256 |
| --- | --- |
| `lodepng.cpp` | `32cdd7a395b5476a3fe4615305a99066dc4d37aaea6988643cca8b4eccc352cc` |
| `lodepng.h` | `a49b98b45ea603a05e2a77bd6b0320e7535a9c6e18aef8ce904692a3e02b87ab` |
| `LICENSE` | `bdafa318b8e637d600ca1b60545e894881977a16a16b74809c95e805701621e3` |

Local changes are marked `rypath extension`. When
`LODEPNG_MAX_BLOCKS` is defined, `lodepng_inflatev` counts all DEFLATE blocks
and stops before processing a block beyond that limit. The header declares
`LODEPNG_ERROR_BLOCK_LIMIT` (124), and the source maps that code to a diagnostic.
rypath sets the limit to 65,536 blocks per stream to bound work on streams with
many small or empty blocks.

`Adam7_getpassvalues` and `postProcessScanlines` compute pass offsets and
scanline padding lengths by widening the width to `size_t` before
multiplication. The noninterlaced path detects padding from the resulting
bit count modulo eight.

| Modified file | SHA-256 |
| --- | --- |
| `lodepng.cpp` | `f123a824f7df555a1c08882552b6d08047d19c59d08a91d56f6a24002e466053` |
| `lodepng.h` | `49cb066a096850bea3cd6b4fa90246577745b71fea6bc4054ed86681c1a659dd` |

The build compiles the decoder from these sources with disk I/O, the C++ wrapper,
encoding, and ancillary metadata decoding disabled. Allocations in LodePNG are
individually limited to 128 MiB. rypath checks PNG header and chunk framing, limits
the expected decompressed scanline size and the aggregate decode scratch, and only
exposes decoded RGB bytes for the supported texture image profile.

The decoder checks the exact decompressed scanline length and the Adler-32
checksum at the end of the concatenated IDAT payload. Final DEFLATE consumption
follows upstream LodePNG, which can accept unused bytes between the final
DEFLATE block and that checksum.
