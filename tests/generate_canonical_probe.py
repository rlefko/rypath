#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate a test TU retaining production MIS code without unrelated guide kernels."""

import argparse
import hashlib
import json
from pathlib import Path

GUIDES = b'#include "reconstruction_guides.metal"\n'
RENDER = b'#include "render.metal"\n'
OMITTED = b'// Test-only: unrelated reconstruction guide entrypoints omitted\n'
ISOLATED = b'#include "canonical_render.metal"\n'


def transform(render, probe):
    """Require the reviewed layout; alter exactly two include lines, no math."""
    if render.count(GUIDES) != 1 or probe.count(RENDER) != 1:
        raise ValueError("Expected exactly one reconstruction-guides and one render include")
    isolated_render = render.replace(GUIDES, OMITTED)
    isolated_probe = probe.replace(RENDER, ISOLATED)
    if isolated_render.replace(OMITTED, GUIDES) != render:
        raise ValueError("Ambiguous generated render omission marker")
    if isolated_probe.replace(ISOLATED, RENDER) != probe:
        raise ValueError("Ambiguous generated probe render include")
    return isolated_render, isolated_probe


def generate(render_path, probe_path, output):
    render, probe = render_path.read_bytes(), probe_path.read_bytes()
    isolated_render, isolated_probe = transform(render, probe)
    result = {"schema": 1,
              "scope": "Test-only isolation; production and whole-TU canonical_full_context unchanged",
              "source_files": {str(p.resolve()): hashlib.sha256(data).hexdigest()
                               for p, data in ((render_path, render), (probe_path, probe))},
              "generated_files": {name: hashlib.sha256(data).hexdigest() for name, data in (
                  ("canonical_render.metal", isolated_render), ("canonical_probe.metal", isolated_probe))},
              "omitted_include": GUIDES.decode().strip(),
              "numeric_or_capability_policy_changes": False}
    # Validate everything before creating any output. CMake owns this directory;
    # regeneration deliberately replaces derived files when source inputs change.
    output.mkdir(parents=True, exist_ok=True)
    (output / "canonical_render.metal").write_bytes(isolated_render)
    (output / "canonical_probe.metal").write_bytes(isolated_probe)
    (output / "generation.json").write_text(json.dumps(result, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--render", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    generate(args.render, args.probe, args.output_dir)


if __name__ == "__main__":
    main()
