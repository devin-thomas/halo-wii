#!/usr/bin/env python3
"""Compare the Wii's real-map runs with the i686 host reference's, run by run (HWI-015B, HWI-016C).

Both run port/wii/engine/engine_map_run.c and log the same lines (RUN,
ALLOC, TICKS, FIXED_STEP, MAP_RUNS, LOAD_FAILURE, BSP_FAILURE). For each run
(map, cycle, cadence) present in both logs this compares the whole game
state's digest and chain, the simulation's, the checkpoints, the random seed,
the player's position, the structure BSP and the tag slot's digests; then
every allocation's checkpoint digests, every tick's simulation digest of each
map's first run, and each refused load's kind.

The host has no video: its "vsync_measured" run is paced at 1/60 s unless it
replays the Wii's measured frame times, so that run's frame-coupled state is
reported separately rather than hidden.

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
    runs, allocations, ticks, failures = {}, {}, {}, []
    fixed = None
    summaries = {}
    for line in text.splitlines():
        if line.startswith("RUN map=") or line.startswith("RUN cycle="):
            f = fields(line)
            if f["cycle"] != "-1":
                runs[(f.get("map", ""), f["cycle"], f["cadence"])] = f
        elif line.startswith("ALLOC "):
            f = fields(line)
            allocations[(f.get("map", ""), f["cycle"], f["cadence"], f["index"])] = f
        elif line.startswith("TICKS "):
            f = fields(line)
            ticks.setdefault(f.get("map", ""), []).extend(word for word in line.split()[1:] if "=" not in word)
        elif line.startswith("FIXED_STEP "):
            fixed = fields(line)
        elif line.startswith("MAP_RUNS "):
            f = fields(line)
            summaries[f.get("map", "")] = f
        elif line.startswith("LOAD_FAILURE ") or line.startswith("BSP_FAILURE "):
            f = fields(line)
            failures.append({key: f.get(key) for key in ("map", "cycle", "cadence", "expected", "kind", "pass", "bsp")})
    return {"runs": runs, "allocations": allocations, "ticks": ticks, "fixed": fixed, "summaries": summaries,
            "failures": failures}


CHECKPOINTS = ("at1", "at30", "at150", "atend", "at300")
RUN_KEYS_WHOLE = ("digest", "chain", "at1", "at30", "at150", "atend", "at300")
RUN_KEYS_SIMULATION = ("sim", "sim_chain", "sim_at1", "sim_at30", "sim_at150", "sim_atend", "sim_at300", "seed",
                       "objects", "player_unit", "spawn_tick", "position", "ticks", "tags_end", "bsp", "bsp_switches")


def compare(wii, host):
    report = {"runs": [], "allocations_compared": 0, "allocation_differences": [], "ticks_compared": {},
              "first_divergent_tick": {}, "failures_same": wii["failures"] == host["failures"],
              "failures": wii["failures"]}
    for key in sorted(set(wii["runs"]) & set(host["runs"])):
        w, h = wii["runs"][key], host["runs"][key]
        simulation = [k for k in RUN_KEYS_SIMULATION if w.get(k) != h.get(k)]
        whole = [k for k in RUN_KEYS_WHOLE if w.get(k) != h.get(k)]
        report["runs"].append({"map": key[0], "cycle": int(key[1]), "cadence": key[2],
                               "simulation_same": not simulation, "simulation_differences": simulation,
                               "whole_same": not whole, "whole_differences": whole,
                               "frame_paced": key[2] == FRAME_PACED, "digest": w.get("digest"), "sim": w.get("sim")})
    for key in sorted(set(wii["allocations"]) & set(host["allocations"])):
        w, h = wii["allocations"][key], host["allocations"][key]
        report["allocations_compared"] += 1
        different = [k for k in (*CHECKPOINTS, "bytes", "offset") if w.get(k) != h.get(k)]
        if different:
            report["allocation_differences"].append({"map": key[0], "cycle": int(key[1]), "cadence": key[2],
                                                     "index": int(key[3]), "name": w.get("name"),
                                                     "fields": different})
    for name in sorted(set(wii["ticks"]) & set(host["ticks"])):
        w, h = wii["ticks"][name], host["ticks"][name]
        report["ticks_compared"][name] = min(len(w), len(h))
        report["first_divergent_tick"][name] = None
        for index, (a, b) in enumerate(zip(w, h)):
            if a != b:
                report["first_divergent_tick"][name] = index + 1
                break
    report["fixed_step_same"] = bool(wii["fixed"] and host["fixed"] and
                                     wii["fixed"].get("digest") == host["fixed"].get("digest") and
                                     wii["fixed"].get("chain") == host["fixed"].get("chain"))
    unpaced = [run for run in report["runs"] if not run["frame_paced"]]
    report["simulation_all_same"] = bool(report["runs"]) and all(run["simulation_same"] for run in report["runs"])
    report["whole_all_same_unpaced"] = bool(unpaced) and all(run["whole_same"] for run in unpaced)
    report["allocations_all_same_unpaced"] = not [d for d in report["allocation_differences"]
                                                  if d["cadence"] != FRAME_PACED]
    report["ticks_all_same"] = all(tick is None for tick in report["first_divergent_tick"].values())
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
                                                   "ticks_compared", "first_divergent_tick", "ticks_all_same",
                                                   "fixed_step_same", "failures_same")}))
    for run in report["runs"]:
        print(f"map={run['map']} cycle={run['cycle']} cadence={run['cadence']} simulation_same={run['simulation_same']} "
              f"whole_same={run['whole_same']} {run['simulation_differences'] + run['whole_differences']}")
    for difference in report["allocation_differences"][:60]:
        print("ALLOC_DIFF", difference)
    return 0 if report["simulation_all_same"] and report["whole_all_same_unpaced"] and report["failures_same"] else 1


if __name__ == "__main__":
    sys.exit(main())
