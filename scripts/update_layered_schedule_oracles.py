#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build stock/modified PBRT schedule tools, then authenticate an f-only overlay.

No oracle is executed by this script. Root runs both --fixtures commands explicitly.
The modified tool is a diagnostic reference, not unmodified PBRT conformance.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = 'b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f'
OLD = '-w, wi, mode, BxDFReflTransFlags::Transmission);'
NEW = 'wi, -w, !mode, BxDFReflTransFlags::Transmission);'
STOCK_HEADER_SHA = '0455cda301f1241e7c6b7437a6ab95bde55f1cfdbda98fece3cb78c570726e34'
CORRECTED_HEADER_SHA = 'f0f79357fa56f4b5e38e51aac3f5c376d2578c5a23f57de5b3e7929c272209ae'


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def build(out, source, build_dir):
    require(not out.exists(), 'fresh build directory required; preserve every failed attempt')
    out.mkdir(parents=True)
    receipt = {'status': 'preparing', 'reference_kind': 'modified-PBRT-two-MIS-proposal-calls',
               'pbrt_revision': REVISION, 'commands': [], 'bindings': {}}
    target = out / 'build.json'
    try:
        revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        require(revision == REVISION, 'wrong pinned PBRT revision')
        subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD'], check=True)
        header = source / 'src/pbrt/bxdfs.h'
        require(sha(header) == STOCK_HEADER_SHA, 'stock bxdfs.h changed')
        original = header.read_text()
        require(original.count(OLD) == 2, 'two exact proposal calls required')
        overlay = out / 'include/pbrt/bxdfs.h'
        overlay.parent.mkdir(parents=True)
        overlay.write_text(original.replace(OLD, NEW))
        require(sha(overlay) == CORRECTED_HEADER_SHA, 'unexpected corrected header bytes')
        lines = subprocess.check_output(['ninja', '-C', str(build_dir), '-t', 'commands', 'pbrt'], text=True).splitlines()
        wrapper = ROOT / 'scripts/pbrt_layered_schedule_probe.cpp'
        base = shlex.split(next(line for line in lines if 'pbrt/cmd/pbrt.cpp.o -c' in line))
        base = base[:base.index('-MD')]
        require('-O3' in base and '-ffp-contract=off' in base, 'lost pinned arithmetic flags')
        objects = [out / 'stock.o', out / 'corrected.o', out / 'bxdfs.o']
        commands = []
        for name, obj in zip(('stock', 'corrected'), objects[:2]):
            command = list(base)
            if name == 'corrected':
                command.insert(1, '-I' + str(out / 'include'))
            commands.append(command + ['-I' + str(ROOT / 'src'), '-MD', '-MF', str(out / (name + '.d')),
                            '-c', str(wrapper), '-o', str(obj)])
        bxdf = shlex.split(next(line for line in lines if 'pbrt/bxdfs.cpp.o -c' in line))
        bxdf = bxdf[:bxdf.index('-MD')]
        bxdf.insert(1, '-I' + str(out / 'include'))
        commands.append(bxdf + ['-MD', '-MF', str(out / 'bxdfs.d'), '-c', str(source / 'src/pbrt/bxdfs.cpp'),
                                '-o', str(objects[2])])
        link = shlex.split(lines[-1])
        link = link[2:link.index('&&', 2)]
        for name, obj in zip(('stock', 'corrected'), objects[:2]):
            command = list(link)
            command[command.index('CMakeFiles/pbrt_exe.dir/src/pbrt/cmd/pbrt.cpp.o')] = str(obj)
            command[command.index('-o') + 1] = str(out / name)
            if name == 'corrected':
                # Explicit TU precedes archive: extern-template implementations must be replaced.
                command.insert(command.index('libpbrt_lib.a'), str(objects[2]))
            commands.append(command)
        receipt.update(status='building', commands=commands, stock_header_sha256=sha(header),
                       corrected_header_sha256=sha(overlay), cwd=str(build_dir))
        save(target, receipt)
        for index, command in enumerate(commands):
            with (out / f'command-{index}.log').open('xb') as log:
                result = subprocess.run(command, cwd=build_dir, stdout=log, stderr=subprocess.STDOUT)
            receipt.setdefault('exits', []).append(result.returncode)
            save(target, receipt)
            require(result.returncode == 0, f'command {index} failed; evidence retained')
        bindings = {wrapper, ROOT / 'tests/layered_schedule_oracles.h', ROOT / 'src/layered_probe_wire.h',
                    header, overlay, source / 'src/pbrt/bxdfs.cpp', build_dir / 'libpbrt_lib.a',
                    Path(commands[0][0]).resolve(), Path(__file__).resolve()}
        # Compiler-generated dependency closure, including generated configuration headers.
        for name in ('stock', 'corrected', 'bxdfs'):
            dependency = out / (name + '.d')
            words = shlex.split(dependency.read_text().replace('\\\n', ' ').split(':', 1)[1])
            for word in words:
                path = Path(word)
                bindings.add(path if path.is_absolute() else build_dir / path)
        for command in commands[-2:]:
            for word in command:
                path = Path(word)
                path = path if path.is_absolute() else build_dir / path
                if path.is_file():
                    bindings.add(path.resolve())
        receipt.update(status='complete-CPU-build-no-oracle-run',
                       bindings={str(p.resolve()): sha(p) for p in sorted(bindings)},
                       binaries={name: {'path': str(out / name), 'sha256': sha(out / name)}
                                 for name in ('stock', 'corrected')})
    except BaseException as error:
        receipt.update(status='failed-preserved', error=repr(error))
        raise
    finally:
        save(target, receipt)


def bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def output_rows(path):
    rows = [[float(word) for word in line.split()] for line in path.read_text().splitlines()]
    require(len(rows) == 28 and all(len(row) == 8 and all(math.isfinite(x) for x in row)
                                   for row in rows), 'exactly 28 finite eight-column results required')
    return [[bits(x) for x in row] for row in rows]


def generate(receipt_path, stock_path, corrected_path, destination):
    receipt = json.loads(receipt_path.read_text())
    require(receipt['status'] == 'complete-CPU-build-no-oracle-run' and
            receipt.get('stock_header_sha256') == STOCK_HEADER_SHA and
            receipt.get('corrected_header_sha256') == CORRECTED_HEADER_SHA,
            'build incomplete or wrong numerical source')
    for path, digest in receipt['bindings'].items():
        require(sha(path) == digest, 'build binding changed: ' + path)
    for item in receipt['binaries'].values():
        require(sha(item['path']) == item['sha256'], 'oracle binary changed')
    fixture_path = ROOT / 'tests/layered_schedule_oracles.h'
    pattern = r'\{\{(.*?)\},\s*\{(.*?)\},\s*"([^"]+)"\}'
    entries = re.findall(pattern, fixture_path.read_text(), re.S)
    require(len(entries) == 28, 'stock fixture parser requires exactly 28 records')
    expected = [[bits(word.strip().removesuffix('f')) for word in entry[1].split(',') if word.strip()]
                for entry in entries]
    stock, corrected = output_rows(stock_path), output_rows(corrected_path)
    require(stock == expected, 'stock eight-column results failed exact retained-golden reproduction')
    require(all(a[1:] == b[1:] for a, b in zip(stock, corrected)),
            'modified reference changed columns other than directional f')
    require(any(a[0] != b[0] for a, b in zip(stock, corrected)), 'no directional f changed')
    require(all(struct.unpack('<f', struct.pack('<I', row[0]))[0] >= 0 for row in corrected),
            'negative corrected directional f')
    require(not destination.exists() and not destination.with_suffix('.json').exists(),
            'preserve existing overlay/receipt; choose a new path for review')
    provenance = {'build_receipt_sha256': sha(receipt_path), 'stock_output_sha256': sha(stock_path),
                  'corrected_output_sha256': sha(corrected_path), 'stock_fixture_sha256': sha(fixture_path),
                  'corrected_header_sha256': receipt['corrected_header_sha256']}
    text = '// SPDX-License-Identifier: GPL-3.0-only\n// GENERATED modified-PBRT reference, not stock conformance.\n'
    text += ''.join('// ' + k + ': ' + v + '\n' for k, v in provenance.items())
    text += '#pragma once\n#include <array>\n#include <cstdint>\nnamespace layered_schedule_corrected {\n'
    text += 'inline constexpr std::array<std::uint32_t, 28> directional_f_bits = {\n'
    text += ''.join(f'    0x{row[0]:08x}u, // {entry[2]}\n' for row, entry in zip(corrected, entries))
    text += '};\n} // namespace layered_schedule_corrected\n'
    destination.write_text(text)
    save(destination.with_suffix('.json'), dict(provenance, output_sha256=sha(destination),
         changed_f=sum(a[0] != b[0] for a, b in zip(stock, corrected)), all_other_columns_bit_identical=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('build', 'generate'))
    parser.add_argument('--root-go-cpu', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT / '.cache/pbrt-reference/source')
    parser.add_argument('--build-dir', type=Path, default=ROOT / '.cache/pbrt-reference/build')
    parser.add_argument('--build-receipt', type=Path)
    parser.add_argument('--stock-output', type=Path)
    parser.add_argument('--corrected-output', type=Path)
    args = parser.parse_args()
    if args.mode == 'build':
        require(args.root_go_cpu, 'root CPU scheduling GO required')
        build(args.output.resolve(), args.source.resolve(), args.build_dir.resolve())
    else:
        require(all((args.build_receipt, args.stock_output, args.corrected_output)), 'missing generation inputs')
        generate(args.build_receipt, args.stock_output, args.corrected_output, args.output)


if __name__ == '__main__':
    main()
