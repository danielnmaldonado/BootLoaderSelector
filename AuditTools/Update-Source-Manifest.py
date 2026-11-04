#!/usr/bin/env python3
"""Check or deliberately refresh source hashes after reviewing changes.

Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
SPDX-License-Identifier: BSD-3-Clause
"""

import argparse
import hashlib
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = "AuditReports/MANIFEST.sha256"
GENERATED_DIRS = {
    ".git", "Build", "Conf", "Release", "BaseTools/Source/C/bin",
    "BaseTools/Source/C/libs",
}
GENERATED_FILES = {
    MANIFEST, "BuildLogs/BootLoaderSelector-Build.log",
    "BaseTools/Source/C/VfrCompile/Pccts/antlr/antlr",
    "BaseTools/Source/C/VfrCompile/Pccts/dlg/dlg",
}


def source_files():
    files = []
    for directory, names, filenames in os.walk(ROOT, followlinks=False):
        parent = Path(directory)
        for name in names[:]:
            path = parent / name
            relative = path.relative_to(ROOT).as_posix()
            if relative in GENERATED_DIRS:
                names.remove(name)
                continue
            if name == "__pycache__" or relative.startswith(".Bls"):
                raise ValueError(f"Remove or inspect generated state first: {relative}")
            if relative == "toolchain.gcc102.snb":
                raise ValueError("Keep binary toolchains outside the source distribution")
            if path.is_symlink():
                raise ValueError(f"Source symlink is not permitted: {relative}")
        for name in filenames:
            path = parent / name
            relative = path.relative_to(ROOT).as_posix()
            if relative in GENERATED_FILES or relative in GENERATED_DIRS:
                continue
            if name == ".DS_Store" and not path.is_symlink():
                continue
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"Source is not a regular file: {relative}")
            if any(character.isspace() for character in relative):
                raise ValueError(f"Manifest paths must not contain whitespace: {relative}")
            if name.endswith((".pyc", ".pyo")):
                raise ValueError(f"Remove generated Python bytecode first: {relative}")
            if relative.startswith("BaseTools/Source/C/") and name.endswith((".o", ".d")):
                continue
            files.append(relative)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="refresh after reviewing intended changes")
    arguments = parser.parse_args()
    try:
        paths = source_files()
        entries = []
        for relative in paths:
            digest = hashlib.sha256()
            with (ROOT / relative).open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            entries.append(f"{digest.hexdigest()}  ./{relative}\n")
        content = "".join(entries).encode("utf-8")
        destination = ROOT / MANIFEST
        if arguments.write:
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
                temporary = Path(stream.name)
                try:
                    stream.write(content)
                    stream.flush()
                    os.fsync(stream.fileno())
                except BaseException:
                    temporary.unlink(missing_ok=True)
                    raise
            try:
                temporary.chmod(0o644)
                temporary.replace(destination)
            finally:
                temporary.unlink(missing_ok=True)
            print(f"Source manifest updated: {len(paths)} files")
        elif destination.read_bytes() != content:
            parser.exit(1, "Source manifest differs; review changes before running --write.\n")
        else:
            print(f"Source manifest: PASS ({len(paths)} files)")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Source manifest error: {error}\n")


if __name__ == "__main__":
    main()
