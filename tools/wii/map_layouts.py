"""Regenerate tools/wii/map_layouts.json: the byte order of every tag schema definition (HWI-015B).

The Wii reads a map's tags in place as the engine's own structures, in
big-endian byte order; a map's tags are little-endian (Xbox). Converting them
needs every scalar of every structure, not only the pointers, counts and
indices the upstream validator's schema names (cache_schema_tables.c). This
tool asks the compiler for both:

1. tools/wii/map_layout_export.c, built with the schema units and
   tools/wii/map_layout_override.h forced in, prints which C type each schema
   definition is, in cache_schema_tables.c's order.
2. For each type, a translation unit that includes the schema unit naming it
   lays out `struct { <type> value; }`; clang's canonical record-layout dump
   (-fdump-record-layouts-canonical) gives every member's offset and type,
   nested structures and unions expanded; arrays of structures are expanded
   here from their element's own layout.

The result, per definition, is the list of its scalars as runs
[offset, width, count] (width 2, 4 or 8: the bytes swapped together; bytes
are not listed). Unions whose members would swap the same bytes differently,
and bit-fields, are listed separately: a converter must decode them
explicitly (ADR-018), never by host layout.

Like export_tag_schema.py it builds for the upstream i686 ABI with clang in
WSL Debian. The generated file is committed; `--check` regenerates into
memory and fails if it differs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_state_census as census  # noqa: E402

ROOT = census.ROOT
OUTPUT = ROOT / "tools/wii/map_layouts.json"
SCHEMA_UNITS = sorted(path.relative_to(ROOT) for path in (ROOT / "port/linux/game").glob("tag_schema_*.c"))
EXPORTER = Path("tools/wii/map_layout_export.c")
OVERRIDE = Path("tools/wii/map_layout_override.h")
# Structures a converter needs beyond the schema's definitions: the contents
# of tag data the engine reads as structures, and those whose members a
# converter reads by name (unit to include, type spelling). Their members are
# listed by path.
EXTRA_TYPES = [
    ("source/hs/hs.c", "struct data_array"),
    ("source/hs/hs.c", "struct hs_syntax_node"),
    ("source/models/model_animations.c", "struct animation"),
    ("source/models/model_animations.c", "struct compressed_animation_header"),
    ("source/structures/structures.c", "struct structure_material"),
    ("port/linux/game/tag_schema_scenario.c", "struct scenario_structure_bsp_reference"),
]

SCALARS = {
    "char": 1, "signed char": 1, "unsigned char": 1, "_Bool": 1,
    "short": 2, "unsigned short": 2,
    "int": 4, "unsigned int": 4, "long": 4, "unsigned long": 4, "float": 4,
    "long long": 8, "unsigned long long": 8, "double": 8,
}
LINE = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))?\s*\|(\s*)(.*?)\s*$")
ARRAY = re.compile(r"^(.*?)((?:\[\d+\])+)$")
# a nested record with no member name: "struct x::(anonymous at file:line:column)"
ANONYMOUS = re.compile(r"^(?:struct|union) [^ ]*\(anonymous at .*\)$")


def sources_hash(root=ROOT):
    digest = hashlib.sha256()
    paths = [Path("port/linux/game/tag_schema.h"), *SCHEMA_UNITS, EXPORTER, OVERRIDE, Path(__file__).relative_to(ROOT)]
    for path in paths:
        data = (root / path).read_bytes().replace(b"\r\n", b"\n")
        digest.update(path.as_posix().encode() + b"\0" + hashlib.sha256(data).digest())
    return digest.hexdigest()


def flags(root, semantics):
    return ["clang", *[flag for flag in census.UPSTREAM_ABI if flag != "-fsyntax-only"],
            "-include", census.wsl_path(root / "port/linux/include/halo_linux_prefix.h"),
            "-include", census.wsl_path(semantics),
            f"-I{census.wsl_path(root / 'port/linux/include')}", f"-I{census.wsl_path(root / 'port/include/xdk')}",
            f"-I{census.wsl_path(root / 'port/linux/game')}", *census.include_flags(root)]


def wsl(script):
    return subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c", script], capture_output=True, text=True)


def export_types(root, work, base):
    """[(index, size, name, type)] in cache_schema_tables.c's order"""
    units = [census.wsl_path(root / unit) for unit in [*SCHEMA_UNITS, EXPORTER]]
    program = census.wsl_path(work / "map_layout_export")
    command = [*base, "-include", census.wsl_path(root / OVERRIDE)]
    script = (f"set -e; {' '.join(census.repr_sh(flag) for flag in command)} "
              f"{' '.join(census.repr_sh(unit) for unit in units)} -no-pie -static -Wl,--unresolved-symbols=ignore-all "
              f"-o {census.repr_sh(program)}; {census.repr_sh(program)}")
    run = wsl(script)
    if run.returncode != 0:
        raise SystemExit(f"layout export failed ({run.returncode}):\n{run.stderr[-4000:]}")
    result = []
    for line in run.stdout.replace("\r\n", "\n").splitlines():
        parts = line.split(" ", 4)
        if parts[0] != "DEF" or len(parts) != 5:
            raise SystemExit(f"unexpected exporter line: {line!r}")
        result.append((int(parts[1]), int(parts[2]), parts[3], parts[4].strip()))
    return result


def dump_layouts(root, work, base, units_and_types):
    """{record name: [lines]} from clang's canonical record layouts of each unit's types"""
    records = {}
    for number, (unit, types) in enumerate(units_and_types):
        text = f'#include "{census.wsl_path(root / unit)}"\n'
        for index, spelling in types:
            text += (f"struct map_layout_{index} {{ {spelling} value; }};\n"
                     f"char map_layout_size_{index}[sizeof(struct map_layout_{index})];\n")
        source = work / f"layout_{number}.c"
        source.write_text(text, encoding="ascii", newline="\n")
        command = [*base, "-fsyntax-only", "-Xclang", "-fdump-record-layouts-canonical", census.wsl_path(source)]
        run = wsl(" ".join(census.repr_sh(flag) for flag in command))
        if run.returncode != 0:
            raise SystemExit(f"layout dump of {unit} failed:\n{run.stderr[-4000:]}")
        for block in run.stdout.replace("\r\n", "\n").split("*** Dumping AST Record Layout")[1:]:
            lines = [line for line in block.splitlines() if line.strip()]
            header = LINE.match(lines[0])
            name = header.group(5)
            body = lines[1:]
            if name in records and records[name] != body:
                raise SystemExit(f"record {name} has two layouts")
            records[name] = body
    return records


def scalar_width(spelling):
    """bytes of a scalar spelling (pointer, enum or builtin), or None for a record"""
    if ANONYMOUS.match(spelling):
        return None
    if "*" in spelling or "(" in spelling:
        return 4
    if spelling.startswith("enum "):
        return 4
    return SCALARS.get(spelling.replace("const ", "").replace("volatile ", "").strip())


class Flattener:
    """a record's scalars: (offset, width, path), its conflicting unions and its bit-fields"""

    def __init__(self, records):
        self.records = records

    def record(self, name, base, path):
        if name not in self.records:
            raise SystemExit(f"no layout for {name}")
        body = self.records[name]
        entries = []
        for line in body:
            match = LINE.match(line)
            if not match or match.group(5).startswith("[sizeof"):
                continue
            entries.append((int(match.group(1)), match.group(2), match.group(3), len(match.group(4)) - 1,
                            match.group(5)))
        return self.members(entries, 0, len(entries), min((e[3] for e in entries), default=0), base, path,
                            name.startswith("union "))

    def members(self, entries, start, end, depth, base, path, is_union):
        """the members at depth in entries[start:end]: each with its own nested lines"""
        groups = []
        index = start
        while index < end:
            entry = entries[index]
            following = index + 1
            while following < end and entries[following][3] > depth:
                following += 1
            groups.append((entry, index + 1, following))
            index = following
        results = []
        for entry, nested_start, nested_end in groups:
            offset, bit_first, bit_last, _, text = entry
            if ANONYMOUS.match(text):
                spelling, member = text, ""
            else:
                spelling, member = (text.rsplit(" ", 1) + [""])[:2]
            results.append(self.member(entries, nested_start, nested_end, depth, base + offset, f"{path}.{member}",
                                       spelling, bit_first is not None))
        if is_union:
            return self.merge_union(results, path)
        merged = {"scalars": [], "conflicts": [], "bitfields": []}
        for result in results:
            for key in merged:
                merged[key].extend(result[key])
        return merged

    def member(self, entries, nested_start, nested_end, depth, offset, path, spelling, bitfield):
        if bitfield:
            return {"scalars": [], "conflicts": [], "bitfields": [{"offset": offset, "path": path,
                                                                    "type": spelling}]}
        array = ARRAY.match(spelling)
        if array:
            element = array.group(1).strip()
            dims = [int(value) for value in re.findall(r"\[(\d+)\]", array.group(2))]
            count = 1
            for value in dims:
                count *= value
            width = scalar_width(element)
            if width is not None:
                return {"scalars": [(offset + i * width, width, f"{path}[{i}]") for i in range(count)],
                        "conflicts": [], "bitfields": []}
            size = self.record_size(element)
            merged = {"scalars": [], "conflicts": [], "bitfields": []}
            for i in range(count):
                result = self.record(element, offset + i * size, f"{path}[{i}]")
                for key in merged:
                    merged[key].extend(result[key])
            return merged
        width = scalar_width(spelling)
        if width is not None:
            return {"scalars": [(offset, width, path)], "conflicts": [], "bitfields": []}
        if nested_end > nested_start:
            nested_depth = entries[nested_start][3]
            # nested lines carry offsets relative to the outermost record: rebase on this member's offset
            first = entries[nested_start - 1][0]
            rebased = [(e[0] - first, e[1], e[2], e[3], e[4]) for e in entries[nested_start:nested_end]]
            return self.members(rebased, 0, len(rebased), nested_depth, offset, path, spelling.startswith("union "))
        return self.record(spelling, offset, path)

    def record_size(self, name):
        for line in self.records.get(name, []):
            match = re.search(r"\[sizeof=(\d+)", line)
            if match:
                return int(match.group(1))
        raise SystemExit(f"no size for {name}")

    @staticmethod
    def merge_union(results, path):
        """one pattern if every member swaps the bytes it shares alike; else a conflict"""
        patterns = [{(s[0], s[1]) for s in r["scalars"]} for r in results]
        merged = {"scalars": [], "conflicts": [], "bitfields": []}
        for result in results:
            merged["conflicts"].extend(result["conflicts"])
            merged["bitfields"].extend(result["bitfields"])
        byte_owner = {}
        consistent = True
        for pattern in patterns:
            for offset, width in pattern:
                for byte in range(offset, offset + width):
                    if byte in byte_owner and byte_owner[byte] != (offset, width):
                        consistent = False
                    byte_owner.setdefault(byte, (offset, width))
        # a member that leaves bytes unswapped (bytes, chars) where another swaps them conflicts too
        covered = set(byte_owner)
        for result in results:
            span = Flattener.span(result)
            if span:
                own = {b for s in result["scalars"] for b in range(s[0], s[0] + s[1])}
                if any(b in covered and b not in own and b in span for b in range(span[0], span[1])):
                    consistent = False
        if consistent:
            seen = set()
            for result in results:
                for scalar in result["scalars"]:
                    if (scalar[0], scalar[1]) not in seen:
                        seen.add((scalar[0], scalar[1]))
                        merged["scalars"].append(scalar)
        else:
            low = min((s[0] for r in results for s in r["scalars"]), default=0)
            merged["conflicts"].append({"offset": low, "path": path, "members": [
                sorted({(s[0] - low, s[1]) for s in r["scalars"]}) for r in results]})
        return merged

    @staticmethod
    def span(result):
        if not result["scalars"]:
            return None
        return (min(s[0] for s in result["scalars"]), max(s[0] + s[1] for s in result["scalars"]))


def runs(scalars, limit):
    """[offset, width, count] runs of the multi-byte scalars below limit, in order"""
    items = sorted({(offset, width) for offset, width, _ in scalars if width > 1 and offset + width <= limit})
    result = []
    for offset, width in items:
        if result and result[-1][1] == width and result[-1][0] + result[-1][1] * result[-1][2] == offset:
            result[-1][2] += 1
        else:
            result.append([offset, width, 1])
    return result


def generate(root=ROOT, work=None):
    work = work or root / ".local/map-layouts"
    work.mkdir(parents=True, exist_ok=True)
    semantics = census.semantics_header(root, work)
    base = flags(root, semantics)
    definitions = export_types(root, work, base)
    spellings = sorted({spelling for _, _, _, spelling in definitions})
    texts = {unit: (root / unit).read_text(encoding="utf-8") for unit in SCHEMA_UNITS}
    assigned = {}
    for spelling in spellings:
        token = re.compile(r"(?<![\w])" + re.escape(spelling) + r"(?![\w])")
        unit = next((unit for unit in SCHEMA_UNITS if token.search(texts[unit])), None)
        if unit is None:
            raise SystemExit(f"no schema unit names {spelling}")
        assigned[spelling] = unit
    for unit, spelling in EXTRA_TYPES:
        assigned.setdefault(spelling, Path(unit))
    numbering = {spelling: index for index, spelling in enumerate(sorted(assigned))}
    by_unit = {}
    for spelling, unit in assigned.items():
        by_unit.setdefault(unit, []).append((numbering[spelling], spelling))
    records = dump_layouts(root, work, base, sorted((unit, sorted(types)) for unit, types in by_unit.items()))
    flattener = Flattener(records)

    def layout(spelling, limit=None):
        result = flattener.record(f"struct map_layout_{numbering[spelling]}", 0, "")
        size = flattener.record_size(f"struct map_layout_{numbering[spelling]}")
        limit = size if limit is None else limit
        return {"size": size, "swaps": runs(result["scalars"], limit),
                "conflicts": [c for c in result["conflicts"] if c["offset"] < limit],
                "bitfields": [b for b in result["bitfields"] if b["offset"] < limit],
                "members": {path[len(".value."):]: [offset, width] for offset, width, path in result["scalars"]}}

    output = {"generator": "tools/wii/map_layouts.py", "abi": "upstream i686 (Xbox cache layout)",
              "sources_sha256": sources_hash(root), "compiler": census.compiler_identity(),
              "definitions": [], "types": {}}
    for index, size, name, spelling in definitions:
        item = layout(spelling, size)
        output["definitions"].append({"index": index, "name": name, "size": size, "type": spelling,
                                      "type_size": item["size"], "swaps": item["swaps"],
                                      "conflicts": item["conflicts"], "bitfields": item["bitfields"]})
    # (members only for the extra types: the definitions' would be most of the file)
    for _, spelling in EXTRA_TYPES:
        output["types"][spelling] = layout(spelling)
    for item in output["definitions"]:
        item.pop("members", None)
    return json.dumps(output, indent=1, sort_keys=False) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = generate()
    if args.check:
        same = OUTPUT.exists() and OUTPUT.read_text(encoding="utf-8").replace("\r\n", "\n") == text
        print("map layouts current" if same else "map layouts differ")
        return 0 if same else 1
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT.relative_to(ROOT).as_posix()} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
