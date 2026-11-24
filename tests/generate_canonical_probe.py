#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Derive five arithmetic probes without changing their production helpers."""

import argparse
import hashlib
import json
from pathlib import Path
import re

RENDER = b'#include "render.metal"\n'
ISOLATED = b'#include "canonical_render.metal"\n'
PREFIX = b'// canonical-probe omitted: '
RENDER_KERNELS = (
    'rypath_checkpoint_contract', 'rypath_sobol_probe', 'rypath_lens_probe',
    'rypath_probe_blackbody', 'rypath_scattering_continuation_probe',
    'rypath_scattering_frame_probe', 'rypath_render_sky', 'rypath_render_paths',
    'rypath_qmc_probe', 'rypath_render_packets',
)
PROBE_KERNELS = (
    'rypath_probe_light_geometry', 'rypath_probe_density_conversion',
    'rypath_probe_offset_rounding', 'rypath_probe_canonical_partition',
    'rypath_probe_canonical_finalizer',
)
FULL_KERNELS = ('strategy_context21_probe', 'rypath_probe_canonical_full_readback')
TAIL = (b'#include "wavefront.metal"\n\n#include "area_reuse.metal"\n'
        b'#include "reconstruction_guides.metal"\n\n#include "camera_medium.metal"\n')
FULL_MARKER = b'// Original full retention reproducer: all executed body statements retained.\n'


def masked(source):
    """Mask comments/quoted literals, preserving every byte offset and newline."""
    out = bytearray(source)
    i = 0
    while i < len(source):
        start = i
        if source[i:i + 2] == b'//':
            end = source.find(b'\n', i + 2)
            i = len(source) if end < 0 else end
        elif source[i:i + 2] == b'/*':
            end = source.find(b'*/', i + 2)
            if end < 0:
                raise ValueError('Unterminated block comment')
            i = end + 2
        elif source[i] in (34, 39):
            quote = source[i]
            i += 1
            while i < len(source) and source[i] != quote:
                if source[i] == 10:
                    raise ValueError('Newline in quoted literal')
                i += 2 if source[i] == 92 else 1
            if i >= len(source):
                raise ValueError('Unterminated quoted literal')
            i += 1
        else:
            i += 1
            continue
        for at in range(start, i):
            if out[at] != 10:
                out[at] = 32
    return bytes(out)


def kernels(source):
    """Recognize only the reviewed kernel-void layout, not general C++ syntax."""
    clean = masked(source)
    starts = list(re.finditer(rb'(?m)^kernel void ([A-Za-z_][A-Za-z_0-9]*)\(', clean))
    if len(starts) != len(re.findall(rb'\bkernel\b', clean)):
        raise ValueError('Unexpected kernel declaration layout')
    result = []
    for match in starts:
        begin = clean.find(b'{', match.end())
        if begin < 0 or b';' in clean[match.end():begin]:
            raise ValueError('Kernel body missing')
        depth = 1
        end = begin + 1
        while depth and end < len(clean):
            depth += (clean[end] == 123) - (clean[end] == 125)
            end += 1
        if depth or clean[end:end + 1] != b'\n':
            raise ValueError('Unbalanced kernel body or changed closing layout')
        result.append((match.group(1).decode(), match.start(), end + 1))
    # Detect overlapping bodies (for example, a missing brace swallowing a kernel).
    if any(a[2] > b[1] for a, b in zip(result, result[1:])):
        raise ValueError('Overlapping kernel definitions')
    return result


def inventory(source, expected):
    found = kernels(source)
    if tuple(row[0] for row in found) != expected:
        raise ValueError('Unexpected kernel inventory')
    return found


def apply(source, edits):
    """Apply reviewed nonoverlapping slices, then prove exact reversibility."""
    if PREFIX in source:
        raise ValueError('Ambiguous generated omission marker')
    out = source
    changes = []
    for name, begin, end, replacement in sorted(edits, key=lambda row: row[1], reverse=True):
        original = source[begin:end]
        if any(not (end <= item['begin'] or begin >= item['end']) for item in changes):
            raise ValueError('Overlapping source edits')
        changes.append({'name': name, 'begin': begin, 'end': end,
                        'original': original, 'replacement': replacement})
        out = out[:begin] + replacement + out[end:]
    restored = out
    for change in reversed(changes):
        replacement = change['replacement']
        if restored.count(replacement) != 1:
            raise ValueError('Ambiguous reversible replacement')
        restored = restored.replace(replacement, change['original'], 1)
    if restored != source or out.count(b'\n') != source.count(b'\n'):
        raise ValueError('Transformation is not byte-for-byte reversible')
    return out, changes


def omission(source, name, begin, end):
    return (name, begin, end,
            PREFIX + name.encode() + b'\n' * source[begin:end].count(b'\n'))


def transform_details(render, probe):
    render_rows = inventory(render, RENDER_KERNELS)
    inventory(probe, PROBE_KERNELS + FULL_KERNELS)
    if not render.endswith(TAIL) or render.count(TAIL) != 1:
        raise ValueError('Unexpected renderer entrypoint include tail')
    if probe.count(RENDER) != 1 or ISOLATED in probe or probe.count(FULL_MARKER) != 1:
        raise ValueError('Unexpected probe include or full-context boundary')
    edits = [omission(render, name, begin, end) for name, begin, end in render_rows]
    for include in TAIL.splitlines(keepends=True):
        if not include.strip():
            continue
        if render.count(include) != 1:
            raise ValueError('Missing or duplicate entrypoint include')
        begin = render.index(include)
        edits.append(omission(render, include.decode().strip(), begin, begin + len(include)))
    isolated_render, render_changes = apply(render, edits)
    begin = probe.index(FULL_MARKER)
    at = probe.index(RENDER)
    isolated_probe, probe_changes = apply(probe, [
        ('render include', at, at + len(RENDER), ISOLATED),
        omission(probe, 'full-context suffix', begin, len(probe)),
    ])
    inventory(isolated_render, ())
    inventory(isolated_probe, PROBE_KERNELS)
    return isolated_render, isolated_probe, render_changes, probe_changes


def transform(render, probe):
    return transform_details(render, probe)[:2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def helper_closure(render_path, probe_path, isolated_render, isolated_probe):
    """Check retained quoted includes contain no extra kernel entrypoints."""
    virtual = {render_path.resolve(): isolated_render, probe_path.resolve(): isolated_probe,
               (render_path.parent / 'canonical_render.metal').resolve(): isolated_render}
    seen = {}
    pending = [render_path.resolve(), probe_path.resolve()]
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        source = virtual[path] if path in virtual else path.read_bytes()
        if path not in virtual and re.search(rb'\bkernel\b', masked(source)):
            raise ValueError('Unexpected kernel in retained helper include: ' + str(path))
        seen[path] = digest(source)
        # Comments cannot invent dependencies; unsupported include spellings fail
        # rather than silently escaping the checked local helper closure.
        for directive in re.finditer(rb'(?m)^[ \t]*#[ \t]*include\b[^\n]*', masked(source)):
            line = source[directive.start():directive.end()]
            include = re.fullmatch(rb'[ \t]*#[ \t]*include[ \t]+"([^"\n]+)"[ \t]*', line)
            if include is None:
                if re.fullmatch(rb'[ \t]*#[ \t]*include[ \t]+<[^>\n]+>[ \t]*', line):
                    continue
                raise ValueError('Unsupported retained helper include layout')
            relative = include.group(1).decode()
            candidates = [(path.parent / relative).resolve(),
                          (render_path.parent / relative).resolve()]
            target = next((p for p in candidates if p in virtual or p.is_file()), None)
            if target is None:
                raise ValueError('Unresolved retained helper include: ' + relative)
            pending.append(target)
    return {str(path): value for path, value in sorted(seen.items())
            if path not in virtual}


def generate(render_path, probe_path, output):
    render, probe = render_path.read_bytes(), probe_path.read_bytes()
    isolated_render, isolated_probe, render_changes, probe_changes = transform_details(render, probe)
    closure = helper_closure(render_path, probe_path, isolated_render, isolated_probe)
    result = {'schema': 2,
              'scope': 'Five test entrypoints; production helpers and full-context library unchanged',
              'entrypoints': list(PROBE_KERNELS),
              'source_files': {str(p.resolve()): digest(data)
                               for p, data in ((render_path, render), (probe_path, probe))},
              'retained_helper_files': closure,
              'generated_files': {name: digest(data) for name, data in (
                  ('canonical_render.metal', isolated_render), ('canonical_probe.metal', isolated_probe))},
              'edits': {name: [{key: value for key, value in row.items()
                                if key not in ('original', 'replacement')} |
                               {'original_sha256': digest(row['original']),
                                'replacement_sha256': digest(row['replacement'])} for row in rows]
                        for name, rows in (('render', render_changes), ('probe', probe_changes))},
              'numeric_or_capability_policy_changes': False}
    # Every layout/closure/reversibility check precedes any output creation.
    manifest = (json.dumps(result, indent=2) + '\n').encode()
    output.mkdir(parents=True, exist_ok=True)
    for name, data in (('canonical_render.metal', isolated_render),
                       ('canonical_probe.metal', isolated_probe), ('generation.json', manifest)):
        temporary = output / (name + '.tmp')
        temporary.write_bytes(data)
        temporary.replace(output / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    generate(args.render, args.probe, args.output_dir)


if __name__ == '__main__':
    main()
