#!/usr/bin/env python3
"""Compare the Wii's real-map run with the i686 host reference's, run by run (HWI-015B).

Both run port/wii/engine/engine_map_run.c and log the same lines (RUN,
ALLOC, TICKS, FIXED_STEP, MAP_RUNS). For each run (cycle, cadence) present in
both logs this compares the whole game state's digest and chain, the
simulation's, the checkpoints, the random seed, the player's position and
the tag slot's digests; then every allocation's checkpoint digests (logged for
the first cycle), and every tick's simulation digest of the first run.

The host has no video: its "vsync_measured" run is paced at 1/60 s, so that
run's frame-coupled state is not expected to match the Wii's measured frames
(its simulation still must). The report says so instead of hiding it.

    python tools/wii/compare_map_runs.py --wii <engine.log> --host <engine.log> [--json <out>]
"""
import argparse
import json
import re
import sys

FIELD = re.compile(r"(\w+)=(\"[^\"]*\"|\S+)")
FRAME_PACED = "vsync_measured"


def fields(line):
    return {key: value.strip('"') for key, value in FIELD.findall(line)}


def parse(text):
    runs, allocations, ticks = {}, {}, []
    fixed = None
    summary = None
    for line in text.splitlines():
        if line.startswith("RUN cycle="):
            f = fields(line)
            if f["cycle"] != "-1":
                runs[(f["cycle"], f["cadence"])] = f
        elif line.startswith("ALLOC cycle="):
            f = fields(line)
            allocations[(f["cycle"], f["cadence"], f["index"])] = f
        elif line.startswith("TICKS "):
            ticks.extend(line.split()[2:])
        elif line.startswith("FIXED_STEP "):
            fixed = fields(line)
        elif line.startswith("MAP_RUNS "):
            summary = fields(line)
    return {"runs": runs, "allocations": allocations, "ticks": ticks, "fixed": fixed, "summary": summary}


RUN_KEYS_WHOLE = ("digest", "chain", "at1", "at30", "at150", "at300")
RUN_KEYS_SIMULATION = ("sim", "sim_chain", "sim_at1", "sim_at30", "sim_at150", "sim_at300", "seed", "objects",
                       "player_unit", "spawn_tick", "position", "ticks", "tags_end")


def compare(wii, host):
    report = {"runs": [], "allocations_compared": 0, "allocation_differences": [], "ticks_compared": 0,
              "first_divergent_tick": None}
    for key in sorted(set(wii["runs"]) & set(host["runs"])):
        w, h = wii["runs"][key], host["runs"][key]
        simulation = [k for k in RUN_KEYS_SIMULATION if w.get(k) != h.get(k)]
        whole = [k for k in RUN_KEYS_WHOLE if w.get(k) != h.get(k)]
        report["runs"].append({"cycle": int(key[0]), "cadence": key[1], "simulation_same": not simulation,
                               "simulation_differences": simulation, "whole_same": not whole,
                               "whole_differences": whole, "frame_paced": key[1] == FRAME_PACED,
                               "digest": w.get("digest"), "sim": w.get("sim")})
    for key in sorted(set(wii["allocations"]) & set(host["allocations"])):
        w, h = wii["allocations"][key], host["allocations"][key]
        report["allocations_compared"] += 1
        different = [k for k in ("at1", "at30", "at150", "at300", "bytes", "offset") if w.get(k) != h.get(k)]
        if different:
            report["allocation_differences"].append({"cycle": int(key[0]), "cadence": key[1], "index": int(key[2]),
                                                     "name": w.get("name"), "fields": different})
    report["ticks_compared"] = min(len(wii["ticks"]), len(host["ticks"]))
    for index, (w, h) in enumerate(zip(wii["ticks"], host["ticks"])):
        if w != h:
            report["first_divergent_tick"] = index + 1
            break
    report["fixed_step_same"] = bool(wii["fixed"] and host["fixed"] and
                                     wii["fixed"].get("digest") == host["fixed"].get("digest") and
                                     wii["fixed"].get("chain") == host["fixed"].get("chain"))
    unpaced = [run for run in report["runs"] if not run["frame_paced"]]
    report["simulation_all_same"] = bool(report["runs"]) and all(run["simulation_same"] for run in report["runs"])
    report["whole_all_same_unpaced"] = bool(unpaced) and all(run["whole_same"] for run in unpaced)
    report["allocations_all_same_unpaced"] = not [d for d in report["allocation_differences"]
                                                  if d["cadence"] != FRAME_PACED]
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wii", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--json")
    args = parser.parse_args()
    read = lambda path: open(path, encoding="ascii", errors="replace").read()  # noqa: E731
    report = compare(parse(read(args.wii)), parse(read(args.host)))
    text = json.dumps(report, indent=1)
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as out:
            out.write(text + "\n")
    print(json.dumps({key: report[key] for key in ("simulation_all_same", "whole_all_same_unpaced",
                                                   "allocations_all_same_unpaced", "allocations_compared",
                                                   "ticks_compared", "first_divergent_tick", "fixed_step_same")}))
    for run in report["runs"]:
        print(f"cycle={run['cycle']} cadence={run['cadence']} simulation_same={run['simulation_same']} "
              f"whole_same={run['whole_same']} {run['simulation_differences'] + run['whole_differences']}")
    for difference in report["allocation_differences"][:40]:
        print("ALLOC_DIFF", difference)
    return 0 if report["simulation_all_same"] and report["whole_all_same_unpaced"] else 1


if __name__ == "__main__":
    sys.exit(main())
