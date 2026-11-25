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
    assert "independent|area0|area1|area2|area4" in result.stdout, result.stdout
    assert "--area-output selected|conditional-mean" in result.stdout, result.stdout
    assert "--spectral-transport auto|scalar|packet|hero" in result.stdout, result.stdout
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        output = root / "must-not-exist"
        base = [
            str(executable),
            "--scene", str(root / "missing.pbrt"),
            "--artifact-root", str(root),
            "--output-dir", str(output),
        ]
        area0 = subprocess.run(base + ["--estimator", "area0"], capture_output=True, text=True)
        assert area0.returncode != 0 and "Estimator must" not in area0.stderr
        assert not output.exists()
        cases = [
            (["--width", "0"], "dimensions"),
            (["--spectral-transport", "invalid"], "Spectral transport"),
            (["--hero-wavelengths", "invalid"], "Hero wavelengths"),
            (["--area-mutation-policy", "invalid"], "Area mutation policy"),
            (["--area-stencil", "invalid"], "Area stencil"),
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
            (["--metalfx-conditioning", "maybe"], "on or off"),
            (["--metalfx-guide-policy", "other"], "policy"),
            (["--metalfx-exposure", "0"], "exposure"),
            (["--metalfx-exposure", "65505"], "exposure"),
            (["--metalfx-exposure", "nan"], "finite"),
            (["--reference-samples", "1024", "--seed", "99", "--metalfx-conditioning", "on"], "fanout"),
            (["--replay-list", "list.json", "--metalfx-guide-policy", "primary"], "fanout"),
            (["--variance-policy", "other"], "variance policy"),
            (["--variance-prefilter", "on"], "requires"),
            (["--variance-policy", "conservative", "--variance-prefilter", "on"], "requires"),
            (["--strict-spatial-segments", "true"], "on or off"),
            (["--save-online-mean", "on", "--motion-offset", "0.1"], "static"),
            (["--area-output", "invalid"], "Area output"),
            (["--area-output", "selected"], "requires"),
            (["--area-output", "conditional-mean", "--estimator", "independent"], "requires"),
            (["--estimator", "area2", "--area-output", "selected", "--area-output", "conditional-mean"], "Duplicate"),
            (["--estimator", "area2", "--replay-list", "x.json", "--area-output", "selected"], "fanout"),
            (["--estimator", "area3"], "Estimator"),
            (["--estimator", "area8"], "Estimator"),
            (["--estimator", "area02"], "Estimator"),
            (["--estimator", "area-1"], "Estimator"),
            (["--estimator", "area1.5"], "Estimator"),
            (["--estimator", "area"], "Estimator"),
            (["--estimator", "independent1"], "Estimator"),
            (["--estimator", "area2", "--estimator", "area4"], "Duplicate"),
            (["--reference-samples", "1024", "--seed", "99", "--estimator", "area1"], "static independent"),
            (["--reference-samples", "1024", "--seed", "99", "--estimator", "area2"], "static independent"),
            (["--reference-samples", "1024", "--seed", "99", "--estimator", "area4"], "static independent"),
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
