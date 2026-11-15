#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create original static glTF showcases, convert through rypath, optionally render.

All geometry, textures, and scene design are generated here without external assets.
Products belong in an untracked directory, normally .cache/showcase/gltf-<run>.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import zlib


def write_png(path, width, height, pixel):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + bytes(c for x in range(width) for c in pixel(x, y))
                    for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


class Asset:
    def __init__(self, name):
        self.name = name
        self.data = bytearray()
        self.doc = dict(asset={'version': '2.0', 'generator': 'rypath owned showcase'},
                        scene=0, scenes=[{'name': name, 'nodes': []}], nodes=[], meshes=[],
                        materials=[], buffers=[], bufferViews=[], accessors=[])

    def accessor(self, values, width, component=5126):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        flat = [v for item in values for v in item] if width > 1 else values
        self.data.extend(struct.pack('<' + ('f' if component == 5126 else 'I') * len(flat), *flat))
        view = len(self.doc['bufferViews'])
        self.doc['bufferViews'].append(dict(buffer=0, byteOffset=offset, byteLength=len(self.data)-offset))
        result = dict(bufferView=view, componentType=component, count=len(values),
                      type={1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4'}[width])
        if component == 5126:
            result['min'] = [min(p[k] for p in values) for k in range(width)]
            result['max'] = [max(p[k] for p in values) for k in range(width)]
        index = len(self.doc['accessors'])
        self.doc['accessors'].append(result)
        return index

    def material(self, name, color, metallic, roughness, **extra):
        result = dict(name=name, pbrMetallicRoughness=dict(baseColorFactor=color,
                      metallicFactor=metallic, roughnessFactor=roughness))
        result.update(extra)
        self.doc['materials'].append(result)
        return len(self.doc['materials'])-1

    def geometry(self, name, positions, normals, uv, tangents, indices, material):
        attributes = {key: self.accessor(value, width) for key, value, width in
                      [('POSITION', positions, 3), ('NORMAL', normals, 3),
                       ('TEXCOORD_0', uv, 2), ('TANGENT', tangents, 4)]}
        primitive = dict(attributes=attributes, indices=self.accessor(indices, 1, 5125),
                         material=material, mode=4)
        self.doc['meshes'].append(dict(name=name, primitives=[primitive]))
        return len(self.doc['meshes'])-1

    def place(self, mesh, name, translation, scale=(1, 1, 1)):
        index = len(self.doc['nodes'])
        self.doc['nodes'].append(dict(mesh=mesh, name=name, translation=list(translation), scale=list(scale)))
        self.doc['scenes'][0]['nodes'].append(index)

    def sphere(self, name, material, radius=1, latitude=24, longitude=48):
        # Per-corner vertices keep the UV seam and pole wedges distinct. There
        # are no coincident pole triangles. T = dP/du, B = cross(N,T)
        # follows top-left native glTF V from north to south.
        points, normals, uv, tangents, indices = [], [], [], [], []

        def corner(theta, phi, u, v):
            n = (math.sin(theta)*math.cos(phi), math.cos(theta), math.sin(theta)*math.sin(phi))
            points.append(tuple(radius*x for x in n))
            normals.append(n)
            uv.append((u, v))
            tangents.append((-math.sin(phi), 0, math.cos(phi), 1))
            indices.append(len(indices))

        def triangle(corners):
            for theta, phi, u, v in corners:
                corner(theta, phi, u, v)

        def value(i, j):
            return math.pi*i/latitude, 2*math.pi*j/longitude, j/longitude, i/latitude

        # Outward winding is (north-west, south-east, south-west) for this
        # parameterization. Tangent handedness is +1: cross(N,T) follows +V.
        for i in range(latitude):
            for j in range(longitude):
                a, b, c, d = value(i, j), value(i+1, j), value(i+1, j+1), value(i, j+1)
                if i != latitude-1:
                    triangle((a, c, b))
                if i != 0:
                    triangle((a, d, c))
        return self.geometry(name, points, normals, uv, tangents, indices, material)

    def panel(self, name, material, width, height):
        return self.geometry(name, [(-width/2, -height/2, 0), (width/2, -height/2, 0),
                                   (width/2, height/2, 0), (-width/2, height/2, 0)],
                             [(0, 0, 1)]*4, [(0, 1), (1, 1), (1, 0), (0, 0)],
                             [(1, 0, 0, -1)]*4, [0, 1, 2, 0, 2, 3], material)

    def save(self, path, binary=False):
        document = dict(self.doc)
        document['buffers'] = [dict(byteLength=len(self.data))]
        if not binary:
            buffer_path = path.with_suffix('.bin')
            buffer_path.write_bytes(self.data)
            document['buffers'][0]['uri'] = buffer_path.name
            path.write_text(json.dumps(document, separators=(',', ':'))+'\n')
        else:
            text = json.dumps(document, separators=(',', ':')).encode()
            text += b' '*((-len(text)) % 4)
            data = bytes(self.data) + b'\0'*((-len(self.data)) % 4)
            chunks = struct.pack('<I4s', len(text), b'JSON') + text + struct.pack('<I4s', len(data), b'BIN\0') + data
            path.write_bytes(struct.pack('<4sII', b'glTF', 2, 12+len(chunks))+chunks)


def pbrt_quad(points):
    return 'Shape "trianglemesh" "point3 P" [' + ' '.join(str(x) for p in points for x in p) + '] "integer indices" [0 1 2 0 2 3]\n'


def studio(size, samples, textured):
    height = round(size*.75)
    eye, target, fov = ((7, 5, 12), (.2, 1, 0), 36) if textured else ((0, 10, 15), (0, .7, 0), 37)
    output = f'''# SPDX-License-Identifier: GPL-3.0-only
# Original glTF showcase wrapper. All featured surfaces come from CLI conversion.
LookAt {' '.join(map(str, eye))} {' '.join(map(str, target))} 0 1 0
Camera "perspective" "float fov" [{fov}]
Film "rgb" "integer xresolution" [{size}] "integer yresolution" [{height}]
 "string sensor" "cie1931" "float iso" [55] "bool savefp16" [false]
PixelFilter "triangle"
Sampler "sobol" "integer pixelsamples" [{samples}] "integer seed" [417]
 "string randomization" "fastowen"
Integrator "path" "integer maxdepth" [8]
WorldBegin
LightSource "infinite" "rgb L" [.055 .065 .085]
Material "diffuse" "rgb reflectance" [.17 .19 .23]
'''
    output += pbrt_quad([(-18, 0, -18), (-18, 0, 18), (18, 0, 18), (18, 0, -18)])
    # Keep the studio panels outside both camera frusta; highlights still show
    # their shape. All emitters remain ordinary visible geometry.
    lights = [([(-6, 10, 2), (-1, 10, 2), (-1, 10, 7), (-6, 10, 7)], (1, .87, .72), 10),
              ([(6, 8, 0), (10, 8, 0), (10, 8, 5), (6, 8, 5)], (.62, .8, 1), 8),
              ([(-3, 10, -6), (2, 10, -6), (2, 10, -2), (-3, 10, -2)], (1, .9, .8), 6)]
    if textured:
        # Smaller key produces readable perforated-screen shadows.
        lights[0] = ([(0, 8, 5), (1.3, 8, 5), (1.3, 8, 6.3), (0, 8, 6.3)], (1, .88, .72), 35)
    for points, rgb, scale in lights:
        output += 'AttributeBegin\nMaterial "diffuse" "rgb reflectance" [0 0 0]\n'
        output += f'AreaLightSource "diffuse" "rgb L" [{" ".join(map(str, rgb))}] "float scale" [{scale}] "bool twosided" [true]\n'
        output += pbrt_quad(points) + 'AttributeEnd\n'
    return output + 'Include "model.pbrt"\n'


def material_study(directory):
    asset = Asset('Metallic / roughness studio')
    for row, metallic in enumerate((0, .5, 1)):
        for col, roughness in enumerate((0, .12, .35, .7)):
            color = [.78, .27, .095, 1] if col % 2 == 0 else [.075, .3, .62, 1]
            name = f'metallic-{metallic}-roughness-{roughness}'
            material = asset.material(name, color, metallic, roughness)
            mesh = asset.sphere(name, material, .9)
            asset.place(mesh, name, ((1.5-col)*2.35, .9, (1-row)*2.5))
    path = directory/'material-study.gltf'
    asset.save(path)
    return path


def texture_study(directory):
    size = 256

    def color(x, y):
        u, v = x/size, y/size
        if u < .34:
            rgb = (235, 137, 48)
        elif u < .69:
            rgb = (28, 139, 165)
        else:
            rgb = (38, 63, 108)
        if ((x//16) + (y//16)) % 2:
            rgb = tuple(round(c*.7) for c in rgb)
        if (u-.22)**2 + (v-.25)**2 < .012:
            rgb = (244, 236, 189)
        return (*rgb, 255)

    def normal(x, y):
        u, v = x/size, y/size
        dx = .4*math.sin(2*math.pi*u*20)
        dy = .4*math.sin(2*math.pi*v*20)
        length = math.sqrt(1+dx*dx+dy*dy)
        return (round(127.5*(1+dx/length)), round(127.5*(1+dy/length)),
                round(127.5*(1+1/length)), 255)

    def perforation(x, y):
        u, v = (x+.5)/size, (y+.5)/size
        dx, dy = (u*7) % 1-.5, (v*10) % 1-.5
        alpha = 0 if dx*dx+dy*dy < .115 else 255
        return (205, 145, 63, alpha)

    write_png(directory/'asymmetric-color.png', size, size, color)
    write_png(directory/'metallic-roughness.png', size, size,
              lambda x, y: (255, round(35+185*y/(size-1)), round(255*x/(size-1)), 255))
    write_png(directory/'normal-detail.png', size, size, normal)
    write_png(directory/'perforated-mask.png', size, size, perforation)
    asset = Asset('Native UV / tangent detail / alpha studio')
    asset.doc['images'] = [{'uri': name} for name in
                           ('asymmetric-color.png', 'metallic-roughness.png', 'normal-detail.png', 'perforated-mask.png')]
    asset.doc['samplers'] = [dict(wrapS=10497, wrapT=10497, minFilter=9987, magFilter=9729)]
    asset.doc['textures'] = [dict(source=i, sampler=0) for i in range(4)]
    detailed = asset.material('Native UV + normal detail', [1, 1, 1, 1], .75, .8,
                              normalTexture={'index': 2, 'scale': 1})
    asset.doc['materials'][detailed]['pbrMetallicRoughness'].update(
        baseColorTexture={'index': 0}, metallicRoughnessTexture={'index': 1})
    sphere = asset.sphere('Detailed UV sphere', detailed, 1.08, 32, 64)
    asset.place(sphere, 'Native UV sphere', (-2.2, 1.08, 0))
    asset.place(sphere, 'Mirrored native frame', (.35, 1.08, -.7), (-1, 1, 1))
    mask = asset.material('Perforated brass MASK', [1, .85, .65, 1], .8, .3,
                          alphaMode='MASK', alphaCutoff=.5, doubleSided=True)
    asset.doc['materials'][mask]['pbrMetallicRoughness']['baseColorTexture'] = {'index': 3}
    asset.place(asset.panel('Perforated screen', mask, 1.75, 2.8), 'Perforated screen', (3, 1.43, -.25))
    blend = asset.material('Blue BLEND veil', [.035, .3, .65, .32], 0, .65,
                           alphaMode='BLEND', doubleSided=True)
    asset.place(asset.panel('Translucent veil', blend, 1.65, 1.75), 'BLEND over mirrored sphere', (.7, 1.1, 1.1))
    path = directory/'texture-study.glb'
    asset.save(path, binary=True)
    return path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, directory):
    result = subprocess.run([str(item) for item in command], check=False, capture_output=True, text=True)
    log = '$ ' + ' '.join(map(str, command)) + '\n' + result.stdout + result.stderr
    (directory/'commands.log').open('a').write(log+'\n')
    print(log, end='', flush=True)
    if result.returncode:
        raise RuntimeError(f'command exited {result.returncode}; see {directory / "commands.log"}')
    return [str(item) for item in command]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='New untracked artifact directory')
    parser.add_argument('--samples', type=int, default=256)
    parser.add_argument('--size', type=int, default=960, help='Image width; height is 3/4 width')
    parser.add_argument('--render', action='store_true', help='Render both PNG and raw EXR after conversion')
    args = parser.parse_args()
    if not 1 <= args.samples <= 65536 or not 64 <= args.size <= 4096:
        parser.error('samples must be 1..65536 and size 64..4096')
    binary = args.rypath.resolve(strict=True)
    directory = args.output_dir.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    source = directory/'source'
    source.mkdir()
    assets = (material_study(source), texture_study(source))
    manifest = dict(schema=1, license='GPL-3.0-only', provenance='Original project-authored geometry and textures; no external assets',
                    generator_sha256=sha256(Path(__file__)), renderer=str(binary), renderer_sha256=sha256(binary),
                    settings=dict(width=args.size, height=round(args.size*.75), samples=args.samples, seed=417,
                                  sampler='Sobol fastowen', filter='triangle', sensor='cie1931', iso=55, maxdepth=8),
                    source={p.name: sha256(p) for p in sorted(source.iterdir())}, scenes=[])
    for index, asset in enumerate(assets):
        bundle = directory/asset.stem
        command = run([binary, '--convert-gltf', asset, '--output', bundle], directory)
        wrapper = bundle/'studio.pbrt'
        wrapper.write_text(studio(args.size, args.samples, index == 1))
        check = run([binary, '--check-pbrt', wrapper], directory)
        entry = dict(asset=asset.relative_to(directory).as_posix(), bundle=bundle.name,
                     scene=wrapper.relative_to(directory).as_posix(), wrapper_sha256=sha256(wrapper), commands=[command, check])
        if args.render:
            for extension in ('png', 'exr'):
                output = directory/f'{asset.stem}.{extension}'
                entry['commands'].append(run([binary, '--render-pbrt', wrapper, '--output', output], directory))
                entry[extension] = dict(path=output.name, sha256=sha256(output))
        manifest['scenes'].append(entry)
    (directory/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (directory/'README.txt').write_text('''Original rypath glTF showcases (GPL-3.0-only)

material-study: twelve smooth imported glTF spheres on a neutral studio floor.
Front-to-back metallic rows: 0, 0.5, 1. Left-to-right roughness: 0, .12, .35, .7.
Copper and blue factors alternate by column; large softboxes reveal reflection width.

texture-study: imported GLB with original asymmetric native-UV color and packed
metallic/roughness textures, tangent-space normal detail, a mirrored sphere,
a perforated brass MASK screen casting shadows, and a blue BLEND veil in front
of the mirrored sphere. The environment, studio floor, camera and lights are
PBRT wrapper content; every featured material and object uses actual glTF import.

PNG is display output; EXR retains raw linear render output. No denoising.
manifest.json records source hashes, wrappers, settings and executed commands.
''')
    print(f'Artifacts: {directory}')


if __name__ == '__main__':
    main()
