"""Exercise the hook policy against synthetic subjects and Git repositories."""

from io import BytesIO
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

from check_commit_subject import subject_error


HANDOFF_CHECK = Path(__file__).with_name("check_handoff.py").resolve()
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


class HandoffGuardTests(unittest.TestCase):
    def setUp(self):
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        self.repo = Path(temporary_directory.name)
        self.git("init", "-q")

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Policy Fixture", "-c", "user.email=fixture@example.invalid",
             "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", *args],
            cwd=self.repo,
            text=True,
        ).strip()

    def check(self, *args):
        return subprocess.run(
            [sys.executable, str(HANDOFF_CHECK), *args], cwd=self.repo, capture_output=True,
            text=True, check=False,
        )

    def write_zip(self, name, member):
        with zipfile.ZipFile(self.repo / name, mode="w") as archive:
            archive.writestr(member, FIXTURE_CONTENT)

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
        data = FIXTURE_CONTENT.encode()
        with tarfile.open(self.repo / "assets.tar.gz", mode="w:gz") as archive:
            member = tarfile.TarInfo("export/.rypath-local/fixture.txt")
            member.size = len(data)
            archive.addfile(member, BytesIO(data))
        self.git("add", "--all")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("assets.tar.gz", result.stderr)

    def test_accepts_ordinary_archives(self):
        self.write_zip("fixture.zip", "bundle/example.txt")
        self.git("add", "--all")
        result = self.check("--staged")
        self.assertEqual(result.returncode, 0, result.stderr)

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
