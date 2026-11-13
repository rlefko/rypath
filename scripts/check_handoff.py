"""Check tracked paths and supported archive member names for local handoff material."""

import argparse
import os
import subprocess
import sys
import tarfile
import tempfile
import zipfile


LOCAL_DIRECTORY = ".rypath-local"
LOCAL_FILENAMES = {"private-session-setup.txt", "implementation-prompt.txt"}
TAR_MODES = {
    ".tar": "r:",
    ".tar.gz": "r:gz",
    ".tgz": "r:gz",
    ".tar.bz2": "r:bz2",
    ".tbz": "r:bz2",
    ".tbz2": "r:bz2",
    ".tar.xz": "r:xz",
    ".txz": "r:xz",
}
UNSUPPORTED_SUFFIXES = (".tar.zst", ".tzst", ".7z", ".rar")
ARCHIVE_SUFFIXES = (".zip", *TAR_MODES, *UNSUPPORTED_SUFFIXES)
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


def archive_contains_handoff(object_id, suffix):
    with tempfile.TemporaryFile() as blob:
        subprocess.run(
            ["git", "cat-file", "blob", object_id.decode()],
            stdout=blob,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        blob.seek(0)
        if suffix == ".zip":
            with zipfile.ZipFile(blob) as archive:
                return any(reserved_path(name) for name in archive.namelist())
        with tarfile.open(fileobj=blob, mode=TAR_MODES[suffix]) as archive:
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
            continue
        if mode not in FILE_MODES:
            continue
        lower_path = path.casefold()
        suffix = next((item for item in ARCHIVE_SUFFIXES if lower_path.endswith(item)), None)
        if suffix is None:
            continue
        if suffix in UNSUPPORTED_SUFFIXES:
            print(f"Unsupported archive format: {path!r}", file=sys.stderr)
            valid = False
            continue
        try:
            contains_handoff = archive_contains_handoff(object_id, suffix)
        except Exception:
            # Archive readers can propagate decoder errors as well as format-specific errors.
            # Their messages and tracebacks can disclose private member names.
            print(f"Could not inspect archive: {path!r}", file=sys.stderr)
            valid = False
            continue
        if contains_handoff:
            print(f"Archive contains a reserved local handoff path: {path!r}", file=sys.stderr)
            valid = False
    return 0 if valid else 1


if __name__ == "__main__":
    sys.exit(main())
