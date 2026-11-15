# JPEG decoder fixtures

These project-authored 19×17 images exercise distinct decoder paths. Each was
encoded with libjpeg-turbo `cjpeg` 3.1.0 at quality 81. The fixtures use the
[project license](../../../LICENSE). The input PPM uses the following RGB
values at column `x` and row `y`, reduced modulo 256.

```text
R = 11x + 29y + 37
G = 43x + 17y + 8
B = 31x + 47y + 101
```

| File | Additional encoder options | SHA-256 |
| --- | --- | --- |
| `baseline-rgb.jpg` | `-optimize -rgb` | `2db49a3189fa58c6251f2e32059694d791e13435b1c2dcc655152bc997f9a0c5` |
| `baseline-ycbcr.jpg` | `-optimize -restart 1B` | `86eba0bfcce8e5a7b6dae5cef143b51fcbd7d19a3962b954394e4998899b0ffd` |
| `progressive-gray.jpg` | `-progressive -grayscale` | `1acfae12f4551b846252f5a7d09054424781b591ee31992d342669acbeeb3f0e` |
| `progressive-mixed-dc.jpg` | `-sample 2x2,1x1,1x1 -scans scans.txt` | `222abb91212df1e2824d1380a442a0f1f0f117d82cf90507840ac0dd3878db08` |
| `progressive-ycbcr.jpg` | `-progressive -restart 2B` | `df75ab1b5df60ccd529d89bd02e9e8fb44991a79e6f201ac910ae417c6c987c0` |

The mixed DC fixture uses separate first DC scans followed by an interleaved
refinement. Its 4:2:0 dimensions require padded luma blocks that the first DC
scan skips. The encoder script `scans.txt` contains the following scan order.

```text
0: 0 0 0 1;
1: 0 0 0 1;
2: 0 0 0 1;
0 1 2: 0 0 1 0;
0: 1 63 0 0;
1: 1 63 0 0;
2: 1 63 0 0;
```

The tests expect the decoded bytes from the unmodified
[PBRT-pinned stb header](../../../third_party/stb/README.md). The baseline
and progressive YCbCr fixtures produce identical RGB bytes.
