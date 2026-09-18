#!/usr/bin/env python3
"""Prepare a makepkg input directory using only files committed to HEAD."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""),
                        help="GitHub owner/repository, used for package metadata")
    parser.add_argument("--tag", help="Release tag; must be vX.Y.Z and match CMake")
    args = parser.parse_args()

    # Read version and template from the same commit as the archive, never local config.
    cmake = git("show", "HEAD:CMakeLists.txt").decode()
    match = re.search(r"\bproject\s*\(\s*trans\s+VERSION\s+(\d+\.\d+\.\d+)\s", cmake)
    if not match:
        parser.error("CMakeLists.txt must declare project(trans VERSION X.Y.Z ...)")
    version = match.group(1)
    if args.tag is not None and args.tag != f"v{version}":
        parser.error(f"release tag must be v{version} to match CMake, got {args.tag!r}")
    if args.repository and not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repository, re.ASCII):
        parser.error("repository must be a GitHub owner/repository")

    template = git("show", "HEAD:packaging/PKGBUILD").decode()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"trans-{version}.tar.gz"
    with archive.open("wb") as stream:
        subprocess.run(["git", "archive", "--format=tar.gz", f"--prefix=trans-{version}/", "HEAD"],
                       stdout=stream, check=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    url = f"https://github.com/{args.repository}" if args.repository else ""
    recipe = (template.replace("@VERSION@", version)
              .replace("@PROJECT_URL@", shlex.quote(url))
              .replace("@SOURCE_SHA256@", digest))
    (output / "PKGBUILD").write_text(recipe)
    print(f"Prepared trans {version} from {git('rev-parse', 'HEAD').decode().strip()}")
    print(f"Source SHA-256: {digest}")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as stream:
            stream.write(f"version={version}\n")


if __name__ == "__main__":
    main()
