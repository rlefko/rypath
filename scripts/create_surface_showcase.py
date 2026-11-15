#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate three owned PBRT surface material studios and external label metadata."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from create_dielectric_showcase import quad

NAMES = ('coatings', 'transmission-and-mix', 'tables')


def sphere(x, y, z, radius=.85, rings=16, segments=32):
    """Emit an outward smooth UV triangle sphere, with nondegenerate pole fans."""
    if rings < 3 or segments < 3 or radius <= 0:
        raise ValueError('positive radius and at least three rings/segments required')
    normals = [(0., 1., 0.)]
    uv = [(.5, 0.)]
    for row in range(1, rings):
        theta = math.pi * row / rings
        for col in range(segments + 1):
            phi = 2 * math.pi * col / segments
            normals.append((math.sin(theta)*math.cos(phi), math.cos(theta), math.sin(theta)*math.sin(phi)))
            uv.append((col / segments, row / rings))
    bottom = len(normals)
    normals.append((0., -1., 0.))
    uv.append((.5, 1.))
    faces = []
    for col in range(segments):
        faces.append((0, 1 + col + 1, 1 + col))
    for row in range(rings - 2):
        for col in range(segments):
            a = 1 + row*(segments+1) + col
            b = a + segments + 1
            faces.extend(((a, a+1, b+1), (a, b+1, b)))
    last = 1 + (rings - 2)*(segments+1)
    for col in range(segments):
        faces.append((bottom, last + col, last + col + 1))
    points = [(x+radius*n[0], y+radius*n[1], z+radius*n[2]) for n in normals]
    def values(items):
        return ' '.join(f'{v:.9g}' for item in items for v in item)
    return ('Shape "trianglemesh" "point3 P" [' + values(points) + ']\n'
            ' "normal N" [' + values(normals) + ']\n'
            ' "point2 uv" [' + values(uv) + ']\n'
            ' "integer indices" [' + ' '.join(str(i) for face in faces for i in face) + ']\n')


def synthetic_fourier():
    """SCATFUN v1 constant achromatic table, not a measured material asset."""
    data = bytearray(b'SCATFUN\x01')
    data += struct.pack('<9If4I', 1, 3, 9, 1, 1, 1, 0, 0, 0, 1.5, 0, 0, 0, 0)
    data += struct.pack('<3f', -1, 0, 1)
    data += struct.pack('<9f', *([0, 1, 2]*3))
    data += b''.join(struct.pack('<2I', i, 1) for i in range(9))
    data += struct.pack('<9f', *([1]*9))
    return data


def synthetic_measured():
    """Tensor layout fixture with constant fields, not a physical measurement."""
    fields = []
    def add(name, dimensions, values):
        fields.append((name, dimensions, 10, struct.pack('<'+'f'*len(values), *values)))
    add('phi_i', (1,), [0])
    add('theta_i', (2,), [0, math.pi/2])
    add('wavelengths', (2,), [400, 700])
    add('ndf', (2, 2), [1]*4)
    add('sigma', (2, 2), [1]*4)
    add('vndf', (1, 2, 2, 2), [1]*8)
    add('luminance', (1, 2, 2, 2), [1]*8)
    add('spectra', (1, 2, 2, 2, 2), [1]*16)
    fields.extend((name, (1,), 1, b'\0') for name in ('description', 'jacobian'))
    offset = 18 + sum(2+len(name)+2+1+8+8*len(dims) for name, dims, _, _ in fields)
    header = bytearray(b'tensor_file\0' + struct.pack('<HI', 1, len(fields)))
    for name, dims, kind, data in fields:
        header += struct.pack('<H', len(name)) + name.encode()
        header += struct.pack('<HBQ', len(dims), kind, offset)
        header += struct.pack('<'+'Q'*len(dims), *dims)
        offset += len(data)
    return header + b''.join(data for _, _, _, data in fields)


def scene(name, measured='synthetic.tensor', fourier='synthetic.bsdf', width=640, height=400, spp=512):
    if name not in NAMES:
        raise ValueError('unknown showcase')
    for path in (measured, fourier):
        if any(c in str(path) for c in ('"', '\n', '\r', '\\')):
            raise ValueError('table paths must be valid literal PBRT paths')
    text = f'''# SPDX-License-Identifier: GPL-3.0-only
# Original authored geometry. Labels and model limitations are in showcase.json.
LookAt 0 1 -6  0 0 3.8  0 1 0
Camera "perspective" "float fov" 30
Film "rgb" "integer xresolution" {width} "integer yresolution" {height}
 "string sensor" "cie1931" "float iso" 80 "bool savefp16" false
PixelFilter "triangle"
Sampler "sobol" "integer pixelsamples" {spp} "integer seed" 42 "string randomization" "fastowen"
Integrator "path" "integer maxdepth" 8
WorldBegin
LightSource "infinite" "rgb L" [.025 .035 .055]
Texture "floor" "spectrum" "checkerboard" "rgb tex1" [.12 .14 .18] "rgb tex2" [.35 .38 .45] "float uscale" 10 "float vscale" 10
Material "diffuse" "texture reflectance" "floor"
'''
    text += quad([(-10,-1,-4),(10,-1,-4),(10,-1,10),(-10,-1,10)])
    text += 'Material "diffuse" "rgb reflectance" [.08 .1 .16]\n'
    text += quad([(-10,-1,7),(10,-1,7),(10,8,7),(-10,8,7)])
    for points, rgb, scale in [
        ([(-4,4,0),(-.4,4,0),(-.4,4,6),(-4,4,6)], [1,.8,.6], 16),
        ([(3,-.2,1),(3.4,-.2,1),(3.4,3,1),(3,3,1)], [.5,.7,1], 24),
    ]:
        text += 'AttributeBegin\nMaterial "diffuse" "rgb reflectance" [0 0 0]\n'
        text += f'AreaLightSource "diffuse" "rgb L" [{" ".join(map(str,rgb))}] "float scale" {scale} "bool twosided" true\n'
        text += quad(points) + 'AttributeEnd\n'
    labels = []
    def orb(label, material, x):
        nonlocal text
        text += material + '\n' + sphere(x, 0, 3.8)
        labels.append({'label': label, 'center': [x, 0, 3.8]})
    if name == 'coatings':
        orb('Coated diffuse / low-roughness interface', 'Material "coateddiffuse" "rgb reflectance" [.55 .13 .05] "float roughness" .03', -2)
        orb('Coated diffuse / scattering layer', 'Material "coateddiffuse" "rgb reflectance" [.08 .3 .55] "float roughness" .15 "rgb albedo" [.3 .5 .7] "float thickness" .08 "float g" .3', 0)
        orb('Coated conductor / rough copper', 'Material "coatedconductor" "float interface.roughness" .06 "float conductor.roughness" .2', 2)
    elif name == 'transmission-and-mix':
        orb('Thin dielectric shell', 'Material "thindielectric" "float eta" 1.5', -2)
        orb('Diffuse transmission', 'Material "diffusetransmission" "rgb reflectance" [.15 .08 .04] "rgb transmittance" [.6 .3 .12]', 0)
        text += '''Texture "blend" "float" "checkerboard" "float tex1" .1 "float tex2" .9 "float uscale" 6 "float vscale" 4
MakeNamedMaterial "coat" "string type" "coateddiffuse" "rgb reflectance" [.1 .45 .3] "float roughness" .1
MakeNamedMaterial "thin" "string type" "thindielectric" "float eta" 1.5
MakeNamedMaterial "mixed" "string type" "mix" "string materials" ["coat" "thin"] "texture amount" "blend"
'''
        orb('Mapped coated diffuse / thin mix', 'NamedMaterial "mixed"', 2)
    else:
        orb('Measured tensor input', f'Material "measured" "string filename" "{measured}"', -2)
        orb('Legacy Fourier input', f'Material "fourier" "string bsdffile" "{fourier}"', 0)
        orb('Conductor reflectance shortcut', 'Material "conductor" "rgb reflectance" [.4 .6 .8] "float roughness" .18', 2)
    return text, labels


def generate(output_dir, measured=None, fourier=None, width=640, height=400, spp=512):
    if min(width, height, spp) < 1:
        raise ValueError('resolution and samples must be positive')
    output_dir.mkdir(parents=True, exist_ok=True)
    sources = {}
    for name, supplied, data in (('measured', measured, synthetic_measured()), ('fourier', fourier, synthetic_fourier())):
        path = Path(supplied).resolve() if supplied else output_dir / ('synthetic.tensor' if name == 'measured' else 'synthetic.bsdf')
        if supplied:
            if not path.is_file():
                raise ValueError(f'missing {name} input: {path}')
        else:
            path.write_bytes(data)
        sources[name] = {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                         'provenance': 'caller-supplied unmodified asset; retain its original license' if supplied else 'owned synthetic format fixture, not a physical measurement'}
    paths = {name: value['path'] if (measured if name == 'measured' else fourier) else Path(value['path']).name for name, value in sources.items()}
    labels = {}
    for name in NAMES:
        text, labels[name] = scene(name, paths['measured'], paths['fourier'], width, height, spp)
        (output_dir / (name+'.pbrt')).write_text(text)
    metadata = {'license': 'GPL-3.0-only (owned geometry and synthetic tables)', 'resolution': [width, height], 'spp': spp, 'seed': 42,
                'tables': sources, 'labels': labels, 'layered_model': 'finite maxdepth/nsamples stochastic approximation; raw accumulation',
                'outputs': 'render each .pbrt to .exr and .png with identical settings; labels remain outside images; no denoising'}
    (output_dir/'showcase.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--measured-table', type=Path)
    parser.add_argument('--fourier-table', type=Path)
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=400)
    parser.add_argument('--spp', type=int, default=512)
    args = parser.parse_args()
    try:
        generate(args.output_dir, args.measured_table, args.fourier_table, args.width, args.height, args.spp)
    except ValueError as error:
        parser.error(str(error))
    for name in NAMES:
        print(args.output_dir/(name+'.pbrt'))


if __name__ == '__main__':
    main()
