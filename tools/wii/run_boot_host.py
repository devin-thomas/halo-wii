#!/usr/bin/env python3
"""Build and run the i686 host reference of the Wii engine build's boot mode (HWI-016D).

The units of the real-map host reference (tools/wii/run_engine_map_host.py
units(), read as it is), with tools/wii/boot_menu_host.c as the driver in
place of engine_map_host.c, plus the boot mode's engine side
(port/wii/render/render_ui.c) and a GX side that draws nothing
(tools/wii/boot_host_render_stub.c). clang in WSL Debian, as there.

A run replays a Wii launch: its pad (the BOOT_PAD lines of its engine.log,
scan by scan) and its arena placement (its ARENA line), with the staged
maps' little-endian files in the run directory's sd:/halo-wii-engine/.

    python tools/wii/run_boot_host.py --build <dir> [--no-build] --run <new dir> --wii-log <engine.log>
        --map <private <name>.le.wmap> [--map ...] --max-frames <n> --environment-frames <n> [--settle-frames 45]
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools/wii"))

import run_engine_map_host as map_host  # noqa: E402

DRIVER = Path("tools/wii/boot_menu_host.c")
STUB = Path("tools/wii/boot_host_render_stub.c")
ENGINE_SIDE = Path("port/wii/render/render_ui.c")
PAD_LINE = re.compile(r"^BOOT_PAD(2?) scan=(\d+) clock_ms=\d+ connected=([0-9a-f]+) buttons=([0-9a-f]+) "
                      r"stick=(-?\d+),(-?\d+) cstick=(-?\d+),(-?\d+) triggers=(\d+),(\d+)")
ARENA_LINE = re.compile(r"^ARENA open offset=(\d+) low=(0x[0-9a-f]+) high_before=(0x[0-9a-f]+)")


def units(semantics, platform_semantics):
    """the map host's units, with this driver, the engine side and the stub"""
    result = []
    engine_flags = driver_flags = None
    for source, flags in map_host.units(semantics, platform_semantics):
        name = Path(source).name
        if name == "engine_map_host.c":
            driver_flags = flags
            continue
        if name == "boot_hooks.c":
            engine_flags = flags
        result.append((source, flags))
    if engine_flags is None or driver_flags is None:
        raise SystemExit("the map host's units changed: no boot_hooks.c or engine_map_host.c")
    driver_flags = [*driver_flags, f"-I{map_host.wsl(ROOT / 'port/wii/render')}"]
    return result + [(DRIVER, driver_flags), (STUB, driver_flags), (ENGINE_SIDE, engine_flags)]


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
        relative = (source if not Path(source).is_absolute() else Path(source).relative_to(ROOT)).as_posix()
        obj = out / "obj" / (relative.replace(" ", "_").replace("/", "__") + ".o")
        objects.append(obj)
        lines.append(f"build {map_host.wsl(obj).replace(' ', '$ ')}: cc {map_host.wsl(ROOT / relative).replace(' ', '$ ')}")
        lines.append("  flags = " + " ".join(map_host.sh(flag) if " " in flag else flag for flag in flags))
    program = out / "boot_menu_host"
    wraps = [f"-Wl,--wrap={name}" for name in map_host.eb.wrapped("omit")]
    lines.append(f"build {map_host.wsl(program)}: link " + " ".join(map_host.wsl(obj).replace(" ", "$ ")
                                                                     for obj in objects))
    lines.append("  flags = -Wl,--gc-sections " + " ".join(wraps))
    (out / "build.ninja").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    started = time.perf_counter()
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c",
                          f"cd {map_host.sh(map_host.wsl(ROOT))} && ninja -f {map_host.sh(map_host.wsl(out / 'build.ninja'))}"],
                         capture_output=True, text=True)
    (out / "build.log").write_text(run.stdout[-200000:] + run.stderr[-200000:], encoding="utf-8")
    if run.returncode != 0:
        raise SystemExit(f"host build failed ({run.returncode}); see {out / 'build.log'}:\n{run.stdout[-3000:]}")
    (out / "build-seconds.txt").write_text(f"{time.perf_counter() - started:.1f}\n", encoding="ascii")
    return program


def pad_lines(log_text):
    """the Wii launch's pad changes: '<port> <scan> <connected> <buttons> <sx> <sy> <cx> <cy> <l> <r>' lines
    (port 0, or 1 for the BOOT_PAD2 lines of a second player)"""
    result = []
    for line in log_text.splitlines():
        match = PAD_LINE.match(line)
        if match:
            groups = match.groups()
            result.append(" ".join(("1" if groups[0] else "0", *groups[1:])))
    return result


def arena_of(log_text):
    """(arena2 low, arena2 high, placement offset) of the Wii launch"""
    for line in log_text.splitlines():
        match = ARENA_LINE.match(line)
        if match:
            return match.group(2), match.group(3), match.group(1)
    raise SystemExit("the Wii log has no ARENA line")


def maps_manifest(maps):
    """the card's maps manifest for the host: each map's line of the manifest.le.txt beside it (map_stage.py)"""
    header, lines = None, {}
    for path in maps:
        listed = path.parent / "manifest.le.txt"
        if not listed.is_file():
            raise SystemExit(f"no manifest.le.txt beside {path.name} (map_stage.py writes it)")
        rows = listed.read_text(encoding="ascii").splitlines()
        header = header or rows[0]
        name = path.name[:-len(".le.wmap")] if path.name.endswith(".le.wmap") else path.stem
        row = next((row for row in rows[1:] if row.split()[:1] == [name]), None)
        if row is None:
            raise SystemExit(f"manifest.le.txt does not list {name}")
        lines[name] = row
    return header + "\n" + "".join(lines[key] + "\n" for key in sorted(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--run", type=Path)
    parser.add_argument("--wii-log", type=Path)
    parser.add_argument("--map", type=Path, action="append", default=[], help="a little-endian staged map (private)")
    parser.add_argument("--max-frames", type=int, default=5400)
    parser.add_argument("--environment-frames", type=int, default=150)
    parser.add_argument("--settle-frames", type=int, default=45)
    parser.add_argument("--dump", type=int, help="a recorded allocation's index: its bytes at the first screen "
                                               "sample to the data folder's boot_dump.bin (diagnosis)")
    args = parser.parse_args()
    out = args.build.resolve()
    program = out / "boot_menu_host" if args.no_build else build(out)
    if not args.run:
        return 0
    if not args.wii_log or not args.map:
        parser.error("--run needs --wii-log and --map")
    log_text = args.wii_log.read_text(encoding="ascii", errors="replace")
    low, high, offset = arena_of(log_text)
    run_dir = args.run.resolve()
    run_dir.mkdir(parents=True, exist_ok=False)
    (run_dir / "pad.txt").write_text("\n".join(pad_lines(log_text)) + "\n", encoding="ascii", newline="\n")
    # (the maps' little-endian manifest lines, from manifest.le.txt beside each, as the card's manifest.txt)
    (run_dir / "manifest.txt").write_text(maps_manifest(args.map), encoding="ascii", newline="\n")
    copies = " && ".join([
        *(f"cp {map_host.sh(map_host.wsl(path.resolve()))} "
          f"{map_host.sh(map_host.wsl(run_dir) + '/sd:/halo-wii-engine/data/maps/' + path.name.replace('.le.wmap', '.wmap'))}"
          for path in args.map),
        f"mv {map_host.sh(map_host.wsl(run_dir / 'manifest.txt'))} "
        f"{map_host.sh(map_host.wsl(run_dir) + '/sd:/halo-wii-engine/data/maps/')}"])
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c",
                          f"mkdir -p {map_host.sh(map_host.wsl(run_dir) + '/sd:/halo-wii-engine/data/maps')} && {copies}"],
                         capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit(run.stderr)
    command = [map_host.wsl(program), low, high, offset, map_host.wsl(run_dir / "pad.txt"), str(args.max_frames),
               str(args.environment_frames), str(args.settle_frames)]
    started = time.perf_counter()
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c",
                          f"cd {map_host.sh(map_host.wsl(run_dir))} && "
                          f"{('BOOT_DUMP=' + str(args.dump) + ' ') if args.dump is not None else ''}"
                          f"{' '.join(map_host.sh(part) for part in command)}"],
                         capture_output=True, text=True)
    seconds = time.perf_counter() - started
    record = {"command": ["boot_menu_host", *command[1:4], "pad.txt", *command[5:]], "exit_code": run.returncode,
              "seconds": round(seconds, 1), "program_sha256": hashlib.sha256(program.read_bytes()).hexdigest(),
              "stdout_tail": run.stdout[-2000:], "stderr_tail": run.stderr[-8000:]}
    (run_dir / "host.json").write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"exit={run.returncode} seconds={seconds:.1f}")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
