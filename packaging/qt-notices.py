#!/usr/bin/env python3
"""Collect Qt module license/attribution files matching the deployed Qt release."""
import argparse
from pathlib import Path, PurePosixPath
import re
import tarfile
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("Expected a Qt X.Y.Z version")
    args.output.mkdir(parents=True, exist_ok=True)
    links = []
    for module in ("qtbase", "qtdeclarative", "qtsvg"):
        url = f"https://codeload.github.com/qt/{module}/tar.gz/refs/tags/v{args.version}"
        links.append(f"{module}: {url}")
        count = 0
        with urllib.request.urlopen(url, timeout=120) as response, \
                tarfile.open(fileobj=response, mode="r|gz") as archive:
            for entry in archive:
                if not entry.isfile():
                    continue
                path = PurePosixPath(entry.name)
                if ".." in path.parts or path.is_absolute():
                    raise ValueError("Unsafe source archive member")
                relative = PurePosixPath(*path.parts[1:])
                name = relative.name.lower()
                if ("LICENSES" not in relative.parts and name != "qt_attribution.json"
                        and not any(token in name for token in ("license", "licence", "copying", "copyright", "notice"))):
                    continue
                target = args.output / module / str(relative)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(entry).read())
                count += 1
        if not count:
            raise RuntimeError(f"No licenses collected for {module}")
    (args.output / "Qt-NOTICE.txt").write_text(
        f"This package dynamically links Qt {args.version}. Qt is copyright The Qt Company Ltd. and other contributors.\n"
        "Qt libraries remain replaceable; the application does not prohibit debugging modifications to these libraries.\n"
        "License texts and third-party notices are included in the module directories. Corresponding source:\n"
        + "\n".join(links) + "\nQt build instructions: https://doc.qt.io/qt-6.8/windows-building.html\n",
        encoding="utf-8")


if __name__ == "__main__":
    main()
