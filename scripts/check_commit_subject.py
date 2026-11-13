"""Validate a commit message file or a set of Git commit subjects."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import unicodedata

import emoji


SENTENCE_END = re.compile(r"[.!?…。！？]+[\"'”’)\]]*(?=\s+\S)")
DOTTED_INITIALISM = re.compile(r"(?:[A-Za-z]\.)+[A-Za-z]")
ABBREVIATIONS = {"vs", "etc"}
SCISSORS_LINE = b"------------------------ >8 ------------------------\n"


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
        ["git", "log", "-z", "--encoding=UTF-8", "--format=%h%x00%s", f"{base}..{head}", "--"]
    )
    valid = True
    fields = commits.decode("utf-8").split("\0")[:-1]
    for revision, subject in zip(fields[::2], fields[1::2], strict=True):
        error = subject_error(subject)
        if error:
            print(f"{revision}: {error}", file=sys.stderr)
            valid = False
    return 0 if valid else 1


def cleanup_message(message):
    # Git defers cleanup of an edited message until after the commit-msg hook.
    config = subprocess.run(
        ["git", "config", "--null", "--get-regexp", r"^(commit\.cleanup|core\.comment(char|string))$"],
        stdout=subprocess.PIPE,
        check=False,
    )
    if config.returncode != 1:
        config.check_returncode()
    settings = {}
    for entry in config.stdout.split(b"\0")[:-1]:
        key, _, value = entry.partition(b"\n")
        settings[key] = value
    cleanup = settings.get(b"commit.cleanup", b"default")
    if cleanup == b"verbatim":
        return message

    # Git also allows ":" as an actual editor. The hook cannot distinguish that from no edit.
    edited = os.environ.get("GIT_EDITOR") != ":"
    # Git selects an automatic comment marker before editing and the hook does not receive it.
    automatic_comments = b"auto" in (settings.get(b"core.commentchar"), settings.get(b"core.commentstring"))
    command = ["git", "stripspace"]
    if not automatic_comments:
        if cleanup == b"strip" or (cleanup == b"default" and edited):
            command.append("--strip-comments")
        elif cleanup == b"scissors" and edited:
            marker = subprocess.check_output(["git", "stripspace", "--comment-lines"], input=SCISSORS_LINE)
            before_marker, _, _ = (b"\n" + message).partition(b"\n" + marker)
            message = before_marker[1:]
    return subprocess.check_output(command, input=message)


def message_subject(message):
    # Git's %s skips blank lines, then joins the first paragraph with spaces.
    lines = (line.rstrip(b" \t\r") for line in cleanup_message(message).split(b"\n"))
    paragraph = []
    for line in lines:
        if line:
            paragraph.append(line)
        elif paragraph:
            break
    return b" ".join(paragraph).decode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("message", nargs="?", type=Path)
    parser.add_argument("--range", nargs=2, metavar=("BASE", "HEAD"))
    options = parser.parse_args()
    if (options.message is None) == (options.range is None):
        parser.error("provide a commit message file or --range BASE HEAD")
    if options.range is not None:
        return check_range(*options.range)

    error = subject_error(message_subject(options.message.read_bytes()))
    if error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
