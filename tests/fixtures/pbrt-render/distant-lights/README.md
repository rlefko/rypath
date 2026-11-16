# Distant-light reference fixtures

Eight original GPL-3.0-only scenes and one owned wrapper around a pinned
external quad exercise the bounded distant-light profile. `manifest.json` pins each entry's size and
SHA-256, `dependencies.json` records the dependency closures, and
`provenance.json` records authorship, terms and the reference revision.

| Scene | Intended sensitivity |
| --- | --- |
| `diffuse-direction` | Oblique `from`/`to` direction, incident cosine, delta light NEE |
| `spectral-illuminance` | Nonconstant numeric `L`, photometric illuminance and scale |
| `affine-transform` | Affine direction mapping with translation cancellation; placed object bounds |
| `mixed` | Distant, point, spot and constant environment contributions together |
| `rough-reflection` | Rough dielectric reflection of a delta-direction source |
| `rough-transmission` | Rough dielectric transmission with the source on the opposite side |
| `bounded-volume` | HG scattering and shadow attenuation through a closed null-interface box |
| `unbounded-volume` | Active infinite camera medium with nonempty geometry and finite distant attenuation |
| `external-quad` | Unchanged pinned polygon asset under an owned transformed distant-light wrapper |

The unbounded-medium scene deliberately retains an opaque receiver. PBRT computes
a finite scene bounding sphere from geometry and sets the distant-light endpoint
to `p + wi * (2 * sceneRadius)`. A positive extinction medium attenuates that finite
segment; treating it as an infinite connection would erase the only light source.
The scenes do not include a camera-visible sun disk.

Run input validation with the pinned PBRT v4 CPU executable:

```sh
python3 scripts/compare_distant_lights.py --pbrt-only \
  --output-dir build/distant-reference
```

Run the full comparison after building `rypath_pbrt_reference`:

```sh
python3 scripts/compare_distant_lights.py \
  --rypath build/debug/rypath_pbrt_reference \
  --output-dir build/distant-comparison
```

`--pbrt` selects an explicit executable; the default is
`.cache/pbrt-reference/build/pbrt`. `--scene NAME` limits either mode to selected
entries. PBRT must be built from
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.

The reference command explicitly adds the official `--render-coord-sys world`
option to match rypath's world-space bounds and finite endpoints. This override
is recorded per fixture and in every actual process command. The reference also
uses `--nthreads 2` to bound local CPU work. Authored scene bytes are identical
between backends except the independent sampler seeds. No reference light-sampler
or material override is applied.

Full comparison uses 8×8 linear RGB images at 4096 spp, independent rypath seeds
0–15 and PBRT seeds 16–31, and five regions (full frame and quadrants). Each of the
135 region/channel gates accepts differences up to five combined standard errors
of the independent seed means plus `2e-5 * max(1, abs(reference))`. Pixels are not
counted as independent replicates. Output retains source variants, PFM images,
process transcripts, executable hashes, overrides, measurements and provenance.
A reference process reporting an error is rejected even if it exits zero.

On 2026-11-15 all nine pinned PBRT input renders passed with finite, positive
signal using the world-coordinate override. This checks reference inputs only;
it is not a measured agreement claim for rypath or full scene-corpus conformance.
See the lead's validation record for subsequent matched comparison results.

`assets/sportscar-quad.ply` preserves all 412 bytes of
[`sportscar/geometry/Plane_0000_m000.ply`](https://github.com/mmp/pbrt-v4-scenes/blob/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/sportscar/geometry/Plane_0000_m000.ply)
at scenes revision `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`.
Yasutoshi Mori created the Sportscar model, licensed under
[CC BY 2.0](https://creativecommons.org/licenses/by/2.0/).
The upstream exporter comment remains intact; see the
[existing attribution](../ply-assets/README.md). The owned `external-quad`
wrapper transforms this unchanged selected asset. This establishes selected asset
coverage only; the original Sportscar scene and full corpus remain outside scope.
