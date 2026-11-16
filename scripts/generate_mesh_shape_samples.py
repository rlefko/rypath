#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate original subdivision-ring and curved-quad showcase scenes."""
import argparse
import math
from pathlib import Path


def torus_control():
    """An original, consistently wound 12-by-6 triangular control cage."""
    points = []
    indices = []
    for i in range(12):
        u = 2 * math.pi * i / 12
        for j in range(6):
            v = 2 * math.pi * j / 6
            r = 1.05 + .38 * math.cos(v)
            points.extend((r * math.cos(u), r * math.sin(u), 3 + .38 * math.sin(v)))
            a, b = i * 6 + j, ((i + 1) % 12) * 6 + j
            c, d = i * 6 + (j + 1) % 6, ((i + 1) % 12) * 6 + (j + 1) % 6
            indices.extend((a, b, c, c, b, d))
    return ('"point3 P" [' + ' '.join(f'{x:.9g}' for x in points) + ']\n'
            '"integer indices" [' + ' '.join(map(str, indices)) + ']\n')


def generate(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    header = '''# SPDX-License-Identifier: GPL-3.0-only
# Original rypath showcase; raw spectral render with no denoising.
LookAt 0 1.1 -5  0 0 3  0 1 0
Camera "perspective" "float fov" [27]
Film "rgb" "integer xresolution" [768] "integer yresolution" [512]
 "string sensor" "cie1931" "float iso" [100] "bool savefp16" [false]
PixelFilter "box" "float xradius" [.5] "float yradius" [.5]
Sampler "sobol" "integer pixelsamples" [512] "integer seed" [0] "string randomization" ["fastowen"]
Integrator "path" "integer maxdepth" [6]
WorldBegin
LightSource "infinite" "rgb L" [.05 .07 .12]
LightSource "point" "point3 from" [-3 4 -1] "blackbody I" [4800] "float scale" [60]
LightSource "point" "point3 from" [3 1 0] "blackbody I" [8000] "float scale" [20]
AttributeBegin
Material "diffuse" "rgb reflectance" [.3 .34 .4]
Shape "bilinearmesh" "point3 P" [-20 -1.45 -10 20 -1.45 -10 -20 -1.45 20 20 -1.45 20]
Material "diffuse" "rgb reflectance" [.045 .075 .12]
Shape "bilinearmesh" "point3 P" [-20 -1.45 7 20 -1.45 7 -20 20 7 20 20 7]
AttributeEnd
'''
    material = '''Material "conductor" "spectrum eta" "metal-Cu-eta" "spectrum k" "metal-Cu-k"
 "float roughness" [.28]
'''
    paths = []
    for level in (None, 0, 1, 3):
        name = 'control' if level is None else f'loop-{level}'
        shape = ('Shape "trianglemesh" ' if level is None else
                 f'Shape "loopsubdiv" "integer levels" [{level}] ')
        path = destination / (name + '.pbrt')
        path.write_text(header + material + shape + torus_control())
        paths.append(path)
    chart = '''Texture "chart" "spectrum" "checkerboard" "integer dimension" [2]
 "rgb tex1" [.025 .32 .46] "rgb tex2" [.85 .3 .055] "float uscale" [10] "float vscale" [8]
Material "diffuse" "texture reflectance" "chart"
Shape "bilinearmesh" "integer indices" [0 1 2 3]
 "point3 P" [-1.5 -1 3 1.5 -1 3 -1.5 1 3 1.5 1 5]
 "point2 uv" [0 0 1 0 0 1 1 1]
'''
    path = destination / 'curved-quad.pbrt'
    path.write_text(header + chart)
    paths.append(path)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    for path in generate(args.output_dir):
        print(path)


if __name__ == '__main__':
    main()
