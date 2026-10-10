"""Regenerate tools/wii/cache_schema_tables.c from the upstream tag validator's schema.

Builds port/wii/memory_strategy/schema_export.c with the actual
port/linux/game/tag_schema_*.c units for the upstream i686 Linux ABI (clang in
WSL, the flags of tools/linux_build.py that decide layout), runs it and writes
its output. The generated file is committed; `--check` regenerates into memory
and fails if it differs. The header names the schema sources' SHA-256.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_state_census as census  # noqa: E402

ROOT = census.ROOT
OUTPUT = ROOT / "tools/wii/cache_schema_tables.c"
SCHEMA_UNITS = sorted(path.relative_to(ROOT) for path in (ROOT / "port/linux/game").glob("tag_schema_*.c"))


def schema_sources_hash(root=ROOT):
    digest = hashlib.sha256()
    for path in [Path("port/linux/game/tag_schema.h"), *SCHEMA_UNITS, Path("port/wii/memory_strategy/schema_export.c")]:
        # Line endings are normalized so the provenance hash matches in LF (CI)
        # and CRLF (autocrlf) checkouts; the generated offsets do not depend on them.
        data = (root / path).read_bytes().replace(b"\r\n", b"\n")
        digest.update(path.as_posix().encode() + b"\0" + hashlib.sha256(data).digest())
    return digest.hexdigest()


def generate(root=ROOT, work=None):
    work = work or root / ".local/tag-schema-export"
    work.mkdir(parents=True, exist_ok=True)
    semantics = census.semantics_header(root, work)
    flags = ["clang", *[flag for flag in census.UPSTREAM_ABI if flag != "-fsyntax-only"],
             "-include", census.wsl_path(root / "port/linux/include/halo_linux_prefix.h"),
             "-include", census.wsl_path(semantics),
             f"-I{census.wsl_path(root / 'port/linux/include')}", f"-I{census.wsl_path(root / 'port/include/xdk')}",
             f"-I{census.wsl_path(root / 'port/linux/game')}", *census.include_flags(root)]
    units = [census.wsl_path(root / unit) for unit in SCHEMA_UNITS] + [
        census.wsl_path(root / "port/wii/memory_strategy/schema_export.c")]
    program = census.wsl_path(work / "schema_export")
    script = (f"set -e; {' '.join(census.repr_sh(flag) for flag in flags)} "
              f"{' '.join(census.repr_sh(unit) for unit in units)} -no-pie -static -Wl,--unresolved-symbols=ignore-all "
              f"-o {census.repr_sh(program)}; {census.repr_sh(program)}")
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c", script], capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit(f"schema export failed ({run.returncode}):\n{run.stderr[-4000:]}")
    lines = run.stdout.replace("\r\n", "\n").split("\n", 2)
    header = (f"{lines[0]}\n{lines[1]}\n/* schema sources SHA-256 {schema_sources_hash(root)}; "
              f"{census.compiler_identity()} */\n")
    return header + lines[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = generate()
    if args.check:
        same = OUTPUT.exists() and OUTPUT.read_text(encoding="utf-8") == text
        print("schema tables current" if same else "schema tables differ")
        return 0 if same else 1
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT.relative_to(ROOT).as_posix()} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
