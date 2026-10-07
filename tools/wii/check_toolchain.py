#!/usr/bin/env python3
"""Inspect one devkitPro installation without installing or building anything."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def command_output(command: list[str]) -> str:
    result = subprocess.run(
        command, capture_output=True, text=True, timeout=15, check=True
    )
    return result.stdout.strip()


def inspect_toolchain(root: Path) -> dict:
    root = root.resolve()
    errors = []
    tools = {}
    suffix = ".exe" if os.name == "nt" else ""
    locations = {
        "powerpc-eabi-gcc": [root / "devkitPPC/bin"],
        "powerpc-eabi-ld": [root / "devkitPPC/bin"],
        "elf2dol": [root / "tools/bin", root / "portlibs/wii/bin"],
    }
    if not root.is_dir():
        errors.append(f"devkitPro directory does not exist: {root}")
    for name, directories in locations.items():
        candidates = [directory / (name + suffix) for directory in directories]
        executable = next((path for path in candidates if path.is_file()), None)
        if executable is None:
            errors.append(f"Missing {name}; checked: " + ", ".join(map(str, candidates)))
            continue
        tools[name] = {"path": str(executable)}
        if name == "elf2dol":
            continue  # elf2dol has no version flag in its documented conversion interface.
        try:
            version = command_output([str(executable), "--version"])
            if not version:
                errors.append(f"{name} returned no version information")
            else:
                tools[name]["version"] = version.splitlines()[0]
            if name == "powerpc-eabi-gcc":
                target = command_output([str(executable), "-dumpmachine"])
                tools[name]["target"] = target
                if target != "powerpc-eabi":
                    errors.append(f"Wrong compiler target: {target!r}; expected 'powerpc-eabi'")
        except (OSError, subprocess.SubprocessError) as error:
            errors.append(f"Cannot execute {name}: {error}")

    required_files = (
        "devkitPPC/wii_rules",
        "libogc/include/gccore.h",
        "libogc/lib/wii/libogc.a",
    )
    for relative in required_files:
        if not (root / relative).is_file():
            errors.append(f"Missing Wii development file: {root / relative}")

    ninja = shutil.which("ninja")
    if ninja is None:
        errors.append("Ninja is missing from this Python process's PATH")
    else:
        tools["ninja"] = {"path": ninja}
        try:
            version = command_output([ninja, "--version"])
            if not version:
                errors.append("Ninja returned no version information")
            else:
                tools["ninja"]["version"] = version
        except (OSError, subprocess.SubprocessError) as error:
            errors.append(f"Cannot execute Ninja: {error}")

    return {
        "kind": "tool_inventory_only",
        "devkitpro": str(root),
        "python": {"path": sys.executable, "version": sys.version.split()[0]},
        "tools": tools,
        "errors": errors,
        "ready_for_example_build": not errors,
        "compile_verified": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--devkitpro", type=Path,
        default=Path(os.environ["DEVKITPRO"]) if os.environ.get("DEVKITPRO") else None,
        help="native installation path (or DEVKITPRO); on Windows, e.g. C:\\devkitPro",
    )
    parser.add_argument("--json", action="store_true", help="print a local inventory as JSON")
    args = parser.parse_args()
    if args.devkitpro is None:
        parser.error("Set DEVKITPRO in the devkitPro shell or pass --devkitpro C:\\devkitPro")
    if os.name == "nt" and str(args.devkitpro).replace("\\", "/").startswith("/opt/"):
        parser.error("This is native Windows Python; pass the Windows installation path with --devkitpro")
    record = inspect_toolchain(args.devkitpro)
    if args.json:
        print(json.dumps(record, indent=2))
    else:
        print(f"devkitPro: {record['devkitpro']}")
        print(f"Python: {record['python']['version']}")
        for name, info in record["tools"].items():
            print(f"{name}: {info.get('version', info['path'])}")
        for error in record["errors"]:
            print(f"ERROR: {error}", file=sys.stderr)
        if record["errors"]:
            print("Install the official Wii Development components, then retry in a fresh shell.")
        else:
            print("Tool inventory complete. Build an unmodified official Wii example next.")
        print("This inventory does not prove compilation, Dolphin execution or hardware execution.")
    return 1 if record["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
