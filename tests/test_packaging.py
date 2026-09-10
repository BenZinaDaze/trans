"""Exercise release version checks and source isolation without an Arch container."""

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]


class PackagingTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.repo = self.root / "repository"
        self.repo.mkdir()
        (self.repo / "packaging").mkdir()
        (self.repo / "CMakeLists.txt").write_text(
            "project(tran VERSION 0.1.0 LANGUAGES CXX)\n")
        (self.repo / "packaging/PKGBUILD").write_text(
            (PROJECT / "packaging/PKGBUILD").read_text())
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Packaging test", "-c", "user.email=test@localhost",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "Fixture")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True, capture_output=True)

    def prepare(self, *args):
        env = os.environ.copy()
        env.pop("GITHUB_OUTPUT", None)
        return subprocess.run(
            [sys.executable, str(PROJECT / "packaging/prepare.py"),
             "--output-dir", str(self.root / "output"), "--repository", "example/tran", *args],
            cwd=self.repo, env=env, capture_output=True, text=True)

    def test_archive_and_metadata_use_committed_files(self):
        # Local edits, user config and build products must not affect a release archive.
        (self.repo / "CMakeLists.txt").write_text("project(tran VERSION 9.9.9 LANGUAGES CXX)")
        (self.repo / "settings.ini").write_text("apiKey=local-only-fixture")
        (self.repo / "packaging/PKGBUILD").write_text("uncommitted template")
        (self.repo / "build").mkdir()
        (self.repo / "build/tran").write_text("old executable")
        result = self.prepare("--tag", "v0.1.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.root / "output"
        archive = output / "tran-0.1.0.tar.gz"
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        recipe = (output / "PKGBUILD").read_text()
        self.assertIn("pkgver=0.1.0", recipe)
        self.assertIn("url=https://github.com/example/tran", recipe)
        self.assertIn(digest, recipe)
        self.assertNotIn("@VERSION@", recipe)
        with tarfile.open(archive) as source:
            self.assertEqual(set(source.getnames()), {
                "tran-0.1.0", "tran-0.1.0/CMakeLists.txt", "tran-0.1.0/packaging",
                "tran-0.1.0/packaging/PKGBUILD"})
            self.assertIn(b"VERSION 0.1.0", source.extractfile("tran-0.1.0/CMakeLists.txt").read())

    def test_branch_build_needs_no_tag(self):
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "output/tran-0.1.0.tar.gz").is_file())

    def test_invalid_or_mismatched_tags_fail_before_packaging(self):
        for tag in ["v0.2.0", "0.1.0", "v0.1.0-rc1", "v0.1.0\n", "vlatest"]:
            with self.subTest(tag=tag):
                result = self.prepare("--tag", tag)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("release tag must be v0.1.0", result.stderr)
                self.assertFalse((self.root / "output").exists())


if __name__ == "__main__":
    unittest.main()
