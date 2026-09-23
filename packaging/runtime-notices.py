#!/usr/bin/env python3
"""Bundle license notices with provenance for the selected, locked Slint runtime graph."""
import argparse
import functools
import hashlib
import html
import json
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path, PurePosixPath

RUNTIME_FEATURES = ("std", "backend-winit", "renderer-femtovg", "accessibility", "system-tray")
LICENSE_TEXT_NAME = re.compile(r"^(?:licen[cs]e|unlicense|copying)(?:$|[-_.])", re.I)
LICENSE_NAME = re.compile(r"^(?:licen[cs]e|unlicense|copying|copyright|notice|authors)(?:$|[-_.])", re.I)
SPDX_REVISION = "31ba1a50e5397e00a304dbadc76531740e89ee48"  # SPDX license-list-data v3.29.0


def run(arguments):
    return subprocess.run(arguments, check=True, text=True, encoding="utf-8", stdout=subprocess.PIPE).stdout


def copy_notice(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


@functools.lru_cache(maxsize=128)
def upstream_bytes(url, limit=4 * 1024 * 1024):
    request = urllib.request.Request(url, headers={"User-Agent": "Trans-runtime-notices"})
    with urllib.request.urlopen(request, timeout=60) as response:
        contents = response.read(limit + 1)
    if not contents or len(contents) > limit:
        raise RuntimeError("Invalid upstream notice download: " + url)
    return contents


def download_notice(url, destination):
    contents = upstream_bytes(url)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(contents)


def notice_files(directory):
    # Crates can have embedded third-party data with its own copyright. Preserve
    # nested notices too, not just a single license identifier from Cargo.toml.
    return sorted(path for path in directory.rglob("*")
                  if path.is_file() and LICENSE_NAME.match(path.name)
                  and not any(part in {".git", "target", "node_modules"} for part in path.relative_to(directory).parts)
                  and path.suffix.lower() not in {".rs", ".h", ".cpp", ".py", ".sh"})


def collect_embedded_notices(directory, destination):
    """Preserve complete original MIT grants embedded in source-file headers."""
    found = []
    for source in sorted(directory.rglob("*")):
        if not source.is_file() or source.suffix.lower() not in {".rs", ".c", ".h", ".cpp"}:
            continue
        with source.open("rb") as stream:
            prefix = stream.read(32 * 1024)
        header = re.match(rb"\s*(/\*.*?\*/)", prefix, re.S)
        if not header:
            continue
        notice = header.group(1)
        if not all(term in notice.lower() for term in (
                b"copyright", b"permission is hereby granted", b"the software is provided",
                b"in no event", b"dealings in the software")):
            continue
        relative = source.relative_to(directory)
        target = destination / "source-notices" / (relative.as_posix() + ".txt")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(notice + b"\n")
        found.append(relative.as_posix())
    if found:
        (destination / "SOURCE-NOTICES.txt").write_text(
            "Original license comment headers copied verbatim from the published crate:\n"
            + "\n".join(found) + "\n", encoding="utf-8")
    return bool(found)


def spdx_terms(expression):
    """Validate SPDX expression grammar without choosing or changing its terms."""
    tokens = re.findall(r"[A-Za-z0-9][A-Za-z0-9.+-]*|[()]|\S", expression)
    position = 0
    terms = set()

    def identifier(exception=False):
        nonlocal position
        if position == len(tokens):
            raise ValueError("Missing SPDX identifier")
        token = tokens[position]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+-]*", token) or token in {"AND", "OR", "WITH"} \
                or token.startswith(("LicenseRef-", "DocumentRef-")):
            raise ValueError("Nonstandard or invalid SPDX identifier")
        terms.add((token, exception))
        position += 1

    def primary():
        nonlocal position
        if position < len(tokens) and tokens[position] == "(":
            position += 1
            expression_terms()
            if position == len(tokens) or tokens[position] != ")":
                raise ValueError("Unbalanced SPDX expression")
            position += 1
        else:
            identifier()
            if position < len(tokens) and tokens[position] == "WITH":
                position += 1
                identifier(exception=True)

    def expression_terms():
        nonlocal position
        primary()
        while position < len(tokens) and tokens[position] in {"AND", "OR"}:
            position += 1
            primary()

    expression_terms()
    if position != len(tokens):
        raise ValueError("Invalid SPDX expression")
    return terms


def collect_declared_spdx(package, directory, destination):
    """Preserve upstream's explicit license-by-reference, never invent attribution."""
    expression = package.get("license") or ""
    try:
        terms = spdx_terms(expression)
    except ValueError as error:
        raise RuntimeError("Missing or nonstandard licensing for " + package["name"]) from error
    destination.mkdir(parents=True, exist_ok=True)
    originals = [path for path in directory.iterdir() if path.is_file()
                 and (path.name in {"Cargo.toml", "Cargo.toml.orig", ".cargo_vcs_info.json"}
                      or path.name.lower().startswith("readme"))]
    for path in originals:
        copy_notice(path, destination / "declaration" / path.name)
    provenance = [
        "Metadata-only licensing provenance: the published package explicitly declares SPDX terms.",
        "No original standalone license text was recovered from the archive or a verifiable published repository revision.",
        "The separate spdx/ files are canonical SPDX reference texts, NOT original upstream LICENSE files.",
        "Canonical copyright placeholders are unchanged; no copyright holder or year has been invented.",
        "Original manifests, available VCS metadata and README are preserved under declaration/.",
        "Declared expression: " + expression,
        "Authors (upstream metadata, not an inferred copyright statement): " + "; ".join(package.get("authors", [])),
        "Repository: " + (package.get("repository") or package.get("homepage") or ""),
        "Package source: " + (package.get("source") or ""),
        "SPDX license-list-data revision: " + SPDX_REVISION,
        "",
    ]
    for identifier, exception in sorted(terms):
        category = "exceptions" if exception else "details"
        source = f"https://raw.githubusercontent.com/spdx/license-list-data/{SPDX_REVISION}/json/{category}/" \
            + urllib.parse.quote(identifier, safe="") + ".json"
        record = json.loads(upstream_bytes(source))
        id_field, text_field = ("licenseExceptionId", "licenseExceptionText") if exception else ("licenseId", "licenseText")
        if record.get(id_field) != identifier or not isinstance(record.get(text_field), str) or not record[text_field].strip():
            raise RuntimeError("Unrecognized canonical SPDX license: " + identifier)
        target = destination / "spdx" / (identifier + ".txt")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(record[text_field], encoding="utf-8")
        provenance.extend([source, "SHA256: " + hashlib.sha256(target.read_bytes()).hexdigest(), ""])
    (destination / "DECLARED-SPDX.txt").write_text("\n".join(provenance), encoding="utf-8")
    return True


def collect_upstream_notices(package, directory, destination):
    """Recover omitted workspace notices at the crate's published git revision."""
    vcs_path = directory / ".cargo_vcs_info.json"
    if not vcs_path.is_file():
        return False
    vcs = json.loads(vcs_path.read_text(encoding="utf-8"))
    revision = vcs.get("git", {}).get("sha1", "")
    if not re.fullmatch(r"[0-9a-f]{40}", revision) or vcs.get("git", {}).get("dirty", False):
        raise RuntimeError("Invalid or dirty published source revision for " + package["name"])
    repository = package.get("repository") or package.get("homepage") or ""
    if repository.startswith("git+"):
        repository = repository[4:]
    url = urllib.parse.urlsplit(repository)
    components = url.path.strip("/").split("/")
    if url.scheme != "https" or url.hostname != "github.com" or len(components) < 2:
        return False
    if len(components) > 2 and components[2] not in {"tree", "blob"}:
        raise RuntimeError("Unrecognized repository homepage for " + package["name"])
    owner, name = components[0], components[1].removesuffix(".git")
    if not all(re.fullmatch(r"[A-Za-z0-9_.-]+", part) for part in (owner, name)):
        raise RuntimeError("Invalid upstream repository identity for " + package["name"])
    relative = vcs.get("path_in_vcs", "")
    if not isinstance(relative, str):
        raise RuntimeError("Invalid published repository path for " + package["name"])
    crate_path = PurePosixPath(relative)
    if crate_path.is_absolute() or ".." in crate_path.parts or "\\" in relative:
        raise RuntimeError("Unsafe published repository path for " + package["name"])
    # Older Cargo publications omit path_in_vcs. Root notices still cover the
    # repository; when a matching package directory exists, include its notices.
    api = f"https://api.github.com/repos/{owner}/{name}/git/trees/{revision}?recursive=1"
    tree = json.loads(upstream_bytes(api, 16 * 1024 * 1024))
    if tree.get("truncated") or not isinstance(tree.get("tree"), list):
        raise RuntimeError("Incomplete immutable repository tree for " + package["name"])
    if "path_in_vcs" not in vcs:
        manifests = [PurePosixPath(item["path"]).parent for item in tree["tree"]
                     if item.get("type") == "blob" and PurePosixPath(item["path"]).name == "Cargo.toml"
                     and PurePosixPath(item["path"]).parent.name.replace("-", "_") == package["name"].replace("-", "_")]
        if len(manifests) > 1:
            raise RuntimeError("Ambiguous published crate directory for " + package["name"])
        if manifests:
            crate_path = manifests[0]
    ancestors = {crate_path, *crate_path.parents, PurePosixPath(".")}
    chosen = []
    for item in tree["tree"]:
        if item.get("type") != "blob" or item.get("mode") not in {"100644", "100755"}:
            continue
        path = PurePosixPath(item["path"])
        if path.is_absolute() or ".." in path.parts or "\\" in item["path"]:
            raise RuntimeError("Unsafe upstream notice path")
        named_notice = bool(LICENSE_NAME.match(path.name))
        license_directory = any(
            path.is_relative_to(ancestor / spelling)
            for ancestor in ancestors for spelling in ("LICENSES", "licenses", "LICENSE", "license")
        )
        in_crate = crate_path != PurePosixPath(".") and path.is_relative_to(crate_path)
        if license_directory or (named_notice and (path.parent in ancestors or in_crate)):
            if path.suffix.lower() not in {".rs", ".h", ".cpp", ".py", ".sh"}:
                chosen.append((path, item["sha"]))
    if not any(LICENSE_TEXT_NAME.match(path.name) or any(part.lower() == "licenses" for part in path.parts)
               for path, _ in chosen):
        return False
    provenance = [
        "Original notices omitted from the published crate archive.",
        f"Repository: https://github.com/{owner}/{name}",
        "Revision from .cargo_vcs_info.json: " + revision,
        "Crate path: " + str(crate_path),
        "Files retain their repository-relative paths under upstream/.",
        "",
    ]
    for path, blob in sorted(chosen):
        source = f"https://raw.githubusercontent.com/{owner}/{name}/{revision}/" + urllib.parse.quote(str(path), safe="/")
        contents = upstream_bytes(source)
        actual_blob = hashlib.sha1(b"blob " + str(len(contents)).encode("ascii") + b"\0" + contents).hexdigest()
        if actual_blob != blob:
            raise RuntimeError("Upstream notice does not match its immutable git blob: " + source)
        target = destination / "upstream" / Path(*path.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(contents)
        provenance.extend([source, "Git blob: " + blob, "SHA256: " + hashlib.sha256(contents).hexdigest(), ""])
    (destination / "UPSTREAM-NOTICES.txt").write_text("\n".join(provenance), encoding="utf-8")
    return True


def bundle(args):
    slint = Path(args.slint_source).resolve(strict=True)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    cargo = ["cargo"] + (["+" + args.rust_toolchain] if args.rust_toolchain else [])
    rustc = ["rustc"] + (["+" + args.rust_toolchain] if args.rust_toolchain else [])
    manifest = slint / "api/cpp/Cargo.toml"
    feature_args = ["--no-default-features", "--features", ",".join("slint-cpp/" + feature for feature in RUNTIME_FEATURES)]
    common = ["--manifest-path", str(manifest), "--locked"]
    # Tree's package selection reflects the runtime build, unlike taking every
    # node of a workspace-wide cargo metadata response (which includes tooling).
    tree = run(cargo + ["tree"] + common + feature_args + ["--package", "slint-cpp", "--target", args.target,
               "--edges", "normal,build", "--prefix", "none", "--format", "{p}"])
    selected = set()
    for line in tree.splitlines():
        match = re.match(r"^(\S+) v([^\s]+)(?:\s|$)", line)
        if not match:
            if line.strip():
                raise RuntimeError("Unrecognized Cargo dependency record: " + line)
            continue
        name, version = match.groups()
        if name == "i-slint-backend-qt" or name.startswith(("qmetaobject", "qttypes")):
            raise RuntimeError("A forbidden GUI backend is present in the selected runtime graph: " + name)
        selected.add((name, version))
    if ("slint-cpp", "1.18.1") not in selected:
        raise RuntimeError("Expected the pinned Slint 1.18.1 runtime in the dependency graph")
    # A cross-target tree also contains host build dependencies. Filtering metadata
    # to the target alone can omit those; the tree above remains the selection filter.
    metadata = json.loads(run(cargo + ["metadata"] + common + feature_args + ["--format-version", "1"]))
    packages = {}
    for package in metadata["packages"]:
        key = (package["name"], package["version"])
        if key in selected:
            if key in packages and packages[key]["id"] != package["id"]:
                raise RuntimeError("Ambiguous dependency source for " + repr(key))
            packages[key] = package
    if selected - packages.keys():
        raise RuntimeError("Cargo metadata omitted selected dependencies: " + repr(selected - packages.keys()))

    slint_notices = output / "slint"
    copy_notice(slint / "LICENSE.md", slint_notices / "LICENSE.md")
    for path in (slint / "LICENSES").iterdir():
        if path.is_file():
            copy_notice(path, slint_notices / "LICENSES" / path.name)
    for name in ("REUSE.toml", "Cargo.lock"):
        source = slint / name
        if source.is_file():
            copy_notice(source, slint_notices / name)
    records = [
        "Slint runtime and transitive dependency notices",
        "Target: " + args.target,
        "Features: " + ", ".join(RUNTIME_FEATURES),
        "Source: https://github.com/slint-ui/slint/tree/v1.18.1",
        "The inventory includes normal/build dependencies (including proc macros); not all listed packages are standalone runtime libraries.",
        "SPDX expressions below preserve upstream licensing alternatives; they do not relicense Trans.",
        "Slint licensing choices and attribution requirements are in slint/LICENSE.md and slint/LICENSES/.",
        "",
    ]
    for key, package in sorted(packages.items()):
        name, version = key
        directory = Path(package["manifest_path"]).parent.resolve(strict=True)
        license_expression = package.get("license")
        license_file = package.get("license_file")
        if not license_expression and not license_file:
            raise RuntimeError("No upstream licensing information for " + name + " " + version)
        destination = output / "rust-crates" / (name + "-" + version)
        files = notice_files(directory)
        if license_file:
            explicit = (directory / license_file).resolve(strict=True)
            if explicit not in files:
                files.append(explicit)
        upstream = False
        if not license_file and not any(LICENSE_TEXT_NAME.match(path.name) for path in files) and not directory.is_relative_to(slint):
            upstream = collect_embedded_notices(directory, destination) \
                or collect_upstream_notices(package, directory, destination) \
                or collect_declared_spdx(package, directory, destination)
        for source in files:
            relative = source.relative_to(directory) if source.is_relative_to(directory) else Path(source.name)
            copy_notice(source, destination / relative)
        records.extend([
            name + " " + version,
            "  License: " + (license_expression or "See upstream license file"),
            "  Authors: " + "; ".join(package.get("authors", [])),
            "  Repository: " + (package.get("repository") or package.get("homepage") or ""),
            "  Source: " + (package.get("source") or "Slint v1.18.1 source tree"),
            "  Texts: " + ("rust-crates/" + name + "-" + version if files or upstream else "slint/LICENSE.md and slint/LICENSES/"),
            "  Notice provenance: " + (
                "Explicit upstream SPDX declaration; canonical reference texts are separate, not original LICENSE files."
                if (destination / "DECLARED-SPDX.txt").is_file() else "Original upstream notice text."),
            "",
        ])
    # The Rust standard library is linked into the runtime but is not a Cargo
    # package. Obtain notices from this exact installed toolchain, falling back
    # only to its immutable compiler source revision, never an unpinned branch.
    rust_version = run(rustc + ["--version", "--verbose"])
    sysroot = Path(run(rustc + ["--print", "sysroot"]).strip())
    commit = re.search(r"^commit-hash: ([0-9a-f]{40})$", rust_version, re.M)
    rust_notices = output / "rust-standard-library"
    rust_notices.mkdir(parents=True, exist_ok=True)
    notice_directories = [sysroot / "share/doc/rust", sysroot / "share/doc/rust/html",
                          sysroot / "share/licenses/rust", sysroot]
    copyright_files = [path for directory in notice_directories
                       for path in directory.glob("COPYRIGHT-library*") if path.is_file()]
    if not copyright_files:
        copyright_files = [path for directory in notice_directories
                           for path in directory.glob("COPYRIGHT.html*") if path.is_file()]
    if not copyright_files:
        raise RuntimeError("The installed Rust toolchain is missing its generated standard-library copyright notice")
    generated = copyright_files[0]
    copy_notice(generated, rust_notices / "COPYRIGHT-library.html")
    # In-tree Unicode tables and platform code carry licenses beyond the two
    # Rust-wide choices. Preserve the generated copyright list plus their texts.
    expressions = re.findall(r"<b>License:</b>\s*([^<]+)", generated.read_text(encoding="utf-8"))
    identifiers = set()
    for expression in expressions:
        identifiers.update(re.findall(r"[A-Za-z0-9][A-Za-z0-9.+-]*", html.unescape(expression)))
    identifiers.difference_update({"AND", "OR", "WITH"})
    for identifier in sorted(identifiers):
        source = next((directory / "LICENSES" / (identifier + ".txt") for directory in notice_directories
                       if (directory / "LICENSES" / (identifier + ".txt")).is_file()), None)
        destination = rust_notices / "LICENSES" / (identifier + ".txt")
        if source:
            copy_notice(source, destination)
        else:
            if not commit:
                raise RuntimeError("Cannot determine the source revision for Rust license " + identifier)
            download_notice("https://raw.githubusercontent.com/rust-lang/rust/" + commit.group(1)
                            + "/LICENSES/" + identifier + ".txt", destination)
    for name in ("LICENSE-APACHE", "LICENSE-MIT", "COPYRIGHT"):
        candidates = [sysroot / "share/doc/rust" / name, sysroot / name]
        source = next((path for path in candidates if path.is_file()), None)
        if source:
            copy_notice(source, rust_notices / name)
        else:
            if not commit:
                raise RuntimeError("Cannot locate Rust standard-library notices or determine the compiler revision")
            url = "https://raw.githubusercontent.com/rust-lang/rust/" + commit.group(1) + "/" + name
            download_notice(url, rust_notices / name)
    (rust_notices / "toolchain.txt").write_text(rust_version, encoding="utf-8")
    (output / "THIRD-PARTY-NOTICES.txt").write_text("\n".join(records), encoding="utf-8")
    print("Bundled license notices with provenance for", len(packages), "selected Cargo packages and the Rust standard library")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slint-source", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--rust-toolchain", default="")
    args = parser.parse_args()
    try:
        bundle(args)
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError, KeyError) as error:
        print("Cannot produce complete runtime notices:", error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
