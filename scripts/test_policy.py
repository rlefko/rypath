"""Exercise the hook policy against synthetic subjects and Git repositories."""

from io import BytesIO
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

from check_commit_subject import subject_error


HANDOFF_CHECK = Path(__file__).with_name("check_handoff.py").resolve()
COMMIT_SUBJECT_CHECK = Path(__file__).with_name("check_commit_subject.py").resolve()
FIXTURE_CONTENT = "synthetic fixture payload"


class CommitSubjectTests(unittest.TestCase):
    def test_accepts_emoji_and_one_subject(self):
        for subject in (
            "⚡ Accelerate Mesh Traversal",
            "👩🏾‍💻 Compile C++",
            "🏳️‍🌈 Update LLVM to 18.1.8",
            "🔧 Allow e.g. Clang",
            "🐛 Fix U.S. locale handling",
        ):
            with self.subTest(subject=subject):
                self.assertIsNone(subject_error(subject))

    def test_rejects_invalid_subjects(self):
        for subject in (
            "Update tests",
            "Update tests ✅",
            "⚡",
            "⚡ Build tests.",
            "⚡ Build tests! Run them",
            "⚡ Build tests. Run them",
        ):
            with self.subTest(subject=subject):
                self.assertIsNotNone(subject_error(subject))


class GitRepositoryTests(unittest.TestCase):
    def setUp(self):
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        self.repo = Path(temporary_directory.name)
        self.git("init", "-q")

    def run_git(self, *args, check=True):
        return subprocess.run(
            ["git", "-c", "user.name=Policy Fixture", "-c", "user.email=fixture@example.invalid",
             "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", *args],
            cwd=self.repo,
            env={**os.environ, "GIT_EDITOR": "true"},
            capture_output=True,
            text=True,
            check=check,
        )

    def git(self, *args):
        return self.run_git(*args).stdout.strip()

    def check_script(self, script, *args):
        return subprocess.run(
            [sys.executable, str(script), *args], cwd=self.repo, capture_output=True,
            text=True, check=False,
        )


class CommitSubjectHookTests(GitRepositoryTests):
    def setUp(self):
        super().setUp()
        self.git("commit", "-q", "--allow-empty", "-m", "🔧 Add base fixture")
        self.base = self.git("rev-parse", "HEAD")
        self.hooks = self.repo / ".git" / "hooks"
        hook = self.hooks / "commit-msg"
        command = shlex.join([sys.executable, str(COMMIT_SUBJECT_CHECK)])
        hook.write_text(f'#!/bin/sh\nexec {command} "$1"\n', encoding="utf-8")
        hook.chmod(0o755)

    def commit_with_hook(self, *args):
        return self.run_git(
            "-c", f"core.hooksPath={self.hooks}", "commit", "-q", "--allow-empty", *args,
            check=False,
        )

    def check_range(self):
        return self.check_script(COMMIT_SUBJECT_CHECK, "--range", self.base, "HEAD")

    def test_rejects_ending_punctuation_on_a_folded_subject(self):
        message = "🔧 Update the fixture\nwith a second line."
        result = self.commit_with_hook("-m", message)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Remove the ending punctuation", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.base)

        self.git("commit", "-q", "--allow-empty", "-m", message)
        self.assertEqual(self.git("log", "-1", "--format=%s"), "🔧 Update the fixture with a second line.")
        result = self.check_range()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Remove the ending punctuation", result.stderr)

    def test_accepts_a_folded_subject_followed_by_a_body(self):
        result = self.commit_with_hook(
            "-m", "🔧 Update the fixture\nwith a second line\n\nBody sentence. Another sentence!"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("log", "-1", "--format=%s"), "🔧 Update the fixture with a second line")
        result = self.check_range()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_honors_configured_comment_cleanup(self):
        self.git("config", "commit.cleanup", "strip")
        self.git("config", "core.commentChar", ";")
        result = self.commit_with_hook("-m", "; comment\n🔧 Update the fixture\n; comment.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("log", "-1", "--format=%s"), "🔧 Update the fixture")
        result = self.check_range()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_honors_scissors_for_an_edited_message_with_a_fixed_comment_marker(self):
        self.git("config", "commit.cleanup", "scissors")
        self.git("config", "core.commentChar", ";")
        message = "🔧 Update the fixture\n; ------------------------ >8 ------------------------\nCut sentence."
        result = self.commit_with_hook("--edit", "-m", message)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("log", "-1", "--format=%s"), "🔧 Update the fixture")
        self.assertEqual(self.check_range().returncode, 0)

    def test_does_not_guess_an_automatic_comment_marker(self):
        self.git("config", "commit.cleanup", "strip")
        self.git("config", "core.commentChar", "auto")
        message = "🔧 Update the fixture\n# A line Git preserves."
        result = self.commit_with_hook("--edit", "-m", message)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Remove the ending punctuation", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.base)

        self.git("commit", "-q", "--allow-empty", "--edit", "-m", message)
        self.assertEqual(self.git("log", "-1", "--format=%s"), "🔧 Update the fixture # A line Git preserves.")
        self.assertEqual(self.check_range().returncode, 1)


class HandoffGuardTests(GitRepositoryTests):
    def check(self, *args):
        return self.check_script(HANDOFF_CHECK, *args)

    def write_zip(self, name, member):
        with zipfile.ZipFile(self.repo / name, mode="w") as archive:
            archive.writestr(member, FIXTURE_CONTENT)

    def write_tar(self, name, mode, member):
        data = FIXTURE_CONTENT.encode()
        with tarfile.open(self.repo / name, mode=mode) as archive:
            info = tarfile.TarInfo(member)
            info.size = len(data)
            archive.addfile(info, BytesIO(data))

    def test_rejects_reserved_paths_in_an_unborn_repository(self):
        path = self.repo / ".rypath-local" / "notes.txt"
        path.parent.mkdir()
        path.write_text(FIXTURE_CONTENT, encoding="utf-8")
        (self.repo / "project-handoff.zip").write_bytes(b"fixture")
        self.git("add", "--all")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(".rypath-local/notes.txt", result.stderr)
        self.assertIn("project-handoff.zip", result.stderr)
        self.assertNotIn(FIXTURE_CONTENT, result.stderr)

    def test_checks_staged_zip_instead_of_working_tree(self):
        self.write_zip("fixture with spaces.zip", "bundle/IMPLEMENTATION-PROMPT.txt")
        self.git("add", "--all")
        self.write_zip("fixture with spaces.zip", "bundle/example.txt")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("fixture with spaces.zip", result.stderr)
        self.assertNotIn("IMPLEMENTATION-PROMPT.txt", result.stderr)

    def test_checks_compressed_tar_members(self):
        self.write_tar("assets.tar.gz", "w:gz", "export/.rypath-local/fixture.txt")
        self.git("add", "--all")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("assets.tar.gz", result.stderr)

    def test_accepts_ordinary_archives(self):
        self.write_zip("fixture.ZIP", "bundle/example.txt")
        for suffix, mode in (
            (".tar", "w:"),
            (".tar.gz", "w:gz"), (".tgz", "w:gz"),
            (".tar.bz2", "w:bz2"), (".tbz", "w:bz2"), (".tbz2", "w:bz2"),
            (".tar.xz", "w:xz"), (".txz", "w:xz"),
        ):
            self.write_tar(f"fixture{suffix}", mode, "bundle/example.txt")
        self.git("add", "--all")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_truncated_zip_in_index_tree_and_range(self):
        self.git("commit", "-q", "--allow-empty", "-m", "Base fixture")
        base = self.git("rev-parse", "HEAD")
        name = "truncated.zip"
        self.write_zip(name, "bundle/implementation-prompt.txt")
        path = self.repo / name
        data = path.read_bytes()
        path.write_bytes(data[: data.index(b"PK\x01\x02")])
        self.git("add", "--all")
        staged = self.check("--staged")
        self.git("commit", "-qm", "Add fixture")

        for result in (staged, self.check("--tree", "HEAD"), self.check("--range", base, "HEAD")):
            with self.subTest(args=result.args):
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stderr, f"Could not inspect archive: {name!r}\n")

    def test_keeps_scanning_after_truncated_gzip(self):
        self.write_tar("a-truncated.tar.gz", "w:gz", "bundle/example.txt")
        path = self.repo / "a-truncated.tar.gz"
        path.write_bytes(path.read_bytes()[:10])
        self.write_zip("z-assets.zip", "bundle/private-session-setup.txt")
        self.git("add", "--all")

        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(
            result.stderr,
            "Could not inspect archive: 'a-truncated.tar.gz'\n"
            "Archive contains a reserved local handoff path: 'z-assets.zip'\n",
        )

    def test_rejects_unsupported_suffixes(self):
        suffixes = (".7z", ".rar", ".tar.zst", ".tzst")
        for suffix in suffixes:
            self.write_zip(f"fixture{suffix}", "bundle/example.txt")
        self.git("add", "--all")

        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(
            result.stderr.splitlines(),
            [f"Unsupported archive format: 'fixture{suffix}'" for suffix in suffixes],
        )

    def test_uses_the_decoder_selected_by_suffix(self):
        self.write_zip("fixture.tar", "bundle/example.txt")
        self.git("add", "--all")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stderr, "Could not inspect archive: 'fixture.tar'\n")

    def test_checks_added_then_deleted_paths_in_pull_request_history(self):
        self.git("commit", "-q", "--allow-empty", "-m", "Base fixture")
        base = self.git("rev-parse", "HEAD")
        path = self.repo / "private-session-setup.txt"
        path.write_text(FIXTURE_CONTENT, encoding="utf-8")
        self.git("add", "--all")
        self.git("commit", "-qm", "Add fixture")
        self.git("rm", "-q", path.name)
        self.assertEqual(self.check("--staged").returncode, 0)
        self.git("commit", "-qm", "Delete fixture")

        self.assertEqual(self.check("--tree", "HEAD").returncode, 0)
        result = self.check("--range", base, "HEAD")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(path.name, result.stderr)


if __name__ == "__main__":
    unittest.main()
