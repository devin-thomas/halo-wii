"""Compare two ABI_SEMANTICS reports (host stdout and PPC SD report).

CHECK lines are compared by observed value, OBS and MATH lines by value;
the TARGET-specific observations listed below are reported but never
counted as differences. A difference listed in CLASSIFIED is reported as
classified only when it is exactly the measured, explained one (pinned
values and preconditions); every other difference is unexplained. Prints
JSON; never rewrites either report.
Usage: python compare_abi_semantics.py <host-report> <ppc-report> [--json out]
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

# Values that legitimately differ by target (matched as id suffixes, so the
# prefixed candidate pass is covered): pointer width, raw control register,
# and a check whose observed value is a pointer.
TARGET_SPECIFIC = ("target.pointer_bytes", "target.fp_control_at_start", "varargs.mixed.06",
                   "runtime.fp_control_before_start", "runtime.fp_control_after_start")

IN_MEMORY_LAYOUT = {
    "classification": "in_memory_bitfield_layout",
    "reason": "The raw byte of an in-memory bit-field: GCC on PowerPC EABI allocates bit-fields from the high "
              "bit, x86 from the low bit. ADR-018 allows compiler layout for data that never crosses an external "
              "boundary; the field round trips (engine.bitfield.*) are identical contracts.",
}
SIGNALLING_NAN = ("Widening a signalling-NaN float to double: x86 cvtss2sd quiets it (0x7ff8...), PowerPC lfs "
                  "keeps it signalling (0x7ff0...). The payload is otherwise identical.")
# id -> classification, pinned to the measured values (host, ppc) and/or ids
# that must be identical for the explanation to hold.
CLASSIFIED = {
    "engine.layout.players_switch.state_byte_2_12": dict(
        IN_MEMORY_LAYOUT, expected=("0xc2", "0x2c"),
        consumer_impact="none: players_globals->bsp_switch_state is never read or written as a byte (the fields "
                        "are used by name); players globals live in the native game-state image, whose "
                        "cross-target representation is the open HWI-024 save policy, not a wire format."),
    "engine.layout.animation_header.byte_2_5": dict(
        IN_MEMORY_LAYOUT, expected=("0x16", "0x85"),
        consumer_impact="none: recorded_animation_playback.c now decodes the Xbox-authored header byte "
                        "explicitly into this in-memory struct (engine.animation_header.*, engine.playback.*)."),
    "engine.layout.hud_nav_point.unit_bytes": dict(
        IN_MEMORY_LAYOUT, expected=("5d00", "d500"),
        consumer_impact="none: hud_nav_point_datum is a runtime-only datum, never persisted or sent."),
    "float.signaling_nan_widened_bits": {
        "classification": "signalling_nan_widening", "reason": SIGNALLING_NAN,
        "expected": ("0x7ff82468a0000000", "0x7ff02468a0000000"),
        "consumer_impact": "none known: signalling NaNs are not produced by IEEE arithmetic (only by bit "
                           "patterns, e.g. corrupt data); quiet-NaN results and NaN classification agree. Simulation "
                           "that branches on NaN payload bits would differ; no such engine code is known."},
    "musl.atan.f64": {
        "classification": "signalling_nan_widening",
        "reason": SIGNALLING_NAN + " atan returns a NaN argument unchanged, so the corpus's one signalling-NaN "
                                   "case gives a different raw result; the NaN-canonical digest is identical.",
        "requires_identical": ["musl.atan.nan_canonical", "musl.atan.narrowed_f32"],
        "requires_same_inputs": True,
        "consumer_impact": "same as float.signaling_nan_widened_bits"},
}


def parse(path):
    text = Path(path).read_text(encoding="ascii").replace("\r\n", "\n")
    lines = text.splitlines()
    begin = [l for l in lines if l.startswith("BEGIN ")]
    end = [l for l in lines if l.startswith("END ")]
    if len(begin) != 1 or len(end) != 1 or not lines[-1].startswith("END "):
        raise ValueError(f"{path}: report must contain one BEGIN and end with one END line")
    entries = {}
    for line in lines:
        kind = line.split(" ", 1)[0]
        if kind not in ("CHECK", "OBS", "MATH"):
            continue
        key = line.split(" ", 2)[1]
        if key in entries:
            raise ValueError(f"{path}: duplicate id {key}")
        if kind == "CHECK":
            match = re.match(r"CHECK (\S+) class=(\S+) observed=(\"[^\"]*\"|\S+) reference=(\"[^\"]*\"|\S+) result=(pass|fail)$", line)
            if not match:
                raise ValueError(f"{path}: malformed CHECK line: {line}")
            entries[key] = {"kind": kind, "class": match.group(2), "value": match.group(3),
                            "reference": match.group(4), "result": match.group(5)}
        elif kind == "OBS":
            entries[key] = {"kind": kind, "value": line.split(" value=", 1)[1]}
        else:
            fields = dict(item.split("=", 1) for item in line.split(" ")[2:])
            entries[key] = {"kind": kind, "value": fields["results"], "inputs": fields["inputs"],
                            "count": fields["count"], "blocks": fields["blocks"],
                            **{f: int(fields[f]) for f in ("subnormal_inputs", "subnormal_results",
                                                           "zero_results", "nan_results")}}
    summary = next((l for l in lines if " SUMMARY " in l), None)
    return {"sha256": hashlib.sha256(text.encode()).hexdigest(), "begin": begin[0], "end": end[0],
            "summary": summary, "entries": entries}


def classify(key, a, b, host, ppc):
    """The CLASSIFIED entry for this difference, when every pinned condition holds."""
    entry = CLASSIFIED.get(key)
    if entry is None:
        return None
    if "expected" in entry and (a["value"], b["value"]) != entry["expected"]:
        return None
    for other in entry.get("requires_identical", ()):
        x, y = host["entries"].get(other), ppc["entries"].get(other)
        if x is None or y is None or x["value"] != y["value"]:
            return None
    if entry.get("requires_same_inputs") and a.get("inputs") != b.get("inputs"):
        return None
    return {k: v for k, v in entry.items() if k not in ("expected", "requires_same_inputs")}


def compare(host, ppc):
    keys = sorted(set(host["entries"]) | set(ppc["entries"]))
    result = {"host": {k: host[k] for k in ("sha256", "begin", "end", "summary")},
              "ppc": {k: ppc[k] for k in ("sha256", "begin", "end", "summary")},
              "only_host": [], "only_ppc": [], "identical": 0, "differences": [], "target_specific": [],
              "classified": []}
    for key in keys:
        a, b = host["entries"].get(key), ppc["entries"].get(key)
        if a is None or b is None:
            result["only_ppc" if a is None else "only_host"].append(key)
            continue
        if a["value"] == b["value"] and a.get("inputs") == b.get("inputs"):
            result["identical"] += 1
            continue
        difference = {"id": key, "kind": a["kind"], "host": a["value"], "ppc": b["value"]}
        if a["kind"] == "CHECK":
            difference.update({"class": a["class"], "reference": a["reference"],
                               "host_result": a["result"], "ppc_result": b["result"]})
        if a["kind"] == "MATH":
            difference["inputs_identical"] = a["inputs"] == b["inputs"]
            difference["results_identical"] = a["value"] == b["value"]
            blocks_a = [a["blocks"][i:i + 8] for i in range(0, 128, 8)]
            blocks_b = [b["blocks"][i:i + 8] for i in range(0, 128, 8)]
            difference["differing_blocks"] = [i for i in range(16) if blocks_a[i] != blocks_b[i]]
        if a["kind"] == "MATH":
            for field in ("subnormal_inputs", "subnormal_results", "zero_results", "nan_results"):
                difference[field] = {"host": a[field], "ppc": b[field]}
        specific = key.endswith(TARGET_SPECIFIC)
        if specific:
            result["target_specific"].append(difference)
            continue
        explanation = classify(key, a, b, host, ppc)
        if explanation:
            difference.update(explanation)
            result["classified"].append(difference)
        else:
            result["differences"].append(difference)
    by_kind = {}
    for item in result["differences"]:
        by_kind[item["kind"]] = by_kind.get(item["kind"], 0) + 1
    result["difference_counts"] = by_kind
    result["unexplained"] = len(result["differences"])
    result["compared"] = len(keys) - len(result["only_host"]) - len(result["only_ppc"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("ppc")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = compare(parse(args.host), parse(args.ppc))
    text = json.dumps(result, indent=2) + "\n"
    if args.json:
        args.json.write_text(text, encoding="utf-8")
    print(json.dumps({"compared": result["compared"], "identical": result["identical"],
                      "difference_counts": result["difference_counts"], "unexplained": result["unexplained"],
                      "classified": [item["id"] for item in result["classified"]],
                      "only_host": result["only_host"], "only_ppc": result["only_ppc"]}))
    for item in result["differences"]:
        print(item["kind"], item["id"], item.get("differing_blocks", ""), item.get("host_result", ""),
              item.get("ppc_result", ""))


if __name__ == "__main__":
    main()
