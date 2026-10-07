"""Asset-free devkitPPC/libogc probe rules; no desktop generator dependencies."""

import hashlib
import json
import os
from pathlib import Path
import subprocess

from .wii.check_toolchain import inspect_toolchain, toolchain_inputs

BUILD = Path("build/wii")
SOURCES = [Path("port/wii/probe/main.c"), Path("port/wii/abi/boundary.c"), Path("port/wii/abi/fixture.c")]
MACHINE_FLAGS = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]
CFLAGS = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", *MACHINE_FLAGS]
LIBRARIES = ("libfat.a", "libwiiuse.a", "libbte.a", "libogc.a")


def source_inputs():
    return [Path("tools/wii_build.py"), Path("tools/wii/build.py"),
            Path("tools/wii/check_toolchain.py"), *SOURCES, *sorted(Path("port/wii/abi").glob("*.h"))]


def wii_configure_inputs(devkitpro=None):
    paths = source_inputs()
    if devkitpro is not None:
        paths.extend(toolchain_inputs(Path(devkitpro).resolve()))
        for name in ("HEAD", "index", "packed-refs"):
            path = Path(subprocess.check_output(["git", "rev-parse", "--git-path", name], text=True).strip())
            if path.exists():
                paths.append(path)
        branch = subprocess.run(["git", "symbolic-ref", "-q", "HEAD"], text=True, capture_output=True)
        if branch.returncode == 0:
            path = Path(subprocess.check_output(["git", "rev-parse", "--git-path", branch.stdout.strip()], text=True).strip())
            if path.exists():
                paths.append(path)
        elif branch.returncode != 1:
            raise ValueError(branch.stderr.strip())
    return paths


def write_if_changed(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def source_identity():
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--", "configure.py", "tools/wii_build.py", "tools/wii", "port/wii"], text=True))
    return commit, dirty


def generate_wii_build(n, sln):
    value = sln.wii_devkitpro or os.environ.get("DEVKITPRO")
    if not value:
        raise ValueError("Wii needs --wii-devkitpro <native path> or DEVKITPRO from the official shell")
    root = Path(value).resolve()
    if not 0 <= sln.wii_probe_frames <= 36000:
        raise ValueError("--wii-probe-frames must be between 0 and 36000")
    inventory = inspect_toolchain(root)
    if inventory["errors"]:
        raise ValueError("Wii toolchain preflight failed:\n" + "\n".join(inventory["errors"]))
    libraries = [root / "libogc/lib/wii" / name for name in LIBRARIES]
    for library in libraries:
        if not library.is_file():
            raise ValueError(f"Missing official Wii development library: {library}")
    commit, dirty = source_identity()
    inputs = [Path("configure.py"), *source_inputs()]
    hashes = {path.as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    sdk = hashlib.sha256()
    for path in toolchain_inputs(root):
        sdk.update(path.relative_to(root).as_posix().encode() + b"\0")
        if path.is_file():
            sdk.update(hashlib.sha256(path.read_bytes()).digest())
    public = {
        "schema_version": 1, "scope": "asset_free_probe", "source_commit": commit,
        "source_dirty": dirty, "inputs_sha256": hashes,
        "compiler": inventory["tools"]["powerpc-eabi-gcc"]["version"],
        "compiler_target": inventory["tools"]["powerpc-eabi-gcc"]["target"],
        "binutils": inventory["tools"]["powerpc-eabi-ld"]["version"],
        "compile_flags": CFLAGS, "probe_auto_exit_frames": sln.wii_probe_frames,
        "wii_rules_sha256": hashlib.sha256((root / "devkitPPC/wii_rules").read_bytes()).hexdigest(),
        "libogc_sha256": hashlib.sha256((root / "libogc/lib/wii/libogc.a").read_bytes()).hexdigest(),
        "libraries_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in libraries},
        "runtime_verified": False,
        "toolchain_sha256": sdk.hexdigest(),
    }
    public["build_id"] = hashlib.sha256(json.dumps(public, sort_keys=True).encode()).hexdigest()[:16]
    local = {"devkitpro": root.as_posix(), "tools": inventory["tools"], "public": public}
    write_if_changed(BUILD / "local-config.json", json.dumps(local, indent=2) + "\n")
    write_if_changed(BUILD / "build_id.h", f'#define WII_BUILD_ID "{public["build_id"]}"\n#define WII_PROBE_AUTO_EXIT_FRAMES {sln.wii_probe_frames}U\n')

    n.comment("Native Wii probe: flags derived from the installed official wii_rules")
    runner = "$python tools/wii/build.py"
    config = "--config build/wii/local-config.json"
    n.rule("wii_cc", f'{runner} compile {config} --source "$in" --output "$out"',
           description="PPC CC $in", depfile="$out.d", deps="gcc")
    n.rule("wii_link", f"{runner} link {config} --output build/wii/probe.elf $in", description="PPC LINK probe.elf")
    n.rule("wii_dol", f'{runner} convert {config} --source "$in" --output "$out"', description="ELF2DOL probe.dol")
    n.rule("wii_manifest", f'{runner} manifest {config} --output "$out"', description="MANIFEST Wii probe", restat=True)
    objects = []
    for source in SOURCES:
        obj = BUILD / (source.stem + ".o")
        n.build(obj, "wii_cc", source, implicit=[BUILD / "local-config.json", BUILD / "build_id.h"])
        objects.append(obj)
    n.build(BUILD / "probe.elf", "wii_link", objects,
            implicit=[BUILD / "local-config.json", *libraries],
            implicit_outputs=BUILD / "probe.map")
    n.build(BUILD / "probe.dol", "wii_dol", BUILD / "probe.elf", implicit=BUILD / "local-config.json")
    n.build(BUILD / "build-info.json", "wii_manifest",
            implicit=[BUILD / "probe.elf", BUILD / "probe.dol", BUILD / "probe.map", BUILD / "local-config.json"])
    n.build("wii_probe", "phony", [BUILD / "probe.dol", BUILD / "build-info.json"])
    n.newline()
