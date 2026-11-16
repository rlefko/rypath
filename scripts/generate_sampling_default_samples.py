#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate three labeled sampling previews; optionally render through the product CLI."""
import argparse
import hashlib
import html
import json
from pathlib import Path
import re
import shutil
import subprocess

from compare_sampling_defaults import FIXTURES, staged_source
from generate_depth_of_field_samples import panel
from generate_projection_samples import pattern_png
from generate_spot_samples import box, diffuse, sphere


def film(width, height):
    return (f'Film "rgb" "integer xresolution" [{width}] "integer yresolution" [{height}]\n'
            ' "string sensor" "cie1931" "float iso" [100] "bool savefp16" [false]\n')


def generate(destination, samples=512, width=768, height=512):
    destination.mkdir(parents=True, exist_ok=True)
    (destination/'focus-chart.png').write_bytes(pattern_png())
    defaults = ('# SPDX-License-Identifier: GPL-3.0-only\n'
        '# Original sampling showcase; Film explicit, other descriptors omitted.\n'+
        film(width, height)+'WorldBegin\n'+
        'LightSource "infinite" "rgb L" [.025 .035 .07]\n'+
        'Material "diffuse" "rgb reflectance" [0 0 0]\n')
    for x, y, z, w, h, rgb in ((-5, -2, 7, 3.8, 4.3, (1.4, .12, .08)),
                              (-.7, -2.5, 8, 4.8, 4.1, (.08, .7, 1.4)),
                              (1.4, 1.3, 6, 2.6, 1.8, (1.3, .7, .08))):
        defaults += ('AttributeBegin\nAreaLightSource "diffuse" "rgb L" ['+
                     ' '.join(map(str, rgb))+']\n'+panel(x, y, z, w, h)+'AttributeEnd\n')
    (destination/'implicit-defaults.pbrt').write_text(defaults)

    studio = ('# SPDX-License-Identifier: GPL-3.0-only\n'
        '# Original Halton/Gaussian thin-lens chart studio.\n'
        'LookAt 0 1.8 0 0 1.8 1 0 1 0\n'
        'Camera "perspective" "float fov" [47] "float lensradius" [.18] "float focaldistance" [4]\n'+
        film(width, height)+
        'PixelFilter "gaussian" "float xradius" [2.5] "float yradius" [1.5] "float sigma" [.6]\n'+
        f'Sampler "halton" "string randomization" ["permutedigits"] "integer pixelsamples" [{samples}] "integer seed" [17]\n'
        'Integrator "path" "integer maxdepth" [6]\nWorldBegin\n'
        'LightSource "infinite" "float scale" [.12]\n'
        'LightSource "point" "point3 from" [-3 6 1] "rgb I" [95 85 70]\n'
        'LightSource "point" "point3 from" [4 4 7] "rgb I" [40 65 100]\n')
    studio += diffuse((.045, .055, .075))+box((-9, -.2, 0), (9, -.1, 17))
    studio += diffuse((.08, .10, .14))+box((-9, -.1, 16), (9, 9, 16.15))
    studio += 'Texture "chart" "spectrum" "imagemap" "string filename" ["focus-chart.png"] "string filter" ["ewa"]\n'
    for x, z, w, h in ((-2.3, 4, 2.1, 2.7), (.4, 10, 4.4, 5.4)):
        studio += diffuse((.018, .023, .032))+box((x-.10, .35, z), (x+w+.10, .35+h+.2, z+.18))
        studio += 'Material "diffuse" "texture reflectance" "chart"\n'+panel(x, .45, z-.015, w, h)
        studio += diffuse((.16, .19, .24))+box((x+w*.4, -.1, z+.04), (x+w*.6, .36, z+.14))
    studio += 'Material "conductor" "rgb reflectance" [.85 .52 .16] "float roughness" [.16] "bool remaproughness" [false]\n'
    studio += sphere((-.1, .48, 6.5), .55)
    (destination/'halton-studio.pbrt').write_text(studio)

    cornell = staged_source('cornell').decode()
    cornell = cornell.replace('See README.md and assets/Cornell-LICENSE.txt.', 'See Cornell-LICENSE.txt and previews.json.')
    cornell = re.sub(r'"integer xresolution" \[8\]', f'"integer xresolution" [{width}]', cornell)
    cornell = re.sub(r'"integer yresolution" \[8\]', f'"integer yresolution" [{height}]', cornell)
    cornell = cornell.replace('Sampler "halton" "string randomization" ["permutedigits"]',
                             'Sampler "zsobol" "string randomization" ["fastowen"]')
    cornell = cornell.replace('"integer pixelsamples" [4096]', f'"integer pixelsamples" [{samples}]')
    (destination/'zsobol-cornell.pbrt').write_text(cornell)
    shutil.copyfile(FIXTURES/'assets/Cornell-LICENSE.txt', destination/'Cornell-LICENSE.txt')
    records = [
        {'name': 'implicit-defaults', 'label': 'Implicit PBRT defaults · 16 samples per pixel',
         'description': 'Perspective + Gaussian + ZSobol/fastowen + volpath; only Film is explicit. Direct emission isolates camera/filter behavior.',
         'samples': 16, 'license': 'GPL-3.0-only'},
        {'name': 'halton-studio', 'label': f'Halton · Gaussian · thin lens · {samples} samples per pixel',
         'description': 'Owned near/far texture studio; permuted digits, anisotropic Gaussian support, raw render without denoising.',
         'samples': samples, 'license': 'GPL-3.0-only'},
        {'name': 'zsobol-cornell', 'label': f'ZSobol · Gaussian · {samples} samples per pixel',
         'description': 'CC0 Cornell derivative by Benedikt Bitterli, converted by Matt Pharr; maxdepth5. Selected derivative coverage, not new stock-scene conformance.',
         'samples': samples, 'license': 'CC0-1.0', 'notice': 'Cornell-LICENSE.txt'}]
    for record in records:
        path = destination/(record['name']+'.pbrt')
        record.update(scene=path.name, image=record['name']+'.png',
            scene_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), resolution=[width, height])
    metadata = {'coverage': 'Labeled selected previews; not a convergence, performance, or full-corpus acceptance study',
                'image_policy': 'Display PNG; raw renderer output, no denoising. Labels are outside image pixels.',
                'previews': records}
    (destination/'previews.json').write_text(json.dumps(metadata, indent=2)+'\n')
    gallery = '<!doctype html><meta charset="utf-8"><title>PBRT Sampling Previews</title>\n'
    gallery += '<style>body{background:#12151b;color:#edf0f5;font:16px system-ui;max-width:1200px;margin:40px auto}img{max-width:100%;height:auto}figure{margin:32px 0}p{color:#bcc4d0}</style>\n'
    for record in records:
        gallery += (f'<figure><h2>{html.escape(record["label"])}</h2>'
                    f'<img src="{record["image"]}" alt="{html.escape(record["label"])}">'
                    f'<figcaption>{html.escape(record["description"])}</figcaption></figure>\n')
    (destination/'index.html').write_text(gallery)
    return metadata


def render(destination, binary, metadata, *, runner=None):
    execute = runner or subprocess.run
    for record in metadata['previews']:
        name = record['name']
        command = [str(binary), '--render-pbrt', str(destination/record['scene']),
                   '--output', str(destination/record['image']), '--batch-size', '16']
        result = execute(command, capture_output=True, text=True, check=False)
        (destination/(name+'.stdout.log')).write_text(result.stdout or '')
        (destination/(name+'.stderr.log')).write_text(result.stderr or '')
        record['render'] = {'command': command, 'returncode': result.returncode}
        if result.returncode:
            (destination/'previews.json').write_text(json.dumps(metadata, indent=2)+'\n')
            raise subprocess.CalledProcessError(result.returncode, command)
        output = destination/record['image']
        record['image_sha256'] = hashlib.sha256(output.read_bytes()).hexdigest()
        (destination/'previews.json').write_text(json.dumps(metadata, indent=2)+'\n')
        print(f'Rendered {output}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=512)
    parser.add_argument('--width', type=int, default=768)
    parser.add_argument('--height', type=int, default=512)
    parser.add_argument('--render', action='store_true')
    parser.add_argument('--rypath', type=Path, default=Path(__file__).resolve().parents[1]/'build/release/rypath')
    args = parser.parse_args()
    if not 1 <= args.samples <= 1048576 or args.samples & (args.samples-1):
        parser.error('--samples must be a power of two from 1 through 1048576')
    if not 1 <= args.width <= 4096 or not 1 <= args.height <= 4096:
        parser.error('--width and --height must be from 1 through 4096')
    destination = args.output_dir.resolve()
    metadata = generate(destination, args.samples, args.width, args.height)
    if args.render:
        render(destination, args.rypath.resolve(strict=True), metadata)
    print(f'Preview gallery and metadata: {destination}', flush=True)


if __name__ == '__main__':
    main()
