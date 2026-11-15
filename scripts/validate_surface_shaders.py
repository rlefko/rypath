#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run affected Metal suites with an explicit shader-instrumentation time budget.

Uses CTest's actual commands without changing normal test deadlines or allowing
hardware skips. Build the selected directory first; do not rebuild it during this
run. This opt-in diagnostic is separate from fast PR tests.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

TESTS = ('texture_graph', 'surface_detail', 'hdr_texture', 'conductor_render',
         'dielectric_render', 'dielectric_bsdf', 'layered_bsdf', 'table_bsdf',
         'surface_materials')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    if not 0 < args.timeout <= 3600:
        parser.error('--timeout must be in (0, 3600] seconds')
    build = args.build_dir.resolve(strict=True)
    listing = json.loads(subprocess.check_output(
        ['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True))
    tests = {test['name']: test for test in listing['tests']}
    names = ['metal.' + name for name in TESTS]
    if any(name not in tests for name in names):
        raise RuntimeError('All selected Metal tests must be built and registered')
    output = build / 'surface-shader-validation'
    output.mkdir(exist_ok=True)
    # Apple documents abort-on-fault because validation can otherwise replace
    # invalid accesses with zero without failing a command buffer.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('MTL_SHADER_VALIDATION')}
    env.update(MTL_SHADER_VALIDATION='1', MTL_SHADER_VALIDATION_DEFAULT_STATE='all',
               MTL_SHADER_VALIDATION_GLOBAL_MEMORY='1',
               MTL_SHADER_VALIDATION_THREADGROUP_MEMORY='1',
               MTL_SHADER_VALIDATION_TEXTURE_USAGE='1',
               MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING='1',
               MTL_SHADER_VALIDATION_REPORT_TO_STDERR='1',
               MTL_SHADER_VALIDATION_ABORT_ON_FAULT='1')
    records = []
    results = output / 'results.json'
    results.write_text('[]\n')
    for name in names:
        test = tests[name]
        properties = {item['name']: item['value'] for item in test['properties']}
        start = time.monotonic()
        record = {'test': name, 'command': test['command'], 'status': 'running'}
        records.append(record)
        results.write_text(json.dumps(records, indent=2) + '\n')
        try:
            with (output / (name + '.log')).open('w') as log:
                result = subprocess.run(test['command'], cwd=properties.get('WORKING_DIRECTORY', build),
                                        env=env, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=args.timeout, check=False)
        except (subprocess.TimeoutExpired, OSError) as error:
            record.update(seconds=time.monotonic() - start,
                          status='timeout' if isinstance(error, subprocess.TimeoutExpired) else 'error',
                          error=str(error))
            results.write_text(json.dumps(records, indent=2) + '\n')
            raise
        record.update(seconds=time.monotonic() - start, returncode=result.returncode,
                      status='passed' if result.returncode == 0 else 'failed')
        results.write_text(json.dumps(records, indent=2) + '\n')
        if result.returncode != 0:
            raise RuntimeError(f'{name}: exit {result.returncode}; see {output / (name + ".log")}')
        print(f'PASS {name}: {records[-1]["seconds"]:.2f}s', flush=True)


if __name__ == '__main__':
    main()
