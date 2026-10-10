#!/usr/bin/env python3
"""Measure the linked Wii engine image in MEM1 from its GNU ld map (HWI-015).

Reads build/wii/engine.map and reports, as JSON:
- the output sections (code, read-only data, data, small data, BSS) and
  where the image ends in MEM1;
- every input section's bytes grouped by kind and by owner: the engine
  units (source/, port/linux/game), the Wii platform layer, the reused Linux
  platform files, third-party code and the SDK's libraries;
- the engine's subsystems (source/<dir>);
- the largest objects and the largest single allocations (with -fdata-sections
  each static variable has its own section, named after it);
- named groups whose storage ADR-015's record singled out (Custom Edition
  map support, the tag validator, netcode, profiling, AI debug).

The map is the link's own record, so this is the image as linked: what
--gc-sections dropped is not counted, and nothing is estimated.
"""

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

MEM1_END = 0x81800000
OUTPUT_SECTIONS = (".init", ".text", ".rodata", ".sdata2", ".eh_frame_hdr", ".eh_frame", ".ctors", ".dtors",
                   ".data", ".sdata", ".sbss", ".bss")
KIND = {".init": "code", ".text": "code", ".rodata": "rodata", ".sdata2": "rodata", ".eh_frame_hdr": "rodata",
        ".eh_frame": "rodata", ".ctors": "data", ".dtors": "data", ".data": "data", ".sdata": "data",
        ".sbss": "bss", ".bss": "bss"}

# groups named by ADR-015's record (2026-10-10) and the HWI-015 ticket; a
# symbol or object belongs to the first group whose pattern matches it
GROUPS = (
    ("custom_edition_map_support", re.compile(r"custom_edition")),
    ("tag_validator", re.compile(r"tag_validate")),
    ("ai_debug", re.compile(r"^ai_debug$|ai_debug\.o$|^actor_debug|^actor_path_debug")),
    ("profiling", re.compile(r"^profile_|profile\.o$|_profile\.o$")),
    ("netcode", re.compile(r"network_|/networking/|bungie_net|transport|distributed|late_joiner|host_sent|"
                           r"client_|objects_host|objects_client|update_(server|client)_globals|lobby|p2p")),
)

SECTION_LINE = re.compile(r"^ (\.\S+|COMMON)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*))?$")
CONTINUATION = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
OUTPUT_LINE = re.compile(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
SYMBOL_LINE = re.compile(r"^\s+0x([0-9a-f]+)\s+([A-Za-z_.$][\w.$]*)\s*$")


def owner(path: str) -> str:
    path = path.replace("\\", "/")
    if "/opt/devkitpro" in path or path.startswith("/") or ".a(" in path:
        return "sdk_libraries"
    if "/obj/source/" in path or "/obj/port/linux/game/" in path:
        return "engine"
    if "/obj/port/wii/engine/engine_hooks" in path or "/obj/port/wii/engine/fixed_step_scenario" in path:
        return "engine_authored"
    if "/obj/port/wii/" in path or "/obj/tools/wii/" in path:
        return "wii_platform"
    if "/obj/port/linux/src/" in path:
        return "linux_platform_reused"
    if "/obj/port/third_party/" in path:
        return "third_party"
    return "other"


def subsystem(path: str) -> str:
    match = re.search(r"/obj/(source/[^/]+|port/linux/game|port/wii/engine|port/linux/src|port/third_party/[^/]+)",
                      path.replace("\\", "/"))
    return match.group(1) if match else owner(path)


def parse_map(text: str):
    """(output sections {name: (address, size)}, input sections
    [(output, input name, address, size, object, symbols)])"""
    outputs, inputs = {}, []
    current_output = None
    pending = None
    last = None
    for line in text.splitlines():
        match = OUTPUT_LINE.match(line)
        if match and not line.startswith(" "):
            name = match.group(1)
            current_output = name if name in OUTPUT_SECTIONS else None
            if current_output:
                outputs[name] = (int(match.group(2), 16), int(match.group(3), 16))
            pending = last = None
            continue
        if not line.startswith(" "):
            current_output = None if line.strip() and not line.startswith(".") else current_output
            pending = last = None
            continue
        if current_output is None:
            continue
        if pending is not None:
            match = CONTINUATION.match(line)
            if match:
                last = [current_output, pending, int(match.group(1), 16), int(match.group(2), 16),
                        match.group(3).strip(), []]
                inputs.append(last)
            pending = None
            continue
        match = SECTION_LINE.match(line)
        if match:
            if match.group(2) is None:
                pending = match.group(1)
                last = None
            else:
                last = [current_output, match.group(1), int(match.group(2), 16), int(match.group(3), 16),
                        match.group(4).strip(), []]
                inputs.append(last)
            continue
        match = SYMBOL_LINE.match(line)
        if match and last is not None:
            last[5].append((int(match.group(1), 16), match.group(2)))
    return outputs, inputs


def allocation_name(input_name: str, symbols) -> str:
    """the static variable (or function) a section holds: -fdata-sections
    names it .bss.<name> / .data.<name>; COMMON lists its symbols"""
    for prefix in (".bss.", ".sbss.", ".data.", ".sdata.", ".rodata.", ".text."):
        if input_name.startswith(prefix):
            return input_name[len(prefix):]
    if symbols:
        return symbols[0][1]
    return input_name


def group_of(name: str, obj: str) -> str:
    for group, pattern in GROUPS:
        if pattern.search(name) or pattern.search(obj.replace("\\", "/")):
            return group
    return ""


def measure(text: str) -> dict:
    outputs, inputs = parse_map(text)
    by_kind = defaultdict(int)
    by_owner = defaultdict(lambda: defaultdict(int))
    by_subsystem = defaultdict(lambda: defaultdict(int))
    by_object = defaultdict(lambda: defaultdict(int))
    allocations = []
    groups = defaultdict(lambda: defaultdict(int))
    for output, name, address, size, obj, symbols in inputs:
        if not size:
            continue
        kind = KIND[output]
        by_kind[kind] += size
        by_owner[owner(obj)][kind] += size
        by_subsystem[subsystem(obj)][kind] += size
        short = re.sub(r"^.*?/obj/", "", obj.replace("\\", "/"))
        by_object[short][kind] += size
        allocation = allocation_name(name, symbols)
        group = group_of(allocation, obj)
        if group:
            groups[group][kind] += size
        if kind in ("bss", "data"):
            allocations.append({"name": allocation, "object": short, "kind": kind, "bytes": size, "group": group})
    end = max(address + size for address, size in outputs.values())
    start = min(address for address, _ in outputs.values())
    allocations.sort(key=lambda item: (-item["bytes"], item["name"]))
    largest_objects = sorted(by_object.items(), key=lambda item: -sum(item[1].values()))[:40]
    return {
        "output_sections": {name: {"address": f"0x{address:08x}", "bytes": size}
                            for name, (address, size) in sorted(outputs.items(), key=lambda item: item[1][0])},
        "image_start": f"0x{start:08x}",
        "image_end": f"0x{end:08x}",
        "image_bytes": end - start,
        "mem1_after_image": MEM1_END - end,
        "bytes_by_kind": dict(sorted(by_kind.items())),
        "bytes_by_owner": {key: dict(value) for key, value in sorted(by_owner.items())},
        "bytes_by_subsystem": {key: dict(value) for key, value in
                               sorted(by_subsystem.items(), key=lambda item: -sum(item[1].values()))},
        "named_groups": {key: dict(value) for key, value in sorted(groups.items())},
        "largest_objects": [{"object": name, **dict(value)} for name, value in largest_objects],
        "largest_allocations": allocations[:60],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("map", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = measure(args.map.read_text(encoding="utf-8", errors="replace"))
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
