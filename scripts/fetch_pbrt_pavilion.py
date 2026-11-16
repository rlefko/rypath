#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage the pinned Pavilion closure without cloning the full scene repository.

By default, inspect local caches and print a plan. --fetch stages verified local
copies and downloads only missing blobs. Large scene assets remain outside Git.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import urllib.request


MANIFEST = Path(__file__).resolve().parent.parent / "tests/corpus/pbrt-pavilion.json"


def git_blob_sha1(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def verified(path: Path, record: dict) -> bool:
    if not path.is_file() or path.is_symlink() or path.stat().st_size != record["bytes"]:
        return False
    data = path.read_bytes()
    return git_blob_sha1(data) == record["git_blob_sha1"] and (
        "sha256" not in record or hashlib.sha256(data).hexdigest() == record["sha256"]
    )


def safe_path(root: Path, name: str) -> Path:
    relative = PurePosixPath(name)
    if relative.is_absolute() or not relative.parts or any(p in ("..", "") for p in relative.parts):
        raise ValueError(f"Unsafe manifest path: {name}")
    target = root.joinpath(*relative.parts)
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Cache path escapes root: {name}")
    return target


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text())
    revision = manifest["revision"]
    if len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision):
        raise ValueError("Manifest requires a full Git revision")
    if manifest["repository"] != "mmp/pbrt-v4-scenes":
        raise ValueError("Unexpected scene repository")
    seen = set()
    for record in manifest["files"]:
        name = record["path"]
        safe_path(Path("."), name)
        if name in seen:
            raise ValueError(f"Duplicate manifest path: {name}")
        seen.add(name)
        if not isinstance(record["bytes"], int) or not 0 <= record["bytes"] <= 128 * 1024**2:
            raise ValueError(f"Invalid asset size: {name}")
        digest = record["git_blob_sha1"]
        if len(digest) != 40 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError(f"Invalid Git blob hash: {name}")
    for entry in manifest["entry_points"]:
        if entry not in seen:
            raise ValueError(f"Entry point missing from closure: {entry}")
    return manifest


def stage(args: argparse.Namespace) -> dict:
    manifest = load_manifest(args.manifest)
    root = args.cache.resolve() / manifest["revision"]
    plan = []
    download_bytes = 0
    copy_bytes = 0
    for record in manifest["files"]:
        target = safe_path(root, record["path"])
        if verified(target, record):
            action, source = "verified-cache", target
        else:
            source = None
            for local_root in args.local_root:
                candidate = safe_path(local_root.resolve(), record.get("reuse_path", record["path"]))
                if verified(candidate, record):
                    source = candidate
                    break
            if source is None and record.get("reuse_basename"):
                for candidate in args.local_file:
                    if candidate.name == record["reuse_basename"] and verified(candidate, record):
                        source = candidate
                        break
            action = "copy" if source else "download"
            if source:
                copy_bytes += record["bytes"]
            else:
                download_bytes += record["bytes"]
        plan.append((record, target, action, source))
    summary = {
        "revision": manifest["revision"],
        "cache": str(root),
        "files": len(plan),
        "verified_cache_files": sum(p[2] == "verified-cache" for p in plan),
        "copy_bytes": copy_bytes,
        "download_bytes": download_bytes,
        "entry_points": [str(safe_path(root, p)) for p in manifest["entry_points"]],
    }
    if not args.fetch:
        summary["pending"] = [{"path": r["path"], "action": a, "bytes": r["bytes"]}
                              for r, _, a, _ in plan if a != "verified-cache"]
        return summary
    if download_bytes > args.max_download_bytes:
        raise ValueError(f"Download plan needs {download_bytes} bytes; limit is {args.max_download_bytes}")
    root.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(root).free < copy_bytes + download_bytes + 1024**3:
        raise ValueError("Insufficient disk space for verified closure plus 1 GiB reserve")
    for record, target, action, source in plan:
        if action == "verified-cache":
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        # Every asset is an immutable snapshot; avoid linking mutable source files.
        if action == "copy":
            data = source.read_bytes()
        else:
            url = (f"https://raw.githubusercontent.com/{manifest['repository']}/"
                   f"{manifest['revision']}/{record['path']}")
            request = urllib.request.Request(url, headers={"User-Agent": "rypath-pinned-corpus"})
            with urllib.request.urlopen(request, timeout=45) as response:
                data = response.read(record["bytes"] + 1)
        if len(data) != record["bytes"] or git_blob_sha1(data) != record["git_blob_sha1"]:
            raise ValueError(f"Pinned blob verification failed: {record['path']}")
        if "sha256" in record and hashlib.sha256(data).hexdigest() != record["sha256"]:
            raise ValueError(f"SHA-256 verification failed: {record['path']}")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".pavilion-", delete=False) as out:
                temporary = Path(out.name)
                out.write(data)
            os.replace(temporary, target)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    receipts = []
    for record, target, _, _ in plan:
        if not verified(target, record):
            raise ValueError(f"Final cache verification failed: {record['path']}")
        receipts.append({"path": record["path"], "bytes": record["bytes"],
                         "git_blob_sha1": record["git_blob_sha1"],
                         "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
    receipt = {"revision": manifest["revision"], "files": receipts}
    (root / "rypath-acquisition.json").write_text(json.dumps(receipt, indent=2) + "\n")
    summary["verified_files"] = len(receipts)
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--cache", type=Path, default=Path(".cache/pbrt-pavilion"))
    parser.add_argument("--local-root", type=Path, action="append", default=[])
    parser.add_argument("--local-file", type=Path, action="append", default=[])
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--max-download-bytes", type=int, default=8 * 1024**2)
    args = parser.parse_args()
    if args.max_download_bytes < 0:
        parser.error("--max-download-bytes must be nonnegative")
    try:
        print(json.dumps(stage(args), indent=2))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Pavilion acquisition failed: {error}\n")


if __name__ == "__main__":
    main()
