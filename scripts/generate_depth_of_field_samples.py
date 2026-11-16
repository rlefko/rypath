#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate matched original pinhole, near-focus and far-focus scenes; never render."""
import argparse
from pathlib import Path

from generate_spot_samples import box, diffuse, sphere
from generate_projection_samples import pattern_png


def panel(x, y, z, width, height):
    return (f'Shape "trianglemesh" "point3 P" [{x} {y} {z} {x+width} {y} {z} '
            f'{x+width} {y+height} {z} {x} {y+height} {z}]\n'
            ' "point2 uv" [0 0 1 0 1 1 0 1] "integer indices" [0 2 1 0 3 2]\n')


def generate(destination, samples=512, width=768, height=512):
    destination.mkdir(parents=True, exist_ok=True)
    (destination/'focus-chart.png').write_bytes(pattern_png())
    # Camera looks straight along +Z. Focus values are camera-space axial depths.
    body = 'LightSource "infinite" "float scale" [.12]\n'
    body += 'LightSource "point" "point3 from" [-3 6 1] "rgb I" [95 85 70]\n'
    body += 'LightSource "point" "point3 from" [4 4 7] "rgb I" [40 65 100]\n'
    body += diffuse((.045, .055, .075)) + box((-9, -.2, 0), (9, -.1, 17))
    body += diffuse((.08, .10, .14)) + box((-9, -.1, 16), (9, 9, 16.15))
    body += 'Texture "chart" "spectrum" "imagemap" "string filename" ["focus-chart.png"] "string filter" ["ewa"]\n'
    # Separated physical cards provide crisp colored glyphs and fine checks at two depths.
    for x, z, w, h in ((-2.3, 4, 2.1, 2.7), (.4, 10, 4.4, 5.4)):
        body += diffuse((.018, .023, .032)) + box((x-.10, .35, z), (x+w+.10, .35+h+.2, z+.18))
        body += 'Material "diffuse" "texture reflectance" "chart"\n' + panel(x, .45, z-.015, w, h)
        body += diffuse((.16, .19, .24)) + box((x+w*.4, -.1, z+.04), (x+w*.6, .36, z+.14))
    body += 'Material "conductor" "rgb reflectance" [.85 .52 .16] "float roughness" [.16] "bool remaproughness" [false]\n'
    body += sphere((-.1, .48, 6.5), .55)
    for name, radius, focus in (('pinhole', 0, 4), ('focus-near', .18, 4), ('focus-far', .18, 10)):
        scene = f'''# SPDX-License-Identifier: GPL-3.0-only
# Original rypath depth-of-field showcase; no external assets.
LookAt 0 1.8 0 0 1.8 1 0 1 0
Camera "perspective" "float fov" [47] "float lensradius" [{radius}] "float focaldistance" [{focus}]
Film "rgb" "integer xresolution" [{width}] "integer yresolution" [{height}]
 "string sensor" "cie1931" "float iso" [100] "bool savefp16" [false]
PixelFilter "triangle" "float xradius" [1] "float yradius" [1]
Sampler "sobol" "string randomization" ["fastowen"] "integer pixelsamples" [{samples}] "integer seed" [17]
Integrator "path" "integer maxdepth" [6]
WorldBegin
'''
        (destination/(name+'.pbrt')).write_text(scene+body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--samples', default=512, type=int)
    parser.add_argument('--width', default=768, type=int)
    parser.add_argument('--height', default=512, type=int)
    args = parser.parse_args()
    if not 1 <= args.samples <= 1048576 or args.samples & (args.samples-1):
        parser.error('--samples must be a power of two from 1 through 1048576')
    if not 1 <= args.width <= 4096 or not 1 <= args.height <= 4096:
        parser.error('--width and --height must be from 1 through 4096')
    generate(args.output_dir.resolve(), args.samples, args.width, args.height)
    print('Generated pinhole.pbrt, focus-near.pbrt, focus-far.pbrt and focus-chart.png')


if __name__ == '__main__':
    main()
