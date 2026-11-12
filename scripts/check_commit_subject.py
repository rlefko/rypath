"""Validate a commit message file or a set of Git commit subjects."""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import unicodedata

import emoji


SENTENCE_END = re.compile(r"[.!?…。！？]+[\"'”’)\]]*(?=\s+\S)")
DOTTED_INITIALISM = re.compile(r"(?:[A-Za-z]\.)+[A-Za-z]")
ABBREVIATIONS = {"vs", "etc"}


def subject_error(subject):
    subject = subject.rstrip()
    matches = emoji.emoji_list(subject)
    if not matches or matches[0]["match_start"] != 0:
        return "Start the commit subject with an emoji"
    if not subject[matches[0]["match_end"] :].strip():
        return "Add a subject after the emoji"
    if unicodedata.category(subject[-1]).startswith("P"):
        return "Remove the ending punctuation from the commit subject"
    for boundary in SENTENCE_END.finditer(subject):
        preceding_word = subject[: boundary.start()].rsplit(maxsplit=1)[-1]
        if boundary.group() == "." and (
            preceding_word.casefold() in ABBREVIATIONS or DOTTED_INITIALISM.fullmatch(preceding_word)
        ):
            continue
        return "Use at most one sentence in the commit subject"
    return None


def check_range(base, head):
    commits = subprocess.check_output(
        ["git", "log", "--format=%h%x00%s", f"{base}..{head}", "--"], text=True
    )
    valid = True
    for commit in commits.split("\n")[:-1]:
        revision, subject = commit.split("\0", maxsplit=1)
        error = subject_error(subject)
        if error:
            print(f"{revision}: {error}", file=sys.stderr)
            valid = False
    return 0 if valid else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("message", nargs="?", type=Path)
    parser.add_argument("--range", nargs=2, metavar=("BASE", "HEAD"))
    options = parser.parse_args()
    if (options.message is None) == (options.range is None):
        parser.error("provide a commit message file or --range BASE HEAD")
    if options.range is not None:
        return check_range(*options.range)

    with options.message.open(encoding="utf-8") as message:
        error = subject_error(message.readline())
    if error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
