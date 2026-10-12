#!/usr/bin/env python3
"""Compare the Wii's sound with the i686 host reference's, and its decoded PCM with the host decoder (HWI-074).

- Each run's SOUND_RUN line (port/wii/sound/sound_hooks.c) from the Wii's
  engine.log and the host reference's: everything the mixer decides
  (frames mixed, voices, packets, blocks decoded, stream starts, ends and
  stops, the CRC of every mixed frame and of the stream events in order) must
  be equal; the timings (mix_us, mix_peak_us, pack_us) are each machine's own
  and only reported.
- Each SOUND_CLASS line (a sound class's starts, ends and stops, and the ticks
  of its first and last) must be equal.
- Each SOUND_DECODE line (a permutation's decoded PCM CRC, logged as the Wii
  read it from the sound pack) must equal the host reference decoder's
  (media_formats.decode_xbox_adpcm, through sound_stage.decoded_crc) for the
  same permutation of the same pack.

    python tools/wii/compare_sound_runs.py --wii <engine.log> [--host <engine.log>] [--packs <dir> ...] [--json <out>]
"""

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sound_stage as ss  # noqa: E402

DETERMINISTIC = ("frames", "clock_ms", "work_calls", "frames_mixed", "skipped_frames", "voices_max", "voice_frames",
                 "voices_by_calls", "sounds_peak", "looping_sounds_peak", "streams", "packets_queued",
                 "packets_completed", "packets_flushed", "blocks_decoded", "starts", "ends", "stops",
                 "obstruction_tests", "marks_unrestored", "notifications_withheld", "mouth_withheld", "indices_withheld", "clipped", "mix_crc", "events_crc", "pack_reads", "pack_bytes", "pack_pcm",
                 "pack_failures", "decode_checks")
TIMINGS = ("mix_us", "mix_peak_us", "pack_us")

# the engine's sound classes (sound_classes.h; the Halo Editing Kit's names)
SOUND_CLASSES = {
    0: "projectile_impact", 1: "projectile_detonation", 4: "weapon_fire", 5: "weapon_ready", 6: "weapon_reload",
    7: "weapon_empty", 8: "weapon_charge", 9: "weapon_overheat", 10: "weapon_idle", 13: "object_impacts",
    14: "particle_impacts", 15: "slow_particle_impacts", 18: "unit_footsteps", 19: "unit_dialog",
    22: "vehicle_collision", 23: "vehicle_engine", 26: "device_door", 27: "device_force_field",
    28: "device_machinery", 29: "device_nature", 30: "device_computers", 32: "music", 33: "ambient_nature",
    34: "ambient_machinery", 35: "ambient_computers", 39: "first_person_damage", 44: "scripted_dialog_player",
    45: "scripted_effect", 46: "scripted_dialog_other", 47: "scripted_dialog_force_unspatialized", 50: "game_event",
}

FIELD = re.compile(r'(\w+)=("[^"]*"|\S+)')


def fields(line):
    return {key: value.strip('"') for key, value in FIELD.findall(line)}


def parse(path):
    """{"runs": {(map, cycle, cadence): fields}, "classes": {...}, "decodes": [fields],
    "kinds": {...}, "outputs": {...}, "launches": n}"""
    text = Path(path).read_text(encoding="ascii", errors="replace")
    result = {"runs": {}, "classes": {}, "decodes": [], "kinds": {}, "outputs": {}, "digests": {}}
    for line in text.splitlines():
        kind = line.split(" ", 1)[0]
        if kind not in ("SOUND_RUN", "SOUND_CLASS", "SOUND_DECODE", "SOUND_KIND", "SOUND_OUTPUT", "RUN"):
            continue
        values = fields(line)
        if kind == "SOUND_DECODE":
            result["decodes"].append(values)
            continue
        key = (values.get("map"), values.get("cycle"), values.get("cadence"))
        if kind == "SOUND_RUN":
            result["runs"][key] = values
        elif kind == "SOUND_CLASS":
            result["classes"][key + (values["class"],)] = values
        elif kind == "SOUND_KIND":
            result["kinds"][key] = values
        elif kind == "SOUND_OUTPUT":
            result["outputs"][key] = values
        elif kind == "RUN":
            result["digests"][key] = {name: values.get(name) for name in ("sim", "sim_raw", "raw", "digest",
                                                                           "sim_hwi015b", "first_divergent_tick")}
    return result


def compare_runs(wii, host):
    rows, mismatches = [], 0
    for key in sorted(set(wii["runs"]) | set(host["runs"]), key=str):
        a, b = wii["runs"].get(key), host["runs"].get(key)
        if a is None or b is None:
            mismatches += 1
            rows.append({"run": list(key), "missing": "wii" if a is None else "host"})
            continue
        different = [name for name in DETERMINISTIC if a.get(name) != b.get(name)]
        digest_a, digest_b = wii["digests"].get(key, {}), host["digests"].get(key, {})
        digests_differ = [name for name in ("sim", "sim_raw", "raw", "digest") if digest_a.get(name) != digest_b.get(name)]
        mismatches += bool(different) + bool(digests_differ)
        rows.append({"run": list(key), "equal": not different and not digests_differ, "different": different,
                     "digests_different": digests_differ, "mix_crc": a.get("mix_crc"),
                     "events_crc": a.get("events_crc"), "sim": digest_a.get("sim"), "whole": digest_a.get("digest"),
                     "wii_timings": {name: a.get(name) for name in TIMINGS},
                     "host_timings": {name: b.get(name) for name in TIMINGS}})
    for key in sorted(set(wii["classes"]) | set(host["classes"]), key=str):
        a, b = wii["classes"].get(key), host["classes"].get(key)
        if a != b:
            mismatches += 1
            rows.append({"class": list(key), "wii": a, "host": b})
    return rows, mismatches


def check_decodes(decodes, pack_dirs):
    """each logged permutation against the host reference decoder"""
    packs, results, mismatches = {}, {}, 0
    for line in decodes:
        name, offset = line["map"], int(line["offset"], 16)
        if (name, offset) in results:
            if results[(name, offset)]["wii_crc"] != line["crc"]:
                mismatches += 1
            continue
        if name not in packs:
            for directory in pack_dirs:
                path = Path(directory) / f"{name}.wsnd"
                if path.is_file():
                    _info, entries = ss.read_pack(path)
                    packs[name] = {entry["offset"]: entry for entry in entries}
                    break
            else:
                packs[name] = None
        entry = packs[name].get(offset) if packs[name] else None
        if entry is None:
            results[(name, offset)] = {"wii_crc": line["crc"], "host_crc": None, "equal": False}
            mismatches += 1
            continue
        frames, crc = ss.decoded_crc(entry["container"])
        equal = f"{crc:08x}" == line["crc"] and str(frames) == line["frames"]
        mismatches += not equal
        results[(name, offset)] = {"wii_crc": line["crc"], "host_crc": f"{crc:08x}", "frames": frames, "equal": equal}
    return results, mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wii", type=Path, required=True)
    parser.add_argument("--host", type=Path)
    parser.add_argument("--packs", type=Path, nargs="*", default=[])
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    wii = parse(args.wii)
    summary = {"runs": len(wii["runs"]), "decodes_logged": len(wii["decodes"])}
    failures = 0
    if args.host:
        host = parse(args.host)
        rows, mismatches = compare_runs(wii, host)
        summary.update(run_rows=rows, run_mismatches=mismatches)
        failures += mismatches
    if args.packs:
        results, mismatches = check_decodes(wii["decodes"], args.packs)
        summary.update(permutations_checked=len(results), decode_mismatches=mismatches,
                       frames_checked=sum(result.get("frames") or 0 for result in results.values()))
        failures += mismatches
    classes = {}
    for key, values in wii["classes"].items():
        name = SOUND_CLASSES.get(int(values["class"]), "class_" + values["class"])
        classes.setdefault("/".join(map(str, key[:3])), {})[name] = {
            item: values[item] for item in ("starts", "ends", "stops", "first_tick", "last_tick")}
    summary["classes"] = classes
    summary["result"] = "pass" if not failures else "fail"
    text = json.dumps(summary, indent=1)
    if args.json:
        args.json.write_text(text + "\n", encoding="utf-8", newline="\n")
    print(f"runs={summary['runs']} run_mismatches={summary.get('run_mismatches')} "
          f"permutations_checked={summary.get('permutations_checked')} decode_mismatches={summary.get('decode_mismatches')} "
          f"result={summary['result']}")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
