#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CPU-only tests for compile-database selection and analyzer invocation."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "analyze_metal", Path(__file__).resolve().parent / "analyze_metal.py"
)
analyze_metal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyze_metal)


class AnalyzeMetalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "adapter.mm"
        self.database = self.root / "compile_commands.json"

    def entry(self, define="FIRST", arguments=True, directory=None):
        command = ["clang++", f"-D{define}", "-x", "objective-c++", "-o", f"{define}.o",
                   "-c", str(self.source)]
        return {"directory": str(directory or self.root), "file": str(self.source),
                "arguments" if arguments else "command": command if arguments else
                analyze_metal.shlex.join(command)}

    def invoke(self, entries, statuses=(0,)):
        self.database.write_text(json.dumps(entries))
        output, error = io.StringIO(), io.StringIO()
        results = [status if isinstance(status, OSError) else subprocess.CompletedProcess([], status)
                   for status in statuses]
        with patch.object(sys, "argv", ["analyze_metal.py", str(self.database), str(self.source)]), \
                patch.object(analyze_metal.subprocess, "run", side_effect=results) as run, \
                redirect_stdout(output), redirect_stderr(error):
            status = analyze_metal.main()
        return status, run.call_args_list, output.getvalue(), error.getvalue()

    def test_single_relative_source(self):
        entry = self.entry()
        entry["file"] = "adapter.mm"
        status, calls, _, error = self.invoke([entry])
        self.assertEqual(status, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(error, "")

    def test_every_target_environment_is_analyzed(self):
        second_directory = self.root / "second target"
        entries = [self.entry(), self.entry("SECOND", arguments=False, directory=second_directory)]
        unrelated = self.entry("UNRELATED")
        unrelated["file"] = "other.mm"
        status, calls, output, error = self.invoke([entries[0], unrelated, entries[1]], (0, 0))
        self.assertEqual(status, 0)
        self.assertEqual(len(calls), 2)
        self.assertEqual(error, "")
        self.assertEqual(len(output.splitlines()), 2)
        for call, entry, define in zip(calls, entries, ("FIRST", "SECOND")):
            command = call.args[0]
            self.assertIn(f"-D{define}", command)
            self.assertIn(str(self.source), command)
            self.assertNotIn("-o", command)
            self.assertNotIn("-c", command)
            self.assertNotIn(f"{define}.o", command)
            self.assertEqual(command[-5:], ["--analyze", "--analyzer-output", "text",
                                           "-Xanalyzer", "-analyzer-werror"])
            self.assertEqual(call.kwargs, {"cwd": entry["directory"], "check": False})

    def test_failure_status_is_retained_without_skipping_later_targets(self):
        for statuses in ((7, 0), (0, 9), (7, 9)):
            with self.subTest(statuses=statuses):
                status, calls, _, _ = self.invoke([self.entry(), self.entry("SECOND")], statuses)
                self.assertEqual(status, next(value for value in statuses if value))
                self.assertEqual(len(calls), 2)

    def test_no_matching_command_is_an_error(self):
        for entries in ([], [{"directory": str(self.root), "file": "other.mm"}]):
            with self.subTest(entries=entries):
                status, calls, _, error = self.invoke(entries)
                self.assertEqual(status, 1)
                self.assertEqual(calls, [])
                self.assertIn("no compile commands", error)

    def test_launch_failure_preserves_first_failure_and_continues(self):
        entries = [self.entry(), self.entry("SECOND"), self.entry("THIRD")]
        for statuses, expected in (((7, OSError("missing compiler"), 0), 7),
                                   ((OSError("missing compiler"), 9, 0), 1)):
            with self.subTest(statuses=statuses):
                status, calls, _, error = self.invoke(entries, statuses)
                self.assertEqual(status, expected)
                self.assertEqual(len(calls), 3)
                self.assertIn("Metal static analysis launch failed in", error)
                self.assertIn("missing compiler", error)

    def test_malformed_database_or_commands_fail_before_analysis(self):
        malformed = [None, {}, [None], [{"file": str(self.source)}]]
        for command in ([], ["clang++", "-c", str(self.source)], ["clang++", "-o"],
                        ["clang++", "-o", "-c", str(self.source)],
                        ["clang++", "-o", "x.o", str(self.source)], "clang++", [None]):
            malformed.append([self.entry(), {**self.entry(), "arguments": command}])
        malformed.extend([[{**self.entry(arguments=False), "command": value}]
                          for value in (None, 'clang++ "unterminated')])
        for entries in malformed:
            with self.subTest(entries=entries):
                status, calls, _, error = self.invoke(entries)
                self.assertEqual(status, 1)
                self.assertEqual(calls, [])
                self.assertIn("Metal static analysis setup failed:", error)

    def test_unreadable_or_invalid_json_is_an_error(self):
        for content in (None, "{"):
            if content is not None:
                self.database.write_text(content)
            with patch.object(sys, "argv", ["analyze_metal.py", str(self.database), str(self.source)]), \
                    patch.object(analyze_metal.subprocess, "run") as run, \
                    redirect_stderr(io.StringIO()) as error:
                self.assertEqual(analyze_metal.main(), 1)
            run.assert_not_called()
            self.assertIn("Metal static analysis setup failed:", error.getvalue())


if __name__ == "__main__":
    unittest.main()
