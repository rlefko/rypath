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
        matches = [
            entry for entry in entries
            if Path(entry["directory"], entry["file"]).resolve() == source
        ]
        if len(matches) != 1:
            raise ValueError(f"expected one compile command for {source}, found {len(matches)}")

        entry = matches[0]
        command = list(entry["arguments"]) if "arguments" in entry else shlex.split(entry["command"])
        output = command.index("-o")
        del command[output : output + 2]
        command.remove("-c")
        command.extend(["--analyze", "--analyzer-output", "text", "-Xanalyzer", "-analyzer-werror"])
        print(shlex.join(command), flush=True)
        return subprocess.run(command, cwd=entry["directory"], check=False).returncode
    except (OSError, KeyError, TypeError, ValueError) as error:
        print(f"Metal static analysis setup failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
