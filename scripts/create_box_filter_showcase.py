#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage three box-radius views of the owned asymmetric emitter scene."""
import argparse
import hashlib
import json
from pathlib import Path
import compare_box_filters as suite

RADII = {'narrow': (.125, .125), 'default': (.5, .5), 'wide': (12., 4.)}


def generate(directory, width=768, height=512, spp=512):
    directory.mkdir(parents=True, exist_ok=True)
    records = {}
    base = suite.staged_source('pinhole-anisotropic')
    for name, (x, y) in RADII.items():
        source = base.replace(b'"integer xresolution" [8]', f'"integer xresolution" [{width}]'.encode())
        source = source.replace(b'"integer yresolution" [8]', f'"integer yresolution" [{height}]'.encode())
        source = source.replace(b'"integer pixelsamples" [4096]', f'"integer pixelsamples" [{spp}]'.encode())
        source = source.replace(b'"float xradius" [1.5]', f'"float xradius" [{x}]'.encode())
        source = source.replace(b'"float yradius" [0.75]', f'"float yradius" [{y}]'.encode())
        (directory/(name+'.pbrt')).write_bytes(source)
        records[name] = {'xradius': x, 'yradius': y, 'sha256': hashlib.sha256(source).hexdigest()}
    (directory/'manifest.json').write_text(json.dumps({'resolution': [width, height], 'spp': spp,
        'source': 'owned box-filters/pinhole-anisotropic.pbrt', 'scenes': records}, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    generate(args.output_dir)
