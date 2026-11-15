# Owned EXR codec fixtures

All images and the generator in this directory are original rypath assets under
GPL-3.0-only. `generate.cpp` generated the `.exr` images using independently
installed upstream OpenEXR **3.3.2** and Imath **3.1.12**, rather than rypath's
vendored Core. The `.f32` files contain little-endian binary32 samples read back
by that independent OpenEXR implementation, in raster order and the generator's
Y/RGB/RGBA channel order. Their bytes are numerical comparison oracles, including
upstream's actual lossy-codec reconstruction; lossy samples are not compared to
the uncompressed original. `SHA256SUMS` pins all generated binaries.

On the Apple Silicon development host, regenerate with:

```sh
c++ -std=c++20 -I/opt/homebrew/include -I/opt/homebrew/include/Imath \
  -I/opt/homebrew/include/OpenEXR generate.cpp -L/opt/homebrew/lib \
  -lOpenEXR -lImath -o /tmp/rypath-exr-fixtures
/tmp/rypath-exr-fixtures .
```

The optional generator requires installed development libraries; routine CMake
and CI never compile it or use installed OpenEXR runtime libraries. The test
executable accepts this directory as its sole argument.

The ten named classic codecs use identical 19×17 HALF RGB image data, with a
scanline data-window origin of (-5,-9). `tile-origin` is 19×17 FLOAT RGBA with
7×5 tiles, origin (-13,21), and partial edge tiles. `gray` is HALF Y and includes
signed zeros, positive/negative binary16 subnormals, normal-boundary values and
maximum finite magnitudes. Decoded values are compared by binary32 bits against
the independent oracle. `mixed`, `uint` and `wide` exercise explicit unsupported
channel-type/color-space diagnostics; `nonfinite` and `alpha-nonfinite` exercise invalid RGB and alpha samples.

Tests additionally mutate owned headers for partial/extra/subsampled channels,
deep/multipart flags and deferred compression values, reject every truncated
prefix of ZIP input, straddle exact retained/scratch budgets, exercise allocation
failure cleanup in five codec families, isolate concurrent/nested allocation
owners, and check output HALF/FLOAT ZIP metadata and positioned-write failures.
These fixtures establish this scoped image profile, not arbitrary OpenEXR or
full PBRT scene conformance.
