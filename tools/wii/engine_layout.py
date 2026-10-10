#!/usr/bin/env python3
"""Engine structure layouts on PowerPC against the upstream i686 ABI (HWI-015).

Two censuses:

game-state  Every game-state allocation site of the engine
            (tools/wii/game_state_census.py SITES and FIXED) evaluated by the
            Wii engine compiler with the Wii engine's flags: count and
            element size, each the compiler's own sizeof of the engine's
            structure. They are compared with the i686 census committed in
            tools/wii/game_state_census_table.c (HWI-007), which the
            pointer-free game-state image and the saved games rest on.
            Nothing is executed: the values are read from a data section
            (big-endian words).

dwarf       Every named structure and union described in the debug
            information of two builds (a `readelf --debug-dump=info` listing
            of each): its size and every member's offset, bit offset and
            bit size. Run it on the Wii engine (build/wii/engine.elf) and on
            the same units compiled for i686 with the Linux build's flags.
            Each type present in both is classified: identical; differing
            only in bit-field allocation within the same bytes (ADR-018: in
            memory only; never at an external boundary); or differing in
            size or member offsets.
"""

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/wii"))
sys.path.insert(0, str(ROOT))

from game_state_census import FIXED, SITES, translation_unit  # noqa: E402

TABLE_ROW = re.compile(r'^\s*\{"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\},', re.M)


# ---------- game-state census on PowerPC

def i686_rows(table: Path):
    """[(label, count, element size, bytes)] from game_state_census_table.c"""
    return [(label, int(count), int(size), int(total))
            for label, _, count, size, total, _ in TABLE_ROW.findall(table.read_text(encoding="utf-8"))]


def ppc_values(compiler: str, flags, work: Path):
    """{("site"|"fixed", index): [values]} evaluated by the PowerPC compiler"""
    work.mkdir(parents=True, exist_ok=True)
    groups = {}
    for index, site in enumerate(SITES):
        groups.setdefault(site[1], []).append((("site", index), [site[3], site[4]]))
    for index, (_, source, expression) in enumerate(FIXED):
        groups.setdefault(source, []).append((("fixed", index), [expression]))
    objcopy = str(Path(compiler).with_name(Path(compiler).name.replace("gcc", "objcopy")))
    values = {}
    for number, (source, entries) in enumerate(sorted(groups.items())):
        keys, expressions = [], []
        for key, items in entries:
            for item in items:
                keys.append(key)
                expressions.append(item)
        unit = work / f"layout_{number}.c"
        unit.write_text(translation_unit((ROOT / source).as_posix(), number, expressions), encoding="ascii",
                        newline="\n")
        obj, blob = work / f"layout_{number}.o", work / f"layout_{number}.bin"
        subprocess.run([compiler, *flags, "-c", str(unit), "-o", str(obj)], cwd=ROOT, check=True)
        subprocess.run([objcopy, "-O", "binary", "--only-section=.hwi_census", str(obj), str(blob)], check=True)
        data = blob.read_bytes()
        if len(data) != 4 * len(expressions):
            raise ValueError(f"{unit.name}: {len(data)} bytes for {len(expressions)} values")
        for key, offset in zip(keys, range(0, len(data), 4)):
            values.setdefault(key, []).append(int.from_bytes(data[offset:offset + 4], "big", signed=True))
    return values


def compare_game_state(values, rows):
    """each census row (label order) against the PowerPC count and size"""
    by_label = defaultdict(list)
    for index, site in enumerate(SITES):
        count, size = values[("site", index)]
        for _ in range(site[5]):
            by_label[site[0]].append((count, size))
    result, differences = [], []
    seen = defaultdict(int)
    for label, count, size, total in rows:
        occurrence = seen[label]
        seen[label] += 1
        ppc = by_label.get(label, [])
        ppc_count, ppc_size = ppc[occurrence] if occurrence < len(ppc) else (None, None)
        same = (ppc_count, ppc_size) == (count, size)
        result.append({"label": label, "count": count, "i686_size": size, "ppc_size": ppc_size,
                       "ppc_count": ppc_count, "same": same})
        if not same:
            differences.append(label)
    fixed = {label: values[("fixed", index)][0] for index, (label, _, _) in enumerate(FIXED)}
    return result, differences, fixed


# ---------- DWARF census

DIE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)")
ATTRIBUTE = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")


def attribute_value(text: str):
    text = text.strip()
    match = re.search(r":\s*([^:]+)$", text) if text.startswith("(") else None
    if match:
        text = match.group(1).strip()
    match = re.match(r"(?:0x)?([0-9a-fA-F]+)", text)
    if text.startswith("0x"):
        return int(text, 16)
    return int(text) if re.fullmatch(r"-?\d+", text) else text


def dwarf_layouts(listing):
    """{(compile unit, tag, name): set of layouts} from a readelf
    --debug-dump=info listing (an iterable of lines); a layout is (size,
    ((member, offset, bit offset, bit size), ...)). Keyed by compile unit:
    the same name can be defined differently in different units, and the
    compilers describe different subsets of a unit's types (clang only those
    the unit uses), so only a unit's own definitions are compared."""
    layouts = defaultdict(set)
    current = None      # [tag, name, size, declaration, members, depth]
    member = None
    unit = ""
    in_unit_header = False

    def finish():
        if current and current[1] and current[2] is not None and not current[3]:
            layouts[(unit, current[0], current[1])].add((current[2], tuple(current[4])))

    for line in listing:
        die = DIE.match(line)
        if die:
            depth, tag = int(die.group(1)), die.group(3)
            in_unit_header = tag == "DW_TAG_compile_unit"
            if member is not None:
                current[4].append(tuple(member))
                member = None
            if current is not None and depth <= current[5]:
                finish()
                current = None
            if tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                current = ["struct" if tag == "DW_TAG_structure_type" else "union", None, None, False, [], depth]
            elif tag == "DW_TAG_member" and current is not None and depth == current[5] + 1:
                member = [None, 0, None, None]
            continue
        attribute = ATTRIBUTE.match(line)
        if attribute and in_unit_header and attribute.group(1) == "DW_AT_name":
            name = attribute.group(2).split(": ")[-1].strip()
            match = re.search(r"(?:source|port|tools)/.*$", name.replace("\\", "/"))
            unit = match.group(0) if match else name
            continue
        if not attribute or current is None:
            continue
        name, value = attribute.group(1), attribute.group(2)
        target = member if member is not None else None
        if name == "DW_AT_name":
            text = value.split(":")[-1].strip() if value.startswith("(") else value.strip()
            if target is not None:
                target[0] = text
            else:
                current[1] = text
        elif name == "DW_AT_byte_size" and target is None:
            current[2] = attribute_value(value)
        elif name == "DW_AT_declaration" and target is None:
            current[3] = True
        elif target is not None and name == "DW_AT_data_member_location":
            target[1] = attribute_value(value)
        elif target is not None and name in ("DW_AT_data_bit_offset", "DW_AT_bit_offset"):
            target[2] = attribute_value(value)
        elif target is not None and name == "DW_AT_bit_size":
            target[3] = attribute_value(value)
    if member is not None and current is not None:
        current[4].append(tuple(member))
    finish()
    return layouts


def classify(ppc, host):
    """identical / bitfield_order / different, for one type's layouts"""
    if ppc == host:
        return "identical"

    def bytes_only(layouts):
        result = set()
        for size, members in layouts:
            stripped = []
            for name, offset, bit_offset, bit_size in members:
                if bit_size is not None and bit_offset is not None:
                    # the byte span a bit-field occupies on either ABI is its
                    # storage unit; compare the members' order and the size
                    stripped.append((name, "bitfield", bit_size))
                else:
                    stripped.append((name, offset, None))
            result.add((size, tuple(stripped)))
        return result

    if bytes_only(ppc) == bytes_only(host):
        return "bitfield_order"
    return "different"


# the C library's own types (newlib on the Wii, glibc on the host): never
# engine data, persisted or shared
C_LIBRARY_TYPES = {"tm", "_stat", "stat", "timeval", "timespec", "_reent", "__sFILE", "_IO_FILE", "dirent", "DIR"}


def compare_dwarf(ppc_layouts, host_layouts):
    common = sorted(set(ppc_layouts) & set(host_layouts))
    counts = defaultdict(int)
    details = []
    for key in common:
        kind = classify(ppc_layouts[key], host_layouts[key])
        if kind != "identical" and key[2] in C_LIBRARY_TYPES:
            kind = "c_library"
        counts[kind] += 1
        if kind != "identical":
            details.append({"unit": key[0], "type": f"{key[1]} {key[2]}", "class": kind,
                            "ppc": sorted((size, len(members)) for size, members in ppc_layouts[key]),
                            "i686": sorted((size, len(members)) for size, members in host_layouts[key])})
    names = {key[1:] for key in common}
    return {"unit_types_in_both": len(common), "distinct_types_in_both": len(names), "ppc_only": len(set(ppc_layouts) - set(host_layouts)),
            "i686_only": len(set(host_layouts) - set(ppc_layouts)), "classes": dict(counts), "differences": details}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    game = sub.add_parser("game-state")
    game.add_argument("--cc", required=True, help="powerpc-eabi-gcc")
    game.add_argument("--flags", type=Path, default=ROOT / "build/wii/engine/flags-engine.json")
    game.add_argument("--libogc-include", required=True)
    game.add_argument("--work", type=Path, required=True)
    game.add_argument("--output", type=Path)
    dwarf = sub.add_parser("dwarf")
    dwarf.add_argument("ppc_listing", type=Path)
    dwarf.add_argument("i686_listing", type=Path)
    dwarf.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.command == "game-state":
        flags = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float", "-O2",
                 *json.loads(args.flags.read_text(encoding="utf-8")), "-I", args.libogc_include]
        values = ppc_values(args.cc, flags, args.work)
        rows, differences, fixed = compare_game_state(values, i686_rows(ROOT / "tools/wii/game_state_census_table.c"))
        report = {"rows": len(rows), "differences": differences, "ppc_fixed": fixed,
                  "i686_bytes": sum(row["count"] * row["i686_size"] for row in rows),
                  "ppc_bytes": sum((row["ppc_count"] or 0) * (row["ppc_size"] or 0) for row in rows),
                  "per_row": rows}
    else:
        with args.ppc_listing.open(encoding="utf-8", errors="replace") as ppc, \
                args.i686_listing.open(encoding="utf-8", errors="replace") as host:
            report = compare_dwarf(dwarf_layouts(ppc), dwarf_layouts(host))
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8", newline="\n")
    print(json.dumps({key: value for key, value in report.items() if key not in ("per_row", "differences")}))
    print("differences:", report["differences"][:40] if isinstance(report["differences"], list) else "")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
