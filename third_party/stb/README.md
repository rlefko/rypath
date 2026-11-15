# stb_image JPEG decoder

`stb_image.h` comes from [stb revision
`af1a5bc352164740c1cc1354942b1c6b72eacb8a`](https://github.com/nothings/stb/tree/af1a5bc352164740c1cc1354942b1c6b72eacb8a),
the submodule pinned by rypath's PBRT v4 reference revision
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. See [LICENSE](LICENSE) and the header
for the original dual-license notice.

| File | SHA-256 |
| --- | --- |
| Original `stb_image.h` | `91f435e0fc6a620018b878b9859c74dff60d28046f87e649191ad6f35a98c722` |
| Modified `stb_image.h` | `00d52444dcd5ad343ae073e033d92ee7b1362b99a4d6d3a1c33eb561d2845e0f` |
| Original `LICENSE` | `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63` |

The rypath adapter compiles JPEG decoding from memory. It accepts 8-bit Huffman
baseline and progressive DCT JPEG in grayscale, RGB, and YCbCr with stb's supported
sampling ratios. The adapter requires complete marker framing through EOI and
limits the file to 16 MiB, dimensions to 4096 per axis, and scans to 256. All
decoder heap allocations use a per-decode scratch budget, including allocation
records and the live output buffer during the final retained RGB copy.

Local changes to the header validate coding tables, component coverage, progressive
scan order, coefficient runs and ranges, entropy padding, and restart intervals.
They reject truncation at an entropy boundary instead of padding with zeros or
returning a partially decoded image. Progressive coefficient planes start at
zero so an interleaved refinement can safely update MCU padding skipped by a
noninterleaved first scan. The JPEG state charges each scan's scheduled
8×8 blocks and the final progressive IDCT pass against a cumulative 2²²-block
limit. Coefficient products widen an operand to `int64_t` before multiplication
and then pass through the signed 16-bit range check. Existing symbol, DC, and
progressive-shift bounds already keep these products within signed 32-bit range.
The explicit widening makes the checked arithmetic consistent across all paths.
The scalar IDCT widens integer intermediates to avoid signed overflow on
malformed coefficients. IDCT scaling, rounding, color conversion, and upsampling
follow the pinned implementation for valid inputs.
