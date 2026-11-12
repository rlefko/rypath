"""Reject reserved local handoff paths and ZIP/TAR archives containing them."""

import argparse
import os
import subprocess
import sys
import tarfile
import tempfile
import zipfile


LOCAL_DIRECTORY = ".rypath-local"
LOCAL_FILENAMES = {"private-session-setup.txt", "implementation-prompt.txt"}
ARCHIVE_SUFFIXES = (
    ".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz", ".tbz2",
    ".tar.xz", ".txz", ".tar.zst", ".tzst", ".7z", ".rar",
)
FILE_MODES = {b"100644", b"100755"}


def reserved_path(path):
    parts = path.replace("\\", "/").rstrip("/").casefold().split("/")
    name = parts[-1]
    return (
        LOCAL_DIRECTORY in parts
        or name in LOCAL_FILENAMES
        or ("handoff" in name and name.endswith(ARCHIVE_SUFFIXES))
    )


def git_output(*args):
    return subprocess.check_output(["git", *args])


def staged_files():
    records = git_output(
        "diff", "--cached", "--raw", "--no-abbrev", "--no-renames",
        "--diff-filter=AMT", "-z", "--",
    ).split(b"\0")[:-1]
    for metadata, path in zip(records[::2], records[1::2], strict=True):
        _, mode, _, object_id, _ = metadata.split()
        yield os.fsdecode(path), mode, object_id


def tree_files(tree):
    records = git_output("ls-tree", "--full-tree", "-rz", tree).split(b"\0")[:-1]
    for record in records:
        metadata, path = record.split(b"\t", maxsplit=1)
        mode, _, object_id = metadata.split()
        yield os.fsdecode(path), mode, object_id


def range_files(base, head):
    trees = git_output("log", "--format=%T", f"{base}..{head}", "--").splitlines()
    for tree in dict.fromkeys(trees):
        yield from tree_files(tree.decode())


def archive_contains_handoff(object_id):
    with tempfile.TemporaryFile() as blob:
        subprocess.run(["git", "cat-file", "blob", object_id.decode()], stdout=blob, check=True)
        blob.seek(0)
        if zipfile.is_zipfile(blob):
            with zipfile.ZipFile(blob) as archive:
                return any(reserved_path(name) for name in archive.namelist())

        blob.seek(0)
        try:
            archive = tarfile.open(fileobj=blob, mode="r:*")
        except tarfile.ReadError:
            return False
        with archive:
            return any(reserved_path(item.name) or reserved_path(item.linkname) for item in archive)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--staged", action="store_true", help="check changes in the Git index")
    source.add_argument("--tree", help="check tracked files at a Git revision")
    source.add_argument("--range", nargs=2, metavar=("BASE", "HEAD"), help="check introduced trees")
    options = parser.parse_args()

    if options.staged:
        files = staged_files()
    elif options.tree is not None:
        files = tree_files(options.tree)
    else:
        files = range_files(*options.range)

    seen = set()
    valid = True
    for entry in files:
        if entry in seen:
            continue
        seen.add(entry)
        path, mode, object_id = entry
        if reserved_path(path):
            print(f"Reserved local handoff path: {path!r}", file=sys.stderr)
            valid = False
        elif mode in FILE_MODES and path.casefold().endswith(ARCHIVE_SUFFIXES):
            try:
                contains_handoff = archive_contains_handoff(object_id)
            except (OSError, tarfile.TarError, zipfile.BadZipFile):
                print(f"Could not inspect archive: {path!r}", file=sys.stderr)
                valid = False
                continue
            if contains_handoff:
                print(f"Archive contains a reserved local handoff path: {path!r}", file=sys.stderr)
                valid = False
    return 0 if valid else 1


if __name__ == "__main__":
    sys.exit(main())
