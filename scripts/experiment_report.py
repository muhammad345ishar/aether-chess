"""Durable experiment artifacts and provenance shared by the command-line tools."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def output_path(root: Path, kind: str, requested: Path | None) -> Path:
    if requested is not None:
        return requested.resolve()
    directory = root / "results"
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H%M%SZ")
    # mkdtemp reserves the directory atomically, including simultaneous runs.
    return Path(tempfile.mkdtemp(prefix=f"{stamp}-{kind}-", dir=directory)) / "report.json"


def fingerprint(path: Path) -> dict:
    result = {"path": str(path.resolve()), "sha256": None}
    try:
        result["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        result["metadata_error"] = str(exc)
    return result


def engine_metadata(path: Path) -> dict:
    result = fingerprint(path)
    cache = path.parent / "CMakeCache.txt"
    result["build"] = None
    if cache.is_file():
        settings = {}
        result["build"] = {"cache": fingerprint(cache), "settings": settings,
                           "note": "Adjacent build configuration; binary identity is its SHA-256."}
        try:
            lines = cache.read_text(errors="replace").splitlines()
        except OSError as exc:
            result["build"]["metadata_error"] = str(exc)
            lines = []
        for line in lines:
            if ":" not in line or "=" not in line or line.startswith(("//", "#")):
                continue
            name, value = line.split("=", 1)
            name = name.split(":", 1)[0]
            if name.startswith(("CMAKE_CXX_FLAGS", "AETHER_")) or name in {
                "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_GENERATOR", "CMAKE_SYSTEM_NAME",
            }:
                settings[name] = value
    manifest = path.parent / "manifest.json"
    if manifest.is_file():
        result["manifest"] = fingerprint(manifest)
        try:
            data = json.loads(manifest.read_text())
            matches = bool(result["sha256"]) and data.get("binary_sha256") == result["sha256"]
            result["manifest"].update({"binary_hash_matches": matches, "data": data})
            if not matches:
                result["manifest"]["warning"] = "Manifest binary hash does not match; provenance is unverified."
        except (OSError, ValueError, AttributeError) as exc:
            result["manifest"]["metadata_error"] = str(exc)
    return result


def run_metadata(root: Path, fixtures: tuple[Path, ...] = ()) -> dict:
    def git(*args: str) -> str | None:
        try:
            return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.DEVNULL,
                                           timeout=5).decode().strip()
        except (OSError, subprocess.SubprocessError):
            return None

    revision = git("rev-parse", "HEAD")
    diff = git("diff", "HEAD", "--", "src", "scripts", "tests", "CMakeLists.txt", ".github")
    status = git("status", "--porcelain", "--untracked-files=normal", "--", "src", "scripts", "tests",
                 "CMakeLists.txt", ".github")
    return {
        "started_at": utc_now(), "status": "running", "failures": [],
        "environment": {"platform": platform.platform(), "machine": platform.machine(),
                        "python": platform.python_version(), "command": sys.argv},
        "source": {"revision": revision, "dirty": bool(status) if status is not None else None,
                   "diff_sha256": hashlib.sha256(diff.encode()).hexdigest() if diff else None,
                   "note": "Checkout at run start; does not assert that every binary was built from this revision."},
        "fixtures": [fingerprint(path) for path in fixtures],
    }


def record_failure(report: dict, stage: str, error: BaseException, **context: object) -> None:
    report["failures"].append({"stage": stage, "error_type": type(error).__name__, "message": str(error),
                               **context})
    report["status"] = "failed"
    report["finished_at"] = utc_now()


def write_report(path: Path, report: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Readers see either the previous checkpoint or the complete next one.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", suffix=".tmp", delete=False) as file:
            temporary = Path(file.name)
            json.dump(report, file, indent=2)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
