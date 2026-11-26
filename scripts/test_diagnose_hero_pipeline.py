# SPDX-License-Identifier: GPL-3.0-only
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

from diagnose_hero_pipeline import capture, retain


class DiagnosticCaptureTests(unittest.TestCase):
    def test_failure_exit_and_output_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "failure.log"
            result = capture([sys.executable, "-c", "print('pipeline failed'); raise SystemExit(7)"], path)
            self.assertEqual(result["exit_code"], 7)
            self.assertFalse(result["timed_out"])
            self.assertEqual(path.read_bytes(), b"pipeline failed\n")

    def test_output_is_explicitly_truncated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "large.log"
            result = capture([sys.executable, "-c", "print('x' * 128, end='')"], path, limit=31)
            self.assertEqual(result["exit_code"], 0)
            self.assertEqual(result["output_bytes"], 128)
            self.assertTrue(result["truncated"])
            self.assertEqual(path.read_bytes(), b"x" * 31)

    def test_timeout_and_launch_failure_are_not_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = capture([sys.executable, "-c", "import time; time.sleep(10)"],
                             root / "timeout.log", timeout=.1)
            self.assertTrue(result["timed_out"])
            self.assertIsNone(result["exit_code"])
            missing = capture([str(root / "absent")], root / "missing.log")
            self.assertIn("launch_error", missing)
            self.assertIsNone(missing["exit_code"])

    def test_retained_bytes_and_missing_artifact_are_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "probe", root / "retained" / "probe"
            source.write_bytes(b"exact compiled artifact\0\xff")
            record = retain(source, destination)
            self.assertEqual(record["status"], "retained")
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            self.assertEqual(record["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(retain(root / "absent", root / "unused")["status"], "missing")


if __name__ == "__main__":
    unittest.main()
