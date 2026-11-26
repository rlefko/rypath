#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Derive arithmetic probes without changing their executed production helpers."""

import argparse
import hashlib
import json
from pathlib import Path
import re

RENDER = b'#include "render.metal"\n'
ISOLATED = b'#include "canonical_render.metal"\n'
TRANSPORT = b'#include "packet_transport.h"\n'
ISOLATED_TRANSPORT = b'#include "canonical_packet_transport.h"\n'
PT_STEP = b'#include "packet_pt_step.h"\n'
TRANSPORT_END = PT_STEP + b'} // namespace rypath_bdpt\n'
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


def render_isolation_edits(render, replacement):
    render_rows = inventory(render, RENDER_KERNELS)
    if not render.endswith(TAIL) or render.count(TAIL) != 1:
        raise ValueError('Unexpected renderer entrypoint include tail')
    if render.count(TRANSPORT) != 1 or replacement in render:
        raise ValueError('Unexpected renderer packet transport include')
    at = render.index(TRANSPORT)
    edits = [('packet transport include', at, at + len(TRANSPORT), replacement)]
    edits += [omission(render, name, begin, end) for name, begin, end in render_rows]
    for include in TAIL.splitlines(keepends=True):
        if not include.strip():
            continue
        if render.count(include) != 1:
            raise ValueError('Missing or duplicate entrypoint include')
        begin = render.index(include)
        edits.append(omission(render, include.decode().strip(), begin, begin + len(include)))
    return edits


def transform_details(render, probe, transport):
    inventory(probe, PROBE_KERNELS + FULL_KERNELS)
    if probe.count(RENDER) != 1 or ISOLATED in probe or probe.count(FULL_MARKER) != 1:
        raise ValueError('Unexpected probe include or full-context boundary')
    if transport.count(PT_STEP) != 1 or not transport.endswith(TRANSPORT_END):
        raise ValueError('Unexpected packet transport PT-step boundary')
    inventory(transport, ())
    edits = render_isolation_edits(render, ISOLATED_TRANSPORT)
    isolated_render, render_changes = apply(render, edits)
    begin = probe.index(FULL_MARKER)
    at = probe.index(RENDER)
    isolated_probe, probe_changes = apply(probe, [
        ('render include', at, at + len(RENDER), ISOLATED),
        omission(probe, 'full-context suffix', begin, len(probe)),
    ])
    at = transport.index(PT_STEP)
    isolated_transport, transport_changes = apply(transport, [
        omission(transport, 'unused packet PT-step include', at, at + len(PT_STEP)),
    ])
    inventory(isolated_render, ())
    inventory(isolated_probe, PROBE_KERNELS)
    inventory(isolated_transport, ())
    return (isolated_render, isolated_probe, isolated_transport,
            render_changes, probe_changes, transport_changes)


def transform(render, probe, transport):
    return transform_details(render, probe, transport)[:3]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def helper_closure(render_path, probe_path, isolated_render, isolated_probe, virtual_helpers=None):
    """Check retained quoted includes contain no extra kernel entrypoints."""
    virtual = {render_path.resolve(): isolated_render, probe_path.resolve(): isolated_probe,
               (render_path.parent / 'canonical_render.metal').resolve(): isolated_render}
    virtual.update(virtual_helpers or {})
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
    transport_path = render_path.parent / 'packet_transport.h'
    if not transport_path.is_file():
        raise ValueError('Missing production packet transport header')
    render, probe = render_path.read_bytes(), probe_path.read_bytes()
    transport = transport_path.read_bytes()
    (isolated_render, isolated_probe, isolated_transport,
     render_changes, probe_changes, transport_changes) = transform_details(render, probe, transport)
    closure = helper_closure(render_path, probe_path, isolated_render, isolated_probe, {
        (render_path.parent / 'canonical_packet_transport.h').resolve(): isolated_transport,
    })
    if str((render_path.parent / 'packet_pt_step.h').resolve()) in closure:
        raise ValueError('Unused packet PT-step escaped the compilation boundary')
    generated = (('canonical_render.metal', isolated_render),
                 ('canonical_probe.metal', isolated_probe),
                 ('canonical_packet_transport.h', isolated_transport))
    result = {'schema': 3,
              'scope': ('Five test entrypoints; unused packet PT-step expansion omitted; '
                        'executed production helpers and full-context library unchanged'),
              'entrypoints': list(PROBE_KERNELS),
              'source_files': {str(p.resolve()): digest(data)
                               for p, data in ((render_path, render), (probe_path, probe),
                                               (transport_path, transport))},
              'retained_helper_files': closure,
              'generated_files': {name: digest(data) for name, data in generated},
              'edits': {name: [{key: value for key, value in row.items()
                                if key not in ('original', 'replacement')} |
                               {'original_sha256': digest(row['original']),
                                'replacement_sha256': digest(row['replacement'])} for row in rows]
                        for name, rows in (('render', render_changes), ('probe', probe_changes),
                                           ('transport', transport_changes))},
              'numeric_or_capability_policy_changes': False}
    # Every layout/closure/reversibility check precedes any output creation.
    manifest = (json.dumps(result, indent=2) + '\n').encode()
    output.mkdir(parents=True, exist_ok=True)
    for name, data in (*generated, ('generation.json', manifest)):
        temporary = output / (name + '.tmp')
        temporary.write_bytes(data)
        temporary.replace(output / name)


CURRENT_RENDER = b'#include "current_render.metal"\n'
CURRENT_TRANSPORT = b'#include "current_packet_transport.h"\n'
CURRENT_STATE = b'#include "current_packet_pt_state.h"\n'
CURRENT_PROBE_RENDER = b'#include "../shaders/render.metal"\n'
CURRENT_KERNELS = ('rypath_hero_current_differential', 'rypath_hero_current_preparation')
STATE_END = b'#include "hero_pt_state.h"\n'
STATE_NEXT = b'template <typename State>\ninline void packet_pt_initialize('


def transform_current_details(render, probe, transport, step):
    inventory(probe, CURRENT_KERNELS)
    inventory(transport, ())
    inventory(step, ())
    if probe.count(CURRENT_PROBE_RENDER) != 1 or CURRENT_RENDER in probe:
        raise ValueError('Unexpected current probe render include')
    if transport.count(PT_STEP) != 1 or not transport.endswith(TRANSPORT_END):
        raise ValueError('Unexpected current transport PT-step boundary')
    if step.count(STATE_END) != 1:
        raise ValueError('Unexpected current state prefix boundary')
    end = step.index(STATE_END) + len(STATE_END)
    if not step[end:].startswith(STATE_NEXT):
        raise ValueError('Unexpected code after current state prefix')
    if (re.search(rb'\bpacket_pt_(initialize|trace|step|result)\s*\(', masked(step[:end])) or
            re.search(rb'(?m)^[ \t]*#[ \t]*include[ \t]+"packet_pt_event_body\.inc"', step[:end])):
        raise ValueError('PT event code moved into current state prefix')
    render, render_edits = apply(render, render_isolation_edits(render, CURRENT_TRANSPORT))
    at = probe.index(CURRENT_PROBE_RENDER)
    probe, probe_edits = apply(probe, [
        ('render include', at, at + len(CURRENT_PROBE_RENDER), CURRENT_RENDER),
    ])
    at = transport.index(PT_STEP)
    transport, transport_edits = apply(transport, [
        ('state-only include', at, at + len(PT_STEP), CURRENT_STATE),
    ])
    step, step_edits = apply(step, [omission(step, 'unused PT event suffix', end, len(step))])
    return (render, probe, transport, step,
            render_edits, probe_edits, transport_edits, step_edits)


def generate_current(render_path, probe_path, output):
    transport_path, step_path = (render_path.parent / name for name in
                                 ('packet_transport.h', 'packet_pt_step.h'))
    sources = tuple((p, p.read_bytes()) for p in
                    (render_path, probe_path, transport_path, step_path))
    render, probe, transport, step, *edits = transform_current_details(
        *(data for _, data in sources))
    generated = (('current_render.metal', render), ('current_probe.metal', probe),
                 ('current_packet_transport.h', transport), ('current_packet_pt_state.h', step))
    closure = helper_closure(render_path, probe_path, render, probe, {
        (render_path.parent / name).resolve(): data for name, data in generated
    })
    if str(step_path.resolve()) in closure:
        raise ValueError('Full PT event header escaped current compilation boundary')
    record = {'schema': 1, 'scope': 'Current hero probes; unused PT event closure omitted',
              'entrypoints': list(CURRENT_KERNELS),
              'source_files': {str(p.resolve()): digest(data) for p, data in sources},
              'retained_helper_files': closure,
              'generated_files': {name: digest(data) for name, data in generated},
              'edits': {name: [{'name': row['name'], 'begin': row['begin'], 'end': row['end'],
                                'original_sha256': digest(row['original']),
                                'replacement_sha256': digest(row['replacement'])}
                               for row in rows]
                        for name, rows in zip(('render', 'probe', 'transport', 'step'), edits)},
              'numeric_or_capability_policy_changes': False}
    # Validation and closure admission finish before any output is touched.
    manifest = (json.dumps(record, indent=2) + '\n').encode()
    output.mkdir(parents=True, exist_ok=True)
    for name, data in (*generated, ('generation.json', manifest)):
        temporary = output / (name + '.tmp')
        temporary.write_bytes(data)
        temporary.replace(output / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=('canonical', 'current'), default='canonical')
    parser.add_argument('--render', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    generate_function = generate_current if args.kind == 'current' else generate
    generate_function(args.render, args.probe, args.output_dir)


if __name__ == '__main__':
    main()
