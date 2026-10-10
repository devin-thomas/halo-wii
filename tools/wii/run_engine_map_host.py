#!/usr/bin/env python3
"""Build and run the i686 host reference of the Wii engine build's real-map run (HWI-015B).

The same engine units as engine.dol (tools/wii/engine_build.py, the Wii
replacements included: cache_files_wii.c, physical_memory_map_wii.c), and
the same platform layer (port/wii/engine) but the Wii driver, compiled for the
native ports' i686 ABI with tools/linux_build.py's LINUX_ABI_FLAGS and
GAME_FLAGS: clang in WSL Debian. libogc's few services come from
tools/wii/engine_host_shim.c; the driver is tools/wii/engine_map_host.c, which
runs port/wii/engine/engine_map_run.c as the Wii does.

The map is the little-endian staging of the same owned map
(tools/wii/map_stage.py <map>.le.wmap), copied into the run directory's
sd:/halo-wii-engine/data/maps/. MEM2's arena is mapped at the addresses the
Wii run reported (its ARENA line), so pointers into the engine's regions have
the same values on both.

    python tools/wii/run_engine_map_host.py --build <dir> --map <private .le.wmap> \\
        --scenario <levels\\...\\name> --run <new dir> [--cycles 3] \\
        [--arena2 0x90002000 0x933db7e0] [--offset 0]
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools/wii"))

from tools.linux_build import GAME_FLAGS, LINUX_ABI_FLAGS, POSIX_FLAGS  # noqa: E402
import engine_build as eb  # noqa: E402
import game_state_census as census  # noqa: E402

HOST_PREFIX = Path("port/wii/engine/include/halo_wii_host_prefix.h")
HOST_UNITS = [Path("tools/wii/engine_host_shim.c"), Path("tools/wii/engine_map_host.c")]
DRIVER_UNITS = ("runtime_start.c", "cache_arena_plan.c", "engine_map_run.c")
ABI = [*LINUX_ABI_FLAGS, "-march=x86-64"]
STRICT = ["-Wall", "-Wextra", "-Werror"]


def wsl(path: Path) -> str:
    return census.wsl_path(path)


def sh(text) -> str:
    return census.repr_sh(text)


def units(semantics: Path, platform_semantics: Path):
    """[(source, flags)] of every unit of the host reference"""
    config = json.loads((ROOT / eb.PORT_CONFIG).read_text(encoding="utf-8"))["game"]
    engine = [*ABI, *GAME_FLAGS, *eb.SECTION_FLAGS, *eb.DIAGNOSTIC_STORAGE_OMITTED,
              "-include", wsl(ROOT / HOST_PREFIX), "-include", wsl(semantics),
              f"-I{wsl(ROOT / 'port/linux/include')}", "-iquote", wsl(ROOT / eb.LINUX_GAME),
              *(f"-D{define}" for define in config.get("defines", [])),
              *(f"-I{wsl(ROOT / directory)}" for directory in config.get("include_dirs", [])),
              "-idirafter", wsl(ROOT / eb.XDK_INCLUDE)]
    platform = [*ABI, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-fms-extensions", "-w",
                *eb.SECTION_FLAGS, "-include", wsl(ROOT / HOST_PREFIX), "-include", wsl(platform_semantics),
                *(f"-I{wsl(ROOT / directory)}" for directory in (
                    eb.ENGINE_DIR, eb.ENGINE_DIR / "sdl", eb.LINUX_PLATFORM, Path("port/linux/include"), eb.TOML,
                    eb.ZLIB, Path("port/include"), Path("source"), Path("source/cseries"))),
                "-idirafter", wsl(ROOT / eb.XDK_INCLUDE)]
    posix = [*POSIX_FLAGS, *eb.SECTION_FLAGS, f"-I{wsl(ROOT / eb.LINUX_PLATFORM)}"]
    # (-march=x86-64: SSE arithmetic, as the engine units', not the i686 default x87's)
    driver = ["--target=i686-linux-gnu", "-m32", "-march=x86-64", "-std=gnu11", "-D_GNU_SOURCE", "-O2", "-g", *STRICT,
              *eb.SECTION_FLAGS, f"-I{wsl(ROOT / eb.ENGINE_DIR)}"]
    math = [*ABI, "-std=gnu11", "-w", f"-I{wsl(ROOT / eb.MUSL_MATH)}/include", "-include",
            f"{wsl(ROOT / eb.MUSL_MATH)}/include/libm.h", *eb.SECTION_FLAGS]
    zlib = [*ABI, "-std=gnu11", "-w", *eb.ZLIB_DEFINES, *eb.SECTION_FLAGS]
    result = []
    for original, compiled in eb.engine_units():
        result.append((original if original in eb.REWRITTEN else compiled, engine))
    for path in eb.REUSED_PLATFORM:
        result.append((path, platform))
    for path in eb.WII_PLATFORM:
        if path.name == "wii_engine_main.c":
            continue
        if path.name == "wii_posix_files.c":
            result.append((path, posix))
        elif path.name in DRIVER_UNITS:
            result.append((path, driver))
        else:
            result.append((path, platform))
    result += [(path, driver) for path in HOST_UNITS]
    result += [(path, math) for path in sorted((ROOT / eb.MUSL_MATH / "src").glob("*.c"))]
    result += [(eb.ZLIB / name, zlib) for name in eb.ZLIB_SOURCES]
    result += [(eb.TOML / "tomlc17.c", zlib)]
    return result


def build(out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    semantics = out / "halo_msvc_semantics.h"
    platform_semantics = out / "platform_msvc_semantics.h"
    for target, scan in ((semantics, ["--all-inlines", "--tags", "source", "--inlines", "source", "--inlines",
                                      "port/include/xdk"]),
                         (platform_semantics, ["--inlines", "port/include/xdk"])):
        subprocess.run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(target), *scan], cwd=ROOT,
                       check=True)
    lines = ["rule cc", "  command = clang $flags -MD -MF $out.d -c $in -o $out", "  depfile = $out.d",
             "  deps = gcc", "  description = HOST CC $in",
             "rule link", "  command = clang --target=i686-linux-gnu -m32 -no-pie $flags -o $out @$out.rsp -lm -lpthread",
             "  rspfile = $out.rsp", "  rspfile_content = $in", "  description = HOST LINK $out"]
    objects = []
    for source, flags in units(semantics, platform_semantics):
        relative = (source if not source.is_absolute() else source.relative_to(ROOT)).as_posix()
        obj = out / "obj" / (relative.replace(" ", "_").replace("/", "__") + ".o")
        objects.append(obj)
        lines.append(f"build {wsl(obj).replace(' ', '$ ')}: cc {wsl(ROOT / relative).replace(' ', '$ ')}")
        lines.append("  flags = " + " ".join(sh(flag) if " " in flag else flag for flag in flags))
    program = out / "engine_map_host"
    wraps = [f"-Wl,--wrap={name}" for name in eb.wrapped("omit")]
    lines.append(f"build {wsl(program)}: link " + " ".join(wsl(obj).replace(" ", "$ ") for obj in objects))
    lines.append("  flags = -Wl,--gc-sections " + " ".join(wraps))
    (out / "build.ninja").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    started = time.perf_counter()
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c",
                          f"cd {sh(wsl(ROOT))} && ninja -f {sh(wsl(out / 'build.ninja'))}"],
                         capture_output=True, text=True)
    (out / "build.log").write_text(run.stdout[-200000:] + run.stderr[-200000:], encoding="utf-8")
    if run.returncode != 0:
        raise SystemExit(f"host build failed ({run.returncode}); see {out / 'build.log'}:\n{run.stdout[-3000:]}")
    (out / "build-seconds.txt").write_text(f"{time.perf_counter() - started:.1f}\n", encoding="ascii")
    return program


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--map", type=Path, help="the little-endian staged map (private)")
    parser.add_argument("--scenario", help="the staged map's scenario path (its map.txt on the Wii's card)")
    parser.add_argument("--run", type=Path)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--arena2", nargs=2, default=["0x90002000", "0x933db7e0"])
    parser.add_argument("--offset", default="0")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--vsync-from", type=Path,
                        help="a Wii engine.log whose VSYNC_DT frame times the host's retrace-paced runs replay")
    parser.add_argument("--dump", help="<allocation index>:<tick>: dump that allocation after that tick (diagnosis)")
    args = parser.parse_args()
    out = args.build.resolve()
    program = out / "engine_map_host" if args.no_build else build(out)
    if not args.run:
        return 0
    if not args.scenario or not args.map:
        parser.error("--run needs --map and --scenario")
    run_dir = args.run.resolve()
    run_dir.mkdir(parents=True, exist_ok=False)
    maps = run_dir / "sd:" / "halo-wii-engine" / "data" / "maps"
    # (a directory named "sd:" on the host stands for the Wii's card)
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c",
                          f"mkdir -p {sh(wsl(run_dir) + '/sd:/halo-wii-engine/data/maps')} && "
                          f"cp {sh(wsl(args.map.resolve()))} "
                          f"{sh(wsl(run_dir) + '/sd:/halo-wii-engine/data/maps/' + args.map.name.replace('.le.wmap', '.wmap'))}"],
                         capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit(run.stderr)
    started = time.perf_counter()
    command = [wsl(program), args.scenario, str(args.cycles), *args.arena2, args.offset]
    if args.vsync_from:
        frames = []
        for line in args.vsync_from.read_text(encoding="ascii", errors="replace").splitlines():
            if line.startswith("VSYNC_DT "):
                frames.extend(word for word in line.split()[3:])
        (run_dir / "vsync.txt").write_text(" ".join(frames) + "\n", encoding="ascii", newline="\n")
        command.append(wsl(run_dir / "vsync.txt"))
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c",
                          f"cd {sh(wsl(run_dir))} && "
                          f"{('HWI015B_DUMP=' + sh(args.dump) + ' ') if args.dump else ''}"
                          f"{' '.join(sh(part) for part in command)}"],
                         capture_output=True, text=True)
    seconds = time.perf_counter() - started
    stderr = run.stderr
    if "FAULT" in stderr:
        # the fault's return addresses as functions and lines
        import re
        addresses = re.findall(r"\[(0x[0-9a-f]+)\]", stderr)
        if addresses:
            resolved = subprocess.run(["wsl", "-d", "Debian", "--", "addr2line", "-f", "-i", "-C", "-e", wsl(program),
                                       *addresses], capture_output=True, text=True).stdout
            stderr += "\nRESOLVED\n" + resolved.replace(wsl(ROOT) + "/", "")
    record = {"command": ["engine_map_host", *command[1:]], "exit_code": run.returncode,
              "seconds": round(seconds, 1),
              "compiler": subprocess.run(["wsl", "-d", "Debian", "--", "clang", "--version"], capture_output=True,
                                         text=True).stdout.splitlines()[0],
              "program_sha256": hashlib.sha256(program.read_bytes()).hexdigest(),
              "stdout_tail": run.stdout[-2000:], "stderr_tail": stderr[-8000:]}
    (run_dir / "host.json").write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8", newline="\n")
    log = run_dir / "sd:" / "halo-wii-engine" / "engine.log"
    del maps
    print(f"exit={run.returncode} seconds={seconds:.1f} log={log}")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
