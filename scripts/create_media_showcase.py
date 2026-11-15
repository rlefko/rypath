#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate four owned homogeneous-medium studios with external label metadata."""
import argparse
import hashlib
import json
from pathlib import Path

from create_dielectric_showcase import quad
from create_surface_showcase import sphere

NAMES = ('absorption', 'scattering', 'emission', 'presets')
PINNED_PBRT = 'b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f'


def scene(name, width=640, height=400, spp=512):
    if name not in NAMES:
        raise ValueError('unknown medium showcase')
    if min(width, height, spp) < 1:
        raise ValueError('resolution and samples must be positive')
    text = f'''# SPDX-License-Identifier: GPL-3.0-only
# Original owned geometry. Label and preset provenance: showcase.json.
LookAt 0 1 -7  0 .1 3.5  0 1 0
Camera "perspective" "float fov" 28
Film "rgb" "integer xresolution" {width} "integer yresolution" {height}
 "string sensor" "cie1931" "float iso" 80 "bool savefp16" false
PixelFilter "triangle"
Sampler "sobol" "integer pixelsamples" {spp} "integer seed" 42 "string randomization" "fastowen"
Integrator "volpath" "integer maxdepth" 12
WorldBegin
LightSource "infinite" "rgb L" [.025 .035 .055]
Texture "floor" "spectrum" "checkerboard" "rgb tex1" [.08 .11 .16] "rgb tex2" [.28 .32 .4] "float uscale" 10 "float vscale" 10
Material "diffuse" "texture reflectance" "floor"
'''
    text += quad([(-10,-1.05,-4), (10,-1.05,-4), (10,-1.05,10), (-10,-1.05,10)])
    text += 'Material "diffuse" "rgb reflectance" [.12 .16 .24]\n'
    text += quad([(-10,-1.05,6), (10,-1.05,6), (10,8,6), (-10,8,6)])
    for points, rgb, scale in [
        ([(-4,4,0),(-.4,4,0),(-.4,4,6),(-4,4,6)], [1,.75,.45], 10),
        ([(3.5,-.3,1),(3.8,-.3,1),(3.8,3.5,1),(3.5,3.5,1)], [.35,.6,1], 20),
    ]:
        text += 'AttributeBegin\nMaterial "diffuse" "rgb reflectance" [0 0 0]\n'
        text += f'AreaLightSource "diffuse" "rgb L" [{" ".join(map(str, rgb))}] "float scale" {scale} "bool twosided" true\n'
        text += quad(points)+'AttributeEnd\n'
    labels = []
    def orb(label, parameters, x, glass=False):
        nonlocal text
        medium = 'medium'+str(len(labels))
        text += f'MakeNamedMedium "{medium}" "string type" "homogeneous" '+parameters+'\n'
        text += 'AttributeBegin\n'
        text += 'Material "dielectric" "float eta" 1.45\n' if glass else 'Material "interface"\n'
        text += f'MediumInterface "{medium}" ""\n'+sphere(x, 0, 3.4, .95)+'AttributeEnd\n'
        labels.append({'label': label, 'center': [x, 0, 3.4], 'radius': .95,
                       'boundary': 'smooth dielectric eta=1.45' if glass else 'null interface',
                       'medium_parameters': parameters})
    if name == 'absorption':
        orb('Amber / spectral absorption', '"rgb sigma_a" [.12 .6 1.6] "rgb sigma_s" [0 0 0]', -2.1, True)
        orb('Cyan / spectral absorption', '"rgb sigma_a" [1.4 .18 .1] "rgb sigma_s" [0 0 0]', 0, True)
        orb('Rose / spectral absorption', '"rgb sigma_a" [.2 1.3 .35] "rgb sigma_s" [0 0 0]', 2.1, True)
    elif name == 'scattering':
        orb('Backward / g = -0.6', '"rgb sigma_a" [.06 .08 .12] "rgb sigma_s" [1.1 1.2 1.4] "float g" -.6', -2.1)
        orb('Isotropic / g = 0', '"rgb sigma_a" [.06 .08 .12] "rgb sigma_s" [1.1 1.2 1.4] "float g" 0', 0)
        orb('Forward / g = 0.7', '"rgb sigma_a" [.06 .08 .12] "rgb sigma_s" [1.1 1.2 1.4] "float g" .7', 2.1)
    elif name == 'emission':
        orb('Warm / emitting absorber', '"rgb sigma_a" [.8 .8 .8] "rgb sigma_s" [0 0 0] "rgb Le" [1 .16 .025] "float Lescale" .6', -2.1)
        orb('Cool / emission and scattering', '"rgb sigma_a" [.6 .6 .6] "rgb sigma_s" [.7 .7 .7] "rgb Le" [.08 .5 1] "float Lescale" .6 "float g" .3', 0)
        orb('Nested / cool core, amber atmosphere', '"rgb sigma_a" [.1 .3 .7] "rgb sigma_s" [.3 .3 .3] "float g" .4', 2.1)
        text += 'MakeNamedMedium "core" "string type" "homogeneous" "rgb sigma_a" [.8 .8 .8] "rgb sigma_s" [.2 .2 .2] "rgb Le" [.15 .7 1] "float Lescale" 1\n'
        text += 'AttributeBegin\nMaterial "interface"\nMediumInterface "core" "medium2"\n'+sphere(2.1,0,3.4,.5)+'AttributeEnd\n'
    else:
        orb('Apple / pinned measured coefficients', '"string preset" "Apple" "float scale" .45 "float g" .3', -2.1)
        orb('Skin1 / pinned measured coefficients', '"string preset" "Skin1" "float scale" 1 "float g" 0', 0)
        orb('Ocean water / pinned measured coefficients', '"string preset" "Pacific Ocean Surface Water" "float scale" 12 "float g" .7', 2.1)
    return text, labels


def generate(output_dir, width=640, height=400, spp=512):
    if min(width, height, spp) < 1:
        raise ValueError('resolution and samples must be positive')
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    labels, files = {}, {}
    for name in NAMES:
        text, labels[name] = scene(name, width, height, spp)
        payload = text.encode()
        (output_dir/(name+'.pbrt')).write_bytes(payload)
        files[name] = {'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}
    metadata = {'license': 'GPL-3.0-only for original owned geometry and scenes',
                'resolution': [width, height], 'spp': spp, 'seed': 42,
                'labels': labels, 'files': files, 'pbrt_revision': PINNED_PBRT,
                'presets': {'source': 'src/pbrt/media.cpp:GetMediumScatteringProperties',
                    'conversion': 'Pinned sRGB RGBUnboundedSpectrum, with explicit scene scale; no unit or anisotropy correction',
                    'Apple': {'sigma_a': [.0030,.0034,.046], 'sigma_s': [2.29,2.39,1.97]},
                    'Skin1': {'sigma_a': [.032,.17,.48], 'sigma_s': [.74,.88,1.01]},
                    'Pacific Ocean Surface Water': {'sigma_a': [.031845,.031324,.030147], 'sigma_s': [.0001764,.00032095,.00019617]},
                    'references': ['Jensen et al., A Practical Model for Subsurface Light Transport, SIGGRAPH 2001',
                                   'Narasimhan et al., Acquiring Scattering Properties of Participating Media by Dilution, SIGGRAPH 2006']},
                'model': 'Homogeneous scalar-wavelength transport; HG phase, explicit medium interfaces, finite depth 12; absorption-coupled photometrically normalized Le',
                'outputs': 'Render each source to raw .exr and display .png; labels remain external; no denoising',
                'coverage': 'Original owned studios using pinned preset data; not original external scene-corpus compatibility'}
    (output_dir/'showcase.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=400)
    parser.add_argument('--spp', type=int, default=512)
    args = parser.parse_args()
    try:
        generate(args.output_dir, args.width, args.height, args.spp)
    except ValueError as error:
        parser.error(str(error))
    for name in NAMES:
        print(args.output_dir/(name+'.pbrt'))


if __name__ == '__main__':
    main()
