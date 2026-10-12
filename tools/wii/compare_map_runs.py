#!/usr/bin/env python3
"""Compare the Wii's real-map runs with the i686 host reference's, run by run (HWI-015B, HWI-016C).

Both run port/wii/engine/engine_map_run.c and log the same lines (RUN,
ALLOC, TICKS, FIXED_STEP, MAP_RUNS, LOAD_FAILURE, BSP_FAILURE). For each run
(map, cycle, cadence) present in both logs this compares the whole game
state's digest and chain, the simulation's, the checkpoints, the random seed,
the player's position, the structure BSP and the tag slot's digests; then
every allocation's checkpoint digests, every tick's simulation digest of each
map's first run, and each refused load's kind.

Two digests (real_map_scenario.c): the canonical one (pointers into the game
state and the tag slot as region and offset, HWI-015B's; since HWI-079 only
the saved game's declared pointer places, so it is the same at every arena
placement) and the raw one (words as they are). Within one launch the Wii
and the host place their regions at the same addresses, and the raw digest
is the comparison. Both are reported; a tick where only the canonical one
differs is listed as such.

The host has no video: its "vsync_measured" run is paced at 1/60 s unless it
replays the Wii's measured frame times, so that run's frame-coupled state is
reported separately rather than hidden.

Cadence invariance (HWI-016F): within each log, every run of a map, whatever
its cadence and cycle, must have the simulation of the map's first run (the
30 Hz run: engine_map_run.c runs a line's 30 Hz first). The RUN lines also
carry the simulation as HWI-015B classified it (sim_hwi015b, compared
between the Wii and the host like the rest) and the effects and looping
sounds the frames made (frame_effects, frame_sounds).

Revert-class requests (HWI-034): a frame of several ticks stops its ticks at
a revert, a loss's revert, a core load, a won map or a map reset
(game_time.c). Per run of each log: no tick begun after such a request in
the frame it arrived in (ticks_after_request=0), and every tick the clock
gave either run or held back for the next frame (clock_ticks = ticks_run +
carried). The Wii's and the host's RUN lines also compare these counts.

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
    runs, allocations, ticks, ticks_raw, failures = {}, {}, {}, {}, []
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
        elif line.startswith("TICKS ") or line.startswith("TICKS_RAW "):
            f = fields(line)
            target = ticks if line.startswith("TICKS ") else ticks_raw
            target.setdefault(f.get("map", ""), []).extend(word for word in line.split()[1:] if "=" not in word)
        elif line.startswith("FIXED_STEP "):
            fixed = fields(line)
        elif line.startswith("MAP_RUNS "):
            f = fields(line)
            summaries[f.get("map", "")] = f
        elif line.startswith("LOAD_FAILURE ") or line.startswith("BSP_FAILURE "):
            f = fields(line)
            failures.append({key: f.get(key) for key in ("map", "cycle", "cadence", "expected", "kind", "pass", "asked", "bsp_before", "bsp_after")})
    return {"runs": runs, "allocations": allocations, "ticks": ticks, "ticks_raw": ticks_raw, "fixed": fixed,
            "summaries": summaries, "failures": failures}


CHECKPOINTS = ("at1", "at30", "at150", "atend", "at300")
RAW_CHECKPOINTS = ("raw1", "raw30", "raw150", "rawend")
RUN_KEYS_WHOLE = ("raw", "raw_chain")
RUN_KEYS_WHOLE_CANONICAL = ("digest", "chain", "at1", "at30", "at150", "atend", "at300")
RUN_KEYS_SIMULATION = ("sim_raw", "sim_raw_chain", "seed", "objects", "player_unit", "spawn_tick", "position",
                       "ticks", "tags_end", "bsp", "bsp_switches", "sim_hwi015b_raw", "sim_hwi015b_raw_chain",
                       "frame_effects", "frame_sounds",
                       # (HWI-034) the revert-class requests and the scheduler's counts
                       "requests", "request_tick", "ticks_after_request", "clock_ticks", "ticks_run", "carried",
                       "frames_stopped", "carried_total", "most_carried", "queue_resets", "core_saved_tick",
                       # (HWI-079) the canonical digest's pointer places (address_values is each byte order's own)
                       "places", "places_failed")
RUN_KEYS_CANONICAL = ("sim", "sim_chain", "sim_at1", "sim_at30", "sim_at150", "sim_atend", "sim_at300", "sim_hwi015b")
# (HWI-016F) what one map's runs must share whatever their cadence: the simulation (HWI-016F's classification)
CADENCE_KEYS = ("sim_raw", "sim_raw_chain", "sim", "seed", "objects", "player_unit", "position", "tags_end", "bsp",
                "bsp_switches", "first_divergent_tick")


def cadence_invariance(log):
    """(HWI-016F) per map of one log: whether every run (every cycle and cadence) has the simulation of the map's
    first run, by the CADENCE_KEYS of the RUN lines; a run's first_divergent_tick is against its line's first run
    (-1: none)"""
    result = {}
    for (name, cycle, cadence), run in sorted(log["runs"].items(), key=lambda item: (item[0][0], int(item[0][1]))):
        entry = result.setdefault(name, {"runs": 0, "cadences": [], "different": [], "reference": None})
        entry["runs"] += 1
        if cadence not in entry["cadences"]:
            entry["cadences"].append(cadence)
        if entry["reference"] is None:
            entry["reference"] = {key: run.get(key) for key in CADENCE_KEYS}
            entry["reference"]["first_divergent_tick"] = "-1"
            continue
        different = [key for key in CADENCE_KEYS if run.get(key) != entry["reference"].get(key)]
        if different:
            entry["different"].append({"cycle": int(cycle), "cadence": cadence, "keys": different})
    for entry in result.values():
        entry["invariant"] = not entry["different"]
    return result


def revert_class(log):
    """(HWI-034) per run of one log that reports them: the revert-class request's tick, the ticks begun after it in
    its frame (0: the frame stopped), the frames stopped and the ticks they held back, and whether every tick the
    clock gave was run or held back (clock_ticks = ticks_run + carried)"""
    result = []
    for (name, cycle, cadence), run in sorted(log["runs"].items(), key=lambda item: (item[0][0], int(item[0][1]))):
        if "clock_ticks" not in run:
            continue
        clock, ran, carried = (int(run[key]) for key in ("clock_ticks", "ticks_run", "carried"))
        result.append({"map": name, "cycle": int(cycle), "cadence": cadence, "requests": run.get("requests"),
                       "request_tick": int(run.get("request_tick", "-1")),
                       "ticks_after_request": int(run.get("ticks_after_request", "0")),
                       "frames_stopped": int(run.get("frames_stopped", "0")),
                       "carried_total": int(run.get("carried_total", "0")),
                       "most_carried": int(run.get("most_carried", "0")),
                       "clock_ticks": clock, "ticks_run": ran, "carried": carried,
                       "conserved": clock == ran + carried})
    return result


def revert_class_pass(entries):
    """(HWI-034) no tick ran after a revert-class request in its frame, and no tick was lost or run twice"""
    return all(entry["ticks_after_request"] == 0 and entry["conserved"] for entry in entries)


def pointer_places(log):
    """(HWI-079) per run of one log that reports them: the pointer places the canonical digest found (the most at a
    tick) and the digests made without them (places_failed: then every address-like word was read as a pointer,
    and the canonical digest can follow the arena's placement)"""
    result = []
    for (name, cycle, cadence), run in sorted(log["runs"].items(), key=lambda item: (item[0][0], int(item[0][1]))):
        if "places_failed" not in run:
            continue
        result.append({"map": name, "cycle": int(cycle), "cadence": cadence, "places": int(run.get("places", "0")),
                       "places_failed": int(run["places_failed"]),
                       "address_values": int(run.get("address_values", "0"))})
    return result


def pointer_places_pass(entries):
    """(HWI-079) every canonical digest found its pointer places"""
    return all(entry["places_failed"] == 0 for entry in entries)


def compare(wii, host):
    report = {"runs": [], "allocations_compared": 0, "allocation_differences": [], "ticks_compared": {},
              "first_divergent_tick": {}, "canonical_only_ticks": {},
              "failures_same": wii["failures"] == host["failures"], "failures": wii["failures"]}
    for key in sorted(set(wii["runs"]) & set(host["runs"])):
        w, h = wii["runs"][key], host["runs"][key]
        simulation = [k for k in RUN_KEYS_SIMULATION if w.get(k) != h.get(k)]
        canonical = [k for k in (*RUN_KEYS_CANONICAL, *RUN_KEYS_WHOLE_CANONICAL) if w.get(k) != h.get(k)]
        whole = [k for k in RUN_KEYS_WHOLE if w.get(k) != h.get(k)]
        report["runs"].append({"map": key[0], "cycle": int(key[1]), "cadence": key[2],
                               "simulation_same": not simulation, "simulation_differences": simulation,
                               "canonical_differences": canonical,
                               "whole_same": not whole, "whole_differences": whole,
                               "frame_paced": key[2] == FRAME_PACED, "digest": w.get("digest"), "sim": w.get("sim")})
    for key in sorted(set(wii["allocations"]) & set(host["allocations"])):
        w, h = wii["allocations"][key], host["allocations"][key]
        report["allocations_compared"] += 1
        different = [k for k in (*RAW_CHECKPOINTS, "bytes", "offset") if w.get(k) != h.get(k)]
        canonical = [k for k in CHECKPOINTS if w.get(k) != h.get(k)]
        if canonical and not different:
            report.setdefault("allocation_canonical_only", []).append(
                {"map": key[0], "cycle": int(key[1]), "cadence": key[2], "name": w.get("name"), "fields": canonical})
        if different:
            report["allocation_differences"].append({"map": key[0], "cycle": int(key[1]), "cadence": key[2],
                                                     "index": int(key[3]), "name": w.get("name"),
                                                     "fields": different})
    for name in sorted(set(wii["ticks_raw"]) & set(host["ticks_raw"])):
        w, h = wii["ticks_raw"][name], host["ticks_raw"][name]
        report["ticks_compared"][name] = min(len(w), len(h))
        report["first_divergent_tick"][name] = None
        for index, (a, b) in enumerate(zip(w, h)):
            if a != b:
                report["first_divergent_tick"][name] = index + 1
                break
        cw, ch = wii["ticks"].get(name, []), host["ticks"].get(name, [])
        report["canonical_only_ticks"][name] = [index + 1 for index, (a, b) in enumerate(zip(cw, ch))
                                                if a != b and w[index] == h[index]]
    report["fixed_step_same"] = bool(wii["fixed"] and host["fixed"] and
                                     wii["fixed"].get("digest") == host["fixed"].get("digest") and
                                     wii["fixed"].get("chain") == host["fixed"].get("chain"))
    unpaced = [run for run in report["runs"] if not run["frame_paced"]]
    report["simulation_all_same"] = bool(report["runs"]) and all(run["simulation_same"] for run in report["runs"])
    report["whole_all_same_unpaced"] = bool(unpaced) and all(run["whole_same"] for run in unpaced)
    report["allocations_all_same_unpaced"] = not [d for d in report["allocation_differences"]
                                                  if d["cadence"] != FRAME_PACED]
    report["ticks_all_same"] = all(tick is None for tick in report["first_divergent_tick"].values())
    report["cadence_invariance_wii"] = cadence_invariance(wii)
    report["cadence_invariance_host"] = cadence_invariance(host)
    report["revert_class_wii"] = revert_class(wii)
    report["revert_class_host"] = revert_class(host)
    report["revert_class_pass"] = (revert_class_pass(report["revert_class_wii"]) and
                                   revert_class_pass(report["revert_class_host"]))
    report["pointer_places_wii"] = pointer_places(wii)
    report["pointer_places_host"] = pointer_places(host)
    report["pointer_places_pass"] = (pointer_places_pass(report["pointer_places_wii"]) and
                                     pointer_places_pass(report["pointer_places_host"]))
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
    for side in ("wii", "host"):
        print(f"cadence invariance ({side}):", {name: (entry["invariant"], entry["runs"], entry["cadences"])
                                                for name, entry in report[f"cadence_invariance_{side}"].items()})
    for side in ("wii", "host"):
        stopped = [entry for entry in report[f"revert_class_{side}"] if entry["frames_stopped"]]
        print(f"revert-class requests ({side}): pass={revert_class_pass(report[f'revert_class_{side}'])} "
              f"frames stopped in {len(stopped)} runs:",
              [(entry["map"], entry["cadence"], entry["request_tick"], entry["carried_total"]) for entry in stopped])
    print("pointer places (HWI-079): pass=" + str(report["pointer_places_pass"]),
          sorted({entry["places"] for side in ("wii", "host") for entry in report[f"pointer_places_{side}"]}))
    print("canonical-only differing ticks:", {k: len(v) for k, v in report["canonical_only_ticks"].items()},
          {k: v[:10] for k, v in report["canonical_only_ticks"].items()})
    for run in report["runs"]:
        print(f"map={run['map']} cycle={run['cycle']} cadence={run['cadence']} simulation_same={run['simulation_same']} "
              f"whole_same={run['whole_same']} {run['simulation_differences'] + run['whole_differences']} "
              f"canonical {run['canonical_differences']}")
    for difference in report["allocation_differences"][:60]:
        print("ALLOC_DIFF", difference)
    invariant = all(entry["invariant"] for side in ("wii", "host")
                    for entry in report[f"cadence_invariance_{side}"].values())
    return 0 if (report["simulation_all_same"] and report["whole_all_same_unpaced"] and report["failures_same"] and
                 invariant and report["revert_class_pass"] and report["pointer_places_pass"]) else 1


if __name__ == "__main__":
    sys.exit(main())
