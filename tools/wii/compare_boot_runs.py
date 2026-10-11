#!/usr/bin/env python3
"""Compare a boot-mode launch on the Wii with its i686 host replay (HWI-033).

Both engine.logs carry the same lines for the same frames
(port/wii/render/render_ui.c): BOOT_SCREEN (a screen's game-state digests
once it has stood), BOOT_ENVIRONMENT first/end (the map the menus loaded),
BOOT_TICK (the whole state after each of that map's first ticks) and
BOOT_STATE_ALLOCATION (each allocation's digest at a checkpoint). This tool
pairs them and reports which are equal. Digests and counts only.

    python tools/wii/compare_boot_runs.py <Wii engine.log> <host engine.log> [--json <out>]
"""
import argparse
import json
import re
import sys

FIELD = re.compile(r"(\w+)=(\"[^\"]*\"|\S+)")


def fields(line):
    return dict(FIELD.findall(line))


def parse(text):
    """the log's comparable lines"""
    out = {"screens": {}, "environment": {}, "ticks": {}, "allocations": {}, "maps": [], "end": None}
    for line in text.splitlines():
        if line.startswith("BOOT_SCREEN "):
            d = fields(line)
            out["screens"][int(d["frame"])] = {"state": d.get("state"),
                                               "without_scripts": d.get("state_without_scripts"),
                                               "screen_tag": d.get("screen_tag")}
        elif line.startswith("BOOT_ENVIRONMENT first") or line.startswith("BOOT_ENVIRONMENT end"):
            d = fields(line)
            out["environment"][line.split()[1]] = d
        elif line.startswith("BOOT_TICK "):
            d = fields(line)
            out["ticks"][int(d["tick"])] = {"state": d.get("state"), "without_scripts": d.get("state_without_scripts"),
                                            "game_time": d.get("game_time"), "frame": d.get("frame"),
                                            "bsp": d.get("bsp")}
        elif line.startswith("BOOT_STATE_ALLOCATION "):
            d = fields(line)
            name = line.split(" name=", 1)[1] if " name=" in line else None  # (the rest of the line: it has spaces)
            out["allocations"].setdefault(d["at"], {})[int(d["index"])] = (name, d.get("digest"))
        elif line.startswith("BOOT_MAP "):
            d = fields(line)
            out["maps"].append({"frame": d.get("frame"), "map": d.get("map"), "kind": d.get("kind")})
        elif line.startswith("END "):
            out["end"] = fields(line)
    return out


def compare(wii, host):
    frames = sorted(set(wii["screens"]) | set(host["screens"]))
    screens = []
    for frame in frames:
        a, b = wii["screens"].get(frame), host["screens"].get(frame)
        screens.append({"frame": frame, "both": bool(a and b),
                        "state_equal": bool(a and b and a["state"] == b["state"]),
                        "without_scripts_equal": bool(a and b and a["without_scripts"] == b["without_scripts"]),
                        "state": a["state"] if a else None})
    ticks = sorted(set(wii["ticks"]) | set(host["ticks"]))
    tick_rows = [(t, wii["ticks"].get(t), host["ticks"].get(t)) for t in ticks]
    unequal_ticks = [t for t, a, b in tick_rows if not (a and b and a["state"] == b["state"]
                                                         and a["game_time"] == b["game_time"])]
    environment = {}
    for key in ("first", "end"):
        a, b = wii["environment"].get(key), host["environment"].get(key)
        environment[key] = {"present": bool(a and b), "equal": bool(a and b and a.get("state") == b.get("state")
                                                                    and a.get("frame") == b.get("frame")),
                            "state": a.get("state") if a else None, "frame": a.get("frame") if a else None,
                            "drawing": a.get("drawing") if a else None}
    allocations = {}
    for at, rows in wii["allocations"].items():
        other = host["allocations"].get(at, {})
        allocations[at] = {"count": len(rows), "equal": sum(1 for i, v in rows.items() if other.get(i) == v),
                           "differing": sorted(v[0] for i, v in rows.items() if other.get(i) != v)}
    return {
        "screens": len(screens), "screens_both": sum(s["both"] for s in screens),
        "screens_state_equal": sum(s["state_equal"] for s in screens),
        "screens_without_scripts_equal": sum(s["without_scripts_equal"] for s in screens),
        "screen_rows": screens,
        "ticks": len(ticks), "ticks_equal": len(ticks) - len(unequal_ticks),
        "first_unequal_tick": unequal_ticks[0] if unequal_ticks else None,
        "first_tick": tick_rows[0][1] if tick_rows and tick_rows[0][1] else None,
        "last_tick": tick_rows[-1][1] if tick_rows and tick_rows[-1][1] else None,
        "environment": environment, "allocations": allocations,
        "wii_maps": wii["maps"], "wii_end": wii["end"], "host_end": host["end"],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wii_log")
    parser.add_argument("host_log")
    parser.add_argument("--json", help="write the whole comparison here")
    args = parser.parse_args(argv)
    with open(args.wii_log, encoding="ascii", errors="replace") as stream:
        wii = parse(stream.read())
    with open(args.host_log, encoding="ascii", errors="replace") as stream:
        host = parse(stream.read())
    result = compare(wii, host)
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(result, stream, indent=1)
    summary = {k: result[k] for k in ("screens", "screens_both", "screens_state_equal",
                                       "screens_without_scripts_equal", "ticks", "ticks_equal", "first_unequal_tick")}
    summary["environment"] = {k: v["equal"] for k, v in result["environment"].items()}
    summary["allocations"] = {k: f"{v['equal']}/{v['count']}" for k, v in result["allocations"].items()}
    print(json.dumps(summary))
    equal = (result["screens"] == result["screens_state_equal"] and result["ticks"] == result["ticks_equal"]
             and all(v["equal"] for v in result["environment"].values() if v["present"]))
    return 0 if equal else 1


if __name__ == "__main__":
    sys.exit(main())
