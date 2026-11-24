"""Run Apple Clang's static analyzer with the adapter's actual build flags."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path)
    parser.add_argument("source", type=Path)
    options = parser.parse_args()
    source = options.source.resolve()

    try:
        entries = json.loads(options.database.read_text(encoding="utf-8"))
        if not isinstance(entries, list):
            raise ValueError("compile database must be an array")
        matches = [
            entry for entry in entries
            if Path(entry["directory"], entry["file"]).resolve() == source
        ]
        if not matches:
            raise ValueError(f"no compile commands found for {source}")

        commands = []
        for entry in matches:
            if "arguments" in entry:
                command = entry["arguments"]
                if not isinstance(command, list) or not all(isinstance(arg, str) for arg in command):
                    raise ValueError("compile arguments must be an array of strings")
                command = command.copy()
            else:
                if not isinstance(entry["command"], str):
                    raise ValueError("compile command must be a string")
                command = shlex.split(entry["command"])
            if not command or not command[0]:
                raise ValueError("compile command must name a compiler")
            output = command.index("-o")
            if output + 1 == len(command) or command[output + 1].startswith("-"):
                raise ValueError("compile command has no output path after -o")
            del command[output : output + 2]
            command.remove("-c")
            command.extend(["--analyze", "--analyzer-output", "text", "-Xanalyzer", "-analyzer-werror"])
            commands.append((entry["directory"], command))

        # A source may belong to multiple targets with different defines, include
        # paths, or language options. Analyze each environment, including those
        # following a failed analysis, and preserve the first failure status.
        result = 0
        for directory, command in commands:
            print(shlex.join(command), flush=True)
            try:
                status = subprocess.run(command, cwd=directory, check=False).returncode
            except OSError as error:
                print(f"Metal static analysis launch failed in {directory}: {error}", file=sys.stderr)
                status = 1
            if status and not result:
                result = status
        return result
    except (OSError, KeyError, TypeError, ValueError) as error:
        print(f"Metal static analysis setup failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
