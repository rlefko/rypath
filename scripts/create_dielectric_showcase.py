#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate original closed-mesh glass studio scenes; no external assets required."""
import argparse
import json
import math
from pathlib import Path


def mesh(vertices, faces):
    center = [sum(v[k] for v in vertices) / len(vertices) for k in range(3)]
    indices = []
    for face in faces:
        a, b, c = [vertices[i] for i in face[:3]]
        u, v = [[q[k] - a[k] for k in range(3)] for q in (b, c)]
        n = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]
        outward = sum(n[k] * (a[k] - center[k]) for k in range(3))
        face = face if outward > 0 else face[::-1]
        for j in range(1, len(face)-1):
            indices.extend((face[0], face[j], face[j+1]))
    return ('Shape "trianglemesh" "point3 P" [' + ' '.join(f'{x:.8g}' for p in vertices for x in p)
            + ']\n "integer indices" [' + ' '.join(map(str, indices)) + ']\n')


def bevel_block():
    # Octagonal cross section, closed by planar end caps. Every face is original.
    x, y, bevel = 1.05, 1.35, .18
    polygon = [(-x+bevel,-y),(x-bevel,-y),(x,-y+bevel),(x,y-bevel),
               (x-bevel,y),(-x+bevel,y),(-x,y-bevel),(-x,-y+bevel)]
    angle = math.radians(24)
    vertices = []
    for z in (-.65,.65):
        for px, py in polygon:
            vertices.append((px*math.cos(angle)+z*math.sin(angle), py,
                             4.3-px*math.sin(angle)+z*math.cos(angle)))
    return mesh(vertices, [list(range(8)),list(range(8,16))] +
                [[i,(i+1)%8,(i+1)%8+8,i+8] for i in range(8)])


def prism():
    polygon = [(-1.15,3.45),(1.2,4.55),(-1.15,5.5)]
    vertices = [(x,y,z) for y in (-1.35,1.35) for x,z in polygon]
    return mesh(vertices, [[0,1,2],[3,4,5],[0,1,4,3],[1,2,5,4],[2,0,3,5]])


def quad(points):
    return mesh(points, [[0,1,2,3]])


def scene(name, width, height, spp):
    material = {
        'polished-glass': '"float eta" [1.52] "float roughness" [0]',
        'frosted-anisotropic': '"float eta" [1.52] "float uroughness" [.008] "float vroughness" [.12]',
        'dispersive-prism': '"spectrum eta" [360 1.72 450 1.62 550 1.52 650 1.46 830 1.42] "float roughness" [0]',
    }[name]
    result = f'''# SPDX-License-Identifier: GPL-3.0-only
# Original owned showcase. Synthetic prism eta is illustrative, not measured glass.
LookAt 0 1.1 -4.5  0 0 4.3  0 1 0
Camera "perspective" "float fov" [29]
Film "rgb" "integer xresolution" [{width}] "integer yresolution" [{height}]
 "string sensor" "cie1931" "float iso" [60] "bool savefp16" [false]
PixelFilter "triangle"
Sampler "sobol" "integer pixelsamples" [{spp}] "integer seed" [17] "string randomization" "fastowen"
Integrator "path" "integer maxdepth" [12]
WorldBegin
LightSource "infinite" "rgb L" [.035 .045 .075]
Texture "grid" "spectrum" "checkerboard" "rgb tex1" [.015 .025 .055] "rgb tex2" [.72 .53 .26]
 "float uscale" [14] "float vscale" [10]
Material "diffuse" "texture reflectance" "grid"
Shape "trianglemesh" "point3 P" [-6 -2 7 6 -2 7 6 5 7 -6 5 7]
 "point2 uv" [0 0 1 0 1 1 0 1] "integer indices" [0 2 1 0 3 2]
Material "diffuse" "rgb reflectance" [.11 .14 .19]
'''
    result += quad([(-8,-1.4,-3),(8,-1.4,-3),(8,-1.4,9),(-8,-1.4,9)])
    for points, rgb, scale in [
        ([(-3,4,1),(-.3,4,1),(-.3,4,6),(-3,4,6)], [.75,.85,1], 12),
        ([(2,0,2),(2.8,0,2),(2.8,3,2),(2,3,2)], [1,.7,.35], 9),
        ([(-3,-.5,1),(-2.5,-.5,1),(-2.5,3,1),(-3,3,1)], [.4,.65,1], 14),
    ]:
        result += 'AttributeBegin\nMaterial "diffuse" "rgb reflectance" [0 0 0]\n'
        result += f'AreaLightSource "diffuse" "rgb L" [{" ".join(map(str,rgb))}] "float scale" [{scale}] "bool twosided" [true]\n'
        result += quad(points) + 'AttributeEnd\n'
    result += f'Material "dielectric" {material}\n'
    result += prism() if name == 'dispersive-prism' else bevel_block()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--width', type=int, default=960)
    parser.add_argument('--height', type=int, default=600)
    parser.add_argument('--spp', type=int, default=4096)
    args = parser.parse_args()
    if min(args.width,args.height,args.spp) < 1:
        parser.error('resolution and spp must be positive')
    args.output_dir.mkdir(parents=True,exist_ok=True)
    for name in ('polished-glass','frosted-anisotropic','dispersive-prism'):
        path = args.output_dir/(name+'.pbrt')
        path.write_text(scene(name,args.width,args.height,args.spp))
        print(path)
    (args.output_dir/'showcase.json').write_text(json.dumps({
        'license':'GPL-3.0-only', 'assets':'original inline closed triangle meshes; no external dependencies',
        'resolution':[args.width,args.height], 'spp':args.spp, 'seed':17,
        'dispersion':'Synthetic illustrative eta; no physical glass claim',
        'outputs':'Render each scene with --render-pbrt --output NAME.exr, then NAME.png; both use raw accumulation, no denoising',
    },indent=2)+'\n')


if __name__ == '__main__':
    main()
