# SPDX-License-Identifier: GPL-3.0-only
"""Argument failures must precede scene/device acquisition and output writes."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    result = subprocess.run([executable, "--help"], capture_output=True, text=True)
    assert result.returncode == 0 and "replay" in result.stdout, result
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        output = root / "must-not-exist"
        base = [
            str(executable),
            "--scene", str(root / "missing.pbrt"),
            "--artifact-root", str(root),
            "--output-dir", str(output),
        ]
        cases = [
            (["--width", "0"], "dimensions"),
            (["--height", "16385"], "dimensions"),
            (["--frames", "-1"], "integer"),
            (["--frames", "1.5"], "integer"),
            (["--frames", "4", "--frames", "8"], "Duplicate"),
            (["--capture-mode", "other"], "Capture mode"),
            (["--capture-mode", "replay", "--frames", "17"], "sixteen"),
            (["--replay-spatial", "6"], "spatial"),
            (["--capture-frames", "128"], "outside"),
            (["--reference-samples", "1024"], "seed"),
            (["--reference-samples", "2"], "Reference samples"),
            (["--motion-offset", "nan"], "finite"),
            (["--unknown", "1"], "Unknown"),
            (["--seed"], "missing"),
        ]
        for arguments, diagnostic in cases:
            result = subprocess.run(base + arguments, capture_output=True, text=True)
            assert result.returncode == 1, (arguments, result)
            assert diagnostic.lower() in result.stderr.lower(), (arguments, result.stderr)
            assert not output.exists(), arguments
    print(f"Diagnostic CLI: help and {len(cases)} pre-acquisition failures passed")


if __name__ == "__main__":
    main()
