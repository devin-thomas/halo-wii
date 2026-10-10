"""Atomic, validated publication of derived content generations.

Layout under an output root (all names produced by the converter):

    <root>/CURRENT                      text: generation id and manifest SHA-256
    <root>/generations/<id>/manifest.json
    <root>/generations/<id>/<relative output paths>
    <root>/.staging-<token>/             in-progress run (never trusted)

A run writes every output and its manifest into a fresh staging directory,
re-reads and validates all of it, then renames the directory to its
generation id (the first 16 hex digits of the manifest hash) and only then
replaces CURRENT through a temporary file and os.replace. An interrupted or
failed run leaves CURRENT and every earlier generation untouched; its staging
directory is discarded by the next run. Identical input and profile give the
same manifest bytes, hence the same generation id.

Relative paths are restricted so the same tree is valid on FAT32 media:
lower-case [a-z0-9._-] components, no traversal or reserved device names,
bounded length and depth, case-insensitively unique, and every file below
4 GiB.
"""
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shutil

MANIFEST = "manifest.json"
CURRENT = "CURRENT"
FAT32_MAX_FILE = (1 << 32) - 1
MAX_COMPONENT = 64
MAX_DEPTH = 6
MAX_OUTPUTS = 1 << 20
_COMPONENT = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
_RESERVED = {"con", "prn", "aux", "nul"} | {"com%d" % i for i in range(1, 10)} | {"lpt%d" % i for i in range(1, 10)}
_GENERATION = re.compile(r"^[0-9a-f]{16}$")


class PublishError(ValueError):
    """A bounded publication failure; the message holds no private path."""


def check_relative(path):
    """Validate one manifest-relative output path; returns it unchanged."""
    if not isinstance(path, str) or not path or "\\" in path or "\0" in path:
        raise PublishError("output path must be a non-empty POSIX relative path")
    pure = PurePosixPath(path)
    if pure.is_absolute() or ":" in path or str(pure) != path:
        raise PublishError("output path must be relative and normalized")
    parts = pure.parts
    if len(parts) > MAX_DEPTH:
        raise PublishError("output path too deep")
    for part in parts:
        if part in (".", "..") or not _COMPONENT.match(part) or len(part) > MAX_COMPONENT:
            raise PublishError("output path component not allowed")
        if part.split(".")[0] in _RESERVED:
            raise PublishError("output path uses a reserved device name")
    if parts[0] in (MANIFEST, CURRENT) or parts[0].startswith(".staging"):
        raise PublishError("output path collides with publication metadata")
    return path


def validate_manifest(manifest):
    """Structural checks shared by publication and later readers."""
    outputs = manifest.get("outputs")
    if not isinstance(outputs, list) or len(outputs) > MAX_OUTPUTS:
        raise PublishError("manifest outputs missing or above bound")
    seen = set()
    for entry in outputs:
        path = check_relative(entry.get("path"))
        folded = path.casefold()
        if folded in seen:
            raise PublishError("duplicate output path (case-insensitive)")
        seen.add(folded)
        size, digest = entry.get("bytes"), entry.get("sha256")
        if not isinstance(size, int) or not 0 <= size <= FAT32_MAX_FILE:
            raise PublishError("output size outside FAT32 bounds")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise PublishError("output hash malformed")
    if [e["path"] for e in outputs] != sorted(e["path"] for e in outputs):
        raise PublishError("manifest outputs not in canonical order")
    return manifest


def manifest_bytes(manifest):
    return (json.dumps(manifest, indent=1, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")


class FaultInjector:
    """Test/demonstration hook: fail like a full disk after `after_bytes` bytes,
    or stop at a named stage as an interruption would."""

    def __init__(self, after_bytes=None, stage=None):
        self.after_bytes = after_bytes
        self.stage = stage
        self.written = 0

    def account(self, size):
        self.written += size
        if self.after_bytes is not None and self.written > self.after_bytes:
            raise OSError(errno.ENOSPC, "simulated disk full")

    def at(self, stage):
        if self.stage == stage:
            raise InterruptedError("simulated interruption at " + stage)


class Generation:
    """One staging directory being filled; promote() publishes it."""

    def __init__(self, root, fault=None, disk_usage=shutil.disk_usage):
        self.root = Path(root)
        self.fault = fault or FaultInjector()
        self.disk_usage = disk_usage
        self.root.mkdir(parents=True, exist_ok=True)
        self.discarded = discard_stale_staging(self.root)
        self.staging = self.root / (".staging-" + secrets.token_hex(8))
        self.staging.mkdir()
        self.outputs = {}
        self._folded = set()

    def require_space(self, estimated_bytes, margin=16 << 20):
        free = self.disk_usage(self.root).free
        if free < estimated_bytes + margin:
            raise PublishError("insufficient destination space for the planned outputs")
        return free

    def write(self, relative, data):
        check_relative(relative)
        if relative.casefold() in self._folded:
            raise PublishError("duplicate output path (case-insensitive)")
        self._folded.add(relative.casefold())
        if len(data) > FAT32_MAX_FILE:
            raise PublishError("output exceeds the FAT32 file size limit")
        target = self.staging.joinpath(*PurePosixPath(relative).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, "xb") as stream:
            view = memoryview(data)
            for start in range(0, len(view), 1 << 20):
                chunk = view[start:start + (1 << 20)]
                self.fault.account(len(chunk))
                stream.write(chunk)
            stream.flush()
            os.fsync(stream.fileno())
        digest = hashlib.sha256(data).hexdigest()
        self.outputs[relative] = {"path": relative, "bytes": len(data), "sha256": digest}
        return self.outputs[relative]

    def finish(self, manifest, verifier=None):
        """Write the manifest, re-validate everything from disk, then promote."""
        self.fault.at("before-manifest")
        manifest = dict(manifest)
        manifest["outputs"] = sorted(manifest["outputs"], key=lambda e: e["path"])
        for entry in manifest["outputs"]:
            recorded = self.outputs.get(entry["path"])
            if recorded is None or recorded["sha256"] != entry["sha256"] or recorded["bytes"] != entry["bytes"]:
                raise PublishError("manifest entry disagrees with the written output")
        if len(manifest["outputs"]) != len(self.outputs):
            raise PublishError("written outputs missing from the manifest")
        validate_manifest(manifest)
        blob = manifest_bytes(manifest)
        self.fault.account(len(blob))
        with open(self.staging / MANIFEST, "xb") as stream:
            stream.write(blob)
            stream.flush()
            os.fsync(stream.fileno())
        self.fault.at("before-validate")
        verify_tree(self.staging, verifier)
        self.fault.at("before-promote")
        manifest_sha = hashlib.sha256(blob).hexdigest()
        generation = manifest_sha[:16]
        generations = self.root / "generations"
        generations.mkdir(exist_ok=True)
        target = generations / generation
        reused = False
        if target.exists():
            verify_tree(target, verifier)
            if hashlib.sha256((target / MANIFEST).read_bytes()).hexdigest() != manifest_sha:
                raise PublishError("existing generation id holds a different manifest")
            shutil.rmtree(self.staging)
            reused = True
        else:
            os.rename(self.staging, target)
        self.fault.at("before-current")
        write_current(self.root, generation, manifest_sha)
        return {"generation": generation, "manifest_sha256": manifest_sha, "reused_existing": reused,
                "discarded_stale_staging": self.discarded}

    def abandon(self):
        if self.staging.exists():
            shutil.rmtree(self.staging, ignore_errors=True)


def discard_stale_staging(root):
    count = 0
    for child in Path(root).iterdir():
        if child.is_dir() and child.name.startswith(".staging-"):
            shutil.rmtree(child, ignore_errors=True)
            count += 1
    return count


def verify_tree(directory, verifier=None):
    """Re-read a generation directory: manifest structure, exact file set, sizes and hashes."""
    directory = Path(directory)
    manifest = json.loads((directory / MANIFEST).read_bytes())
    validate_manifest(manifest)
    expected = {e["path"]: e for e in manifest["outputs"]}
    found = set()
    for path in directory.rglob("*"):
        if path.is_file() and path.name != MANIFEST:
            found.add(path.relative_to(directory).as_posix())
    if found != set(expected):
        raise PublishError("generation file set differs from its manifest")
    for relative, entry in expected.items():
        data = (directory / relative).read_bytes()
        if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise PublishError("generation output differs from its manifest")
        if verifier is not None:
            verifier(entry, data)
    return manifest


def write_current(root, generation, manifest_sha):
    if not _GENERATION.match(generation):
        raise PublishError("generation id malformed")
    root = Path(root)
    temporary = root / (".current-" + secrets.token_hex(8))
    with open(temporary, "x", encoding="ascii", newline="\n") as stream:
        stream.write("%s %s\n" % (generation, manifest_sha))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, root / CURRENT)


def read_current(root, verifier=None):
    """The published generation, fully re-verified; None when nothing is published."""
    root = Path(root)
    try:
        text = (root / CURRENT).read_text(encoding="ascii")
    except FileNotFoundError:
        return None
    parts = text.split()
    if len(parts) != 2 or not _GENERATION.match(parts[0]) or not re.fullmatch(r"[0-9a-f]{64}", parts[1]):
        raise PublishError("CURRENT pointer malformed")
    directory = root / "generations" / parts[0]
    manifest = verify_tree(directory, verifier)
    if hashlib.sha256((directory / MANIFEST).read_bytes()).hexdigest() != parts[1]:
        raise PublishError("CURRENT manifest hash mismatch")
    return {"generation": parts[0], "manifest_sha256": parts[1], "manifest": manifest, "directory": directory}
