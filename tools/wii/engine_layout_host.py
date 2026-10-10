#!/usr/bin/env python3
"""The i686 side of the engine layout census (HWI-015, tools/wii/engine_layout.py dwarf).

Compiles every engine unit the Wii engine build compiles (tools/wii/engine_build.py
game_units) for the native ports' i686 ABI with the Linux build's game flags
(tools/wii/run_engine_scenario_host.py game_flags) and debug information, and
writes one `readelf --debug-dump=info` listing of all of them. Run it where an
i686 clang is installed (Linux, or WSL on Windows):

    python3 tools/wii/engine_layout_host.py --work <scratch directory> --output <listing>
"""

import argparse
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.wii.engine_build import game_units  # noqa: E402
from tools.wii.run_engine_scenario_host import game_flags  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    semantics = work / "halo_msvc_semantics.h"
    subprocess.run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(semantics), "--all-inlines",
                    "--tags", "source", "--inlines", "source", "--inlines", "port/include/xdk"], cwd=ROOT, check=True)
    flags = game_flags(semantics)

    def compile_unit(unit):
        obj = work / (unit.as_posix().replace("/", "__").replace(" ", "_") + ".o")
        run = subprocess.run([args.cc, *flags, "-c", str(unit), "-o", str(obj)], cwd=ROOT, capture_output=True,
                             text=True)
        return unit, obj, run.returncode

    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(compile_unit, game_units()))
    failed = [unit.as_posix() for unit, _, code in results if code]
    with args.output.open("w", encoding="utf-8") as listing:
        for _, obj, code in results:
            if code == 0:
                listing.write(subprocess.run([args.readelf, "--debug-dump=info", str(obj)], capture_output=True,
                                             text=True, errors="replace").stdout)
    print(f"units={len(results)} failed={len(failed)} {failed[:5]}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
