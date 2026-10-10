#!/usr/bin/env python3
"""Build and run the host reference of the Wii engine's fixed-step scenario (HWI-015).

Compiles tools/wii/engine_scenario_host.c with the engine units the scenario
uses (port/wii/engine/fixed_step_scenario.c, source/game/game_time.c,
source/memory/data.c, crc.c, source/math/random_math.c, real_math.c and the
shared musl-derived maths) exactly as the Linux build compiles the game:
clang for i686 with tools/linux_build.py's LINUX_ABI_FLAGS and GAME_FLAGS,
the Linux prefix header and the generated MSVC semantics header. Then runs it
and writes its CADENCE lines, which the Wii log's must equal.

Run where an i686 clang and glibc are installed (Linux, or WSL on Windows):
    python3 tools/wii/run_engine_scenario_host.py --cc clang --output <new dir>
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.linux_build import (GAME_FLAGS, LINUX_ABI_FLAGS, MUSL_MATH_DIR,  # noqa: E402
                               musl_math_sources)

ENGINE_UNITS = ["port/wii/engine/fixed_step_scenario.c", "source/game/game_time.c", "source/memory/data.c",
                "source/memory/crc.c", "source/math/random_math.c", "source/math/real_math.c"]
FIXTURE = "tools/wii/engine_scenario_host.c"


def game_flags(semantics: Path) -> list:
    config = json.loads((ROOT / "port/linux/port.json").read_text(encoding="utf-8"))["game"]
    return [*LINUX_ABI_FLAGS, "-march=x86-64", "-ffunction-sections", "-fdata-sections", *GAME_FLAGS, "-include", "port/linux/include/halo_linux_prefix.h",
            "-include", str(semantics), "-Iport/linux/include", "-iquote", "port/linux/game",
            *(f"-D{define}" for define in config.get("defines", [])),
            *(f"-I{directory}" for directory in config.get("include_dirs", [])),
            "-idirafter", "port/include/xdk"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    semantics = out / "halo_msvc_semantics.h"
    subprocess.run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(semantics), "--all-inlines",
                    "--tags", "source", "--inlines", "source", "--inlines", "port/include/xdk"], cwd=ROOT, check=True)
    flags = game_flags(semantics)
    objects = []
    for index, unit in enumerate([*ENGINE_UNITS, FIXTURE]):
        obj = out / f"{index:02d}_{Path(unit).stem}.o"
        subprocess.run([args.cc, *flags, "-c", unit, "-o", str(obj)], cwd=ROOT, check=True)
        objects.append(obj)
    math_flags = [*LINUX_ABI_FLAGS, "-march=x86-64", "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include", "-include",
                  f"{MUSL_MATH_DIR}/include/libm.h"]
    for source in musl_math_sources():
        obj = out / f"musl_{source.stem}.o"
        subprocess.run([args.cc, *math_flags, "-c", str(source), "-o", str(obj)], cwd=ROOT, check=True)
        objects.append(obj)
    program = out / "engine_scenario_host"
    subprocess.run([args.cc, "--target=i686-linux-gnu", "-m32", "-no-pie", "-Wl,--gc-sections", "-o", str(program),
                    *map(str, objects), "-lm"], cwd=ROOT, check=True)
    run = subprocess.run([str(program)], cwd=ROOT, capture_output=True, text=True)
    (out / "host.txt").write_text(run.stdout, encoding="ascii", newline="\n")
    version = subprocess.run([args.cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    record = {"compiler": version, "flags": flags, "exit_code": run.returncode,
              "sources_sha256": {unit: hashlib.sha256((ROOT / unit).read_bytes()).hexdigest()
                                 for unit in [*ENGINE_UNITS, FIXTURE]},
              "lines": run.stdout.splitlines()}
    (out / "host.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8", newline="\n")
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
