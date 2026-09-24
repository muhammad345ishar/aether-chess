#!/usr/bin/env python3
"""Rebuild clean, committed engine source and preserve a portable source baseline."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import tarfile
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", help="new baseline directory name, for example 0.3")
    parser.add_argument("--output-dir", type=Path, default=root / "baselines")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.label):
        parser.error("label must contain only letters, numbers, dots, underscores, or hyphens")
    destination = args.output_dir.resolve() / args.label
    if destination.exists():
        parser.error(f"baseline already exists: {destination}; choose a new label")
    source = root / "build-baselines" / args.label / "source"
    build = root / "build-baselines" / args.label / "build"
    if source.exists() or build.exists():
        parser.error(f"build-baselines/{args.label} already exists; choose a new label")

    def git(*arguments: str) -> str:
        return subprocess.check_output(["git", *arguments], cwd=root, text=True).strip()

    # Archive/build HEAD rather than the live working tree. Refuse uncommitted
    # engine edits so a snapshot can never quietly omit the intended changes.
    if git("status", "--porcelain", "--", "src", "CMakeLists.txt"):
        parser.error("commit engine source and CMakeLists.txt before preserving a baseline")
    revision = git("rev-parse", "HEAD")
    destination.mkdir(parents=True)
    source_archive = destination / "source.tar.gz"
    subprocess.run(["git", "archive", "--format=tar.gz", "--output", str(source_archive), revision],
                   cwd=root, check=True)
    # The archive is the source of truth even if files change in the working
    # tree while CMake is running. Build in a durable, ignored build directory.
    source.mkdir(parents=True)
    with tarfile.open(source_archive) as archive:
        archive.extractall(source, filter="data")
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"], check=True)
    subprocess.run(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure"], check=True)
    executable = build / ("aether.exe" if platform.system() == "Windows" else "aether")
    if not executable.exists():
        executable = build / "Release" / executable.name
    binary = destination / executable.name
    shutil.copy2(executable, binary)
    cache = (build / "CMakeCache.txt").read_text()
    settings = {}
    for line in cache.splitlines():
        if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "AETHER_")):
            key, value = line.split("=", 1)
            settings[key] = value
    manifest = {
        "version": args.label,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "source_revision": revision,
        "source_archive_sha256": hashlib.sha256(source_archive.read_bytes()).hexdigest(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "compiler_version": subprocess.check_output(
            [settings["CMAKE_CXX_COMPILER:FILEPATH"], "--version"], text=True).strip()
            if platform.system() != "Windows" else "See cmake_settings for compiler path.",
        "cmake_settings": settings,
        "validation": "Rebuilt archived source and passed CTest before preserving executable.",
    }
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Preserved {revision[:12]} at {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
