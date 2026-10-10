"""Prepare one owned Halo map for the Wii engine build's SD card (HWI-015B).

The Wii engine reads a map's tags in place as its own structures. An Xbox map
holds them little-endian, with pointers that are Xbox addresses in a fixed
22 MiB tag slot (0x803a6000). This tool, run on the host at import time,
writes the map as the Wii's cache layer loads it (port/wii/engine/
cache_files_wii.c):

- Byte order: every scalar the engine reads is put in the target's byte
  order, by explicit decoders, never by the host's layout:
  - each tag root, block element and structure BSP root by its schema
    definition's full layout (tools/wii/map_layouts.json, the compiler's
    view of the engine's own structures);
  - the tag header, tag table, BSP header and vertex/index buffer tables by
    their fixed formats;
  - each tag data field by what the engine reads it as (DATA_RULES): bytes,
    16-bit text, 32-bit bit vectors, BSP vertices, animation frames
    (uncompressed and compressed), script syntax nodes.
  Bytes no schema field claims (Xbox vertex and index payloads, which only
  the unsupported renderer reads) are left as they are and counted.
- Relocation (ADR-015): the HWI-007 walker (cache_schema_graph.c, through
  tools/wii/map_stage_walk.c) names every pointer field the game reads, as
  it relocates them on the Wii. Each holds its target's offset in the slot,
  and the file lists them; the loader adds the slot's run-time base. The
  fields the walker nulls are zero.

Output (private: owned data): <out>/<map>.wmap (big-endian, the Wii's) and
<out>/<map>.le.wmap (little-endian, for the i686 host reference), which
differ only in byte order, and <out>/<map>.stage.json, a report of counts and
hashes with no map content.

File format "HWIMAP01", every number in the file's own byte order:
    0x00  char[8] "HWIMAP01"
    0x08  u32 0x01020304 (in the file's order: tells the order)
    0x0c  u32 segment count
    0x10  u32 header bytes (0x800): the engine's cache_file_header, converted
    0x14  u32 offset of that header in this file
    0x18  u32 source map file length (the header's file_length)
    0x1c  u32 reserved (0)
    0x20  segments, 32 bytes each:
          u32 kind (1 tag data, 2 structure BSP)
          u32 map file offset it stands for, u32 bytes
          u32 slot offset it loads to (tag data 0; a BSP its load address's)
          u32 offset in this file of its bytes
          u32 relocation count, u32 offset in this file of its relocations
            (u32 slot offsets of the words that hold slot offsets)
          u32 reserved (0)

usage: python tools/wii/map_stage.py --map <owned .map> --output <private dir> [--walker <built map_stage_walk>]
       [--cc <native C compiler: no WSL>]
"""
import argparse
from array import array
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import halo_cache  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TABLES = ROOT / "tools/wii/cache_schema_tables.c"
LAYOUTS = ROOT / "tools/wii/map_layouts.json"
WALKER_SOURCES = ["tools/wii/map_stage_walk.c", "tools/wii/cache_schema_graph.c", "tools/wii/cache_schema_tables.c"]
TAG_BASE = 0x803A6000
SLOT_BYTES = 0x01600000
MAGIC = b"HWIMAP01"
FORMAT_VERSION = 1
SEGMENT_TAGS, SEGMENT_BSP = 1, 2
SCENARIO_BSP_BLOCK = 0x5A4          # struct scenario structure_bsp_references (asserted in tag_schema_scenario.c)
HEADER_BYTES = 0x800
SBSP = 0x73627370

# schema field types (cache_schema_tables.h)
F_BLOCK, F_DATA, F_FILE_DATA = 0, 1, 2


class StageError(ValueError):
    """A refused map; the text names no input bytes, tag names or paths."""


# ---------- the schema and layouts

class Schema:
    def __init__(self, text):
        self.fields = [tuple(map(int, m)) for m in re.findall(
            r"^\t\{(-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+)\},$", text, re.M)]
        self.definitions = [(int(size), int(first), int(count), name) for size, first, count, name in re.findall(
            r"^\t\{(-?\d+), (-?\d+), (-?\d+)\}, /\* (\w+) \*/$", text, re.M)]
        self.groups = {int(tag, 16): int(definition) for tag, _, _, definition in re.findall(
            r"\{(0x[0-9a-f]+)u, \{(0x[0-9a-f]+)u, (0x[0-9a-f]+)u\}, (-?\d+)\}", text)}
        # the pointer fields a check reads (model part and bsp material buffers, bsp load addresses)
        self.pointers = [(int(definition), int(offset)) for definition, offset in re.findall(
            r"^\t\{(\d+), (\d+)\}, /\* \w+ \*/$", text, re.M)]
        if not self.fields or not self.definitions or not self.groups:
            raise StageError("schema tables unreadable")

    def field_definition(self, field_id):
        for index, (_, first, count, _) in enumerate(self.definitions):
            if first <= field_id < first + count:
                return index
        raise StageError("field outside the schema")


def load_layouts(schema, path=LAYOUTS):
    layouts = json.loads(path.read_text(encoding="utf-8"))
    for item, (size, _, _, name) in zip(layouts["definitions"], schema.definitions):
        if item["name"] != name or item["size"] != size:
            raise StageError("map_layouts.json does not match cache_schema_tables.c")
        if item["conflicts"] or item["bitfields"]:
            raise StageError("a definition needs an explicit decoder the stager does not have")
    if len(layouts["definitions"]) != len(schema.definitions):
        raise StageError("map_layouts.json does not match cache_schema_tables.c")
    return layouts


def member(layouts, type_name, path):
    return layouts["types"][type_name]["members"][path][0]


# ---------- the plan: which bytes swap together

class Plan:
    """Byte-order plan of the slot: runs of (offset, width, count); each byte is planned once."""

    def __init__(self, size):
        self.owner = bytearray(size)   # 1 where a byte is planned
        self.runs = []
        self.words = set()             # offsets of planned 4-byte scalars (pointer fields must be one)
        self.halfwords = set()         # (offset, 2) of planned 2-byte scalars
        self.counts = {}

    def add(self, offset, width, count, what):
        span = width * count
        if span <= 0:
            return
        if offset < 0 or offset + span > len(self.owner):
            raise StageError(f"{what}: outside the slot")
        if self.owner[offset:offset + span].count(0) != span:
            raise StageError(f"{what}: bytes planned twice")
        self.owner[offset:offset + span] = b"\x01" * span
        if width > 1:
            self.runs.append((offset, width, count))
        if width == 4:
            self.words.update(range(offset, offset + span, 4))
        elif width == 2 and what == "model_indices":
            self.halfwords.update((at, 2) for at in range(offset, offset + span, 2))
        self.counts[what] = self.counts.get(what, 0) + span

    def bytes_(self, offset, count, what):
        self.add(offset, 1, count, what)

    def apply(self, data):
        """data swapped to the other byte order in place, run by run"""
        for offset, width, count in self.runs:
            end = offset + width * count
            if width == 8:
                values = array("Q")
            elif width == 4:
                values = array("I")
            else:
                values = array("H")
            values.frombytes(bytes(data[offset:end]))
            values.byteswap()
            data[offset:end] = values.tobytes()


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def s16(data, offset):
    return struct.unpack_from("<h", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def s32(data, offset):
    return struct.unpack_from("<i", data, offset)[0]


F_STRUCT = 9


def definition_widths(schema, layouts, definition, cache):
    """a definition's width code per byte (1 a byte, 2/4/8 a scalar's first byte, 0 its other bytes):
    its C type's layout, refined by the schema's inline structures (_tag_schema_struct) in it, whose
    definitions may name a part the C type leaves as reserved bytes (a contrail's shader is a
    shader_effect to the schema). A refinement may only turn bytes into scalars; any other
    disagreement refuses the map."""
    if definition in cache:
        return cache[definition]
    item = layouts["definitions"][definition]
    widths = bytearray(b"" * item["size"])
    for offset, width, count in item["swaps"]:
        for index in range(count):
            at = offset + index * width
            widths[at:at + width] = bytes([width]) + bytes(width - 1)
    _, first, count, _ = schema.definitions[definition]
    for field_id in range(first, first + count):
        kind, _, size, repeat, offset = schema.fields[field_id][:5]
        if kind != F_STRUCT:
            continue
        inner = definition_widths(schema, layouts, schema.fields[field_id][8], cache)
        for index in range(repeat):
            at = offset + index * size
            span = widths[at:at + len(inner)]
            if span == inner:
                continue
            for position in range(len(inner)):
                outer_code, inner_code = span[position], inner[position]
                if outer_code == inner_code:
                    continue
                # the outer part must be bytes over the whole of the inner scalar
                if inner_code > 1 and all(widths[at + position + k] == 1 for k in range(inner_code)):
                    continue
                if inner_code == 0 or inner_code == 1:
                    # (the inside of a scalar the inner one starts, checked above; or bytes over bytes)
                    if outer_code == 1:
                        continue
                raise StageError("a schema structure disagrees with its container's layout")
            widths[at:at + len(inner)] = inner
    cache[definition] = bytes(widths)
    return cache[definition]


def width_runs(widths):
    """(offset, width, count) runs of a width-code string"""
    runs = []
    position = 0
    size = len(widths)
    while position < size:
        code = widths[position]
        if code <= 1:
            position += 1
            continue
        start, count = position, 0
        while position < size and widths[position] == code:
            count += 1
            position += code
        runs.append((start, code, count))
    return runs


def add_definition(plan, schema, layouts, offset, definition, cache, run_cache):
    if definition not in run_cache:
        widths = definition_widths(schema, layouts, definition, cache)
        run_cache[definition] = (width_runs(widths), len(widths))
    runs, size = run_cache[definition]
    for run_offset, width, count in runs:
        plan.add(offset + run_offset, width, count, "elements")
    fill_bytes(plan, offset, size, "elements")


# ---------- tag data: what the engine reads each field as

def rule_bytes(plan, slot, layouts, offset, size, element):
    plan.bytes_(offset, size, "data_bytes")


def rule_words16(plan, slot, layouts, offset, size, element):
    plan.add(offset, 2, size // 2, "data_text16")
    plan.bytes_(offset + size // 2 * 2, size % 2, "data_text16")


def rule_words32(plan, slot, layouts, offset, size, element):
    plan.add(offset, 4, size // 4, "data_words32")
    plan.bytes_(offset + size // 4 * 4, size % 4, "data_words32")


def rule_material_compressed(plan, slot, layouts, offset, size, element):
    """structure_material compressed_vertex_data: its vertices (struct environment_vertex_compressed: three
    floats, three packed normals, two floats: 32 bytes, all 32-bit), then its lightmap vertices
    (struct environment_lightmap_vertex_compressed: a packed color, two shorts)"""
    vertices = s32(slot, element + member(layouts, "struct structure_material", "vertices.count"))
    lightmap = s32(slot, element + member(layouts, "struct structure_material", "lightmap_vertices.count"))
    if vertices < 0 or lightmap < 0 or vertices * 32 + lightmap * 8 > size:
        raise StageError("compressed bsp vertices do not fit their data")
    plan.add(offset, 4, vertices * 8, "data_bsp_vertices")
    at = offset + vertices * 32
    for index in range(lightmap):
        plan.add(at + index * 8, 4, 1, "data_bsp_vertices")
        plan.add(at + index * 8 + 4, 2, 2, "data_bsp_vertices")
    rest = size - vertices * 32 - lightmap * 8
    plan.bytes_(offset + size - rest, rest, "data_bsp_vertices")


def rule_material_uncompressed(plan, slot, layouts, offset, size, element):
    """structure_material uncompressed_vertex_data: floats only (positions, normals, texture coordinates)"""
    rule_words32(plan, slot, layouts, offset, size, element)


def animation_fields(slot, layouts, element):
    def get(path, reader):
        return reader(slot, element + member(layouts, "struct animation", path))
    return {
        "frame_count": get("frame_count", s16), "frame_size": get("frame_size", s16),
        "node_count": get("node_count", s16), "flags": get("flags", u16),
        "compressed_data_offset": get("compressed_data_offset", s32),
        "rotation": [get("nodes_with_rotation_flags[0]", u32), get("nodes_with_rotation_flags[1]", u32)],
        "translation": [get("nodes_with_translation_flags[0]", u32), get("nodes_with_translation_flags[1]", u32)],
        "scale": [get("nodes_with_scale_flags[0]", u32), get("nodes_with_scale_flags[1]", u32)],
    }


def node_flag(flags, node):
    return (flags[node >> 5] >> (node & 31)) & 1


def rule_animation_default(plan, slot, layouts, offset, size, element):
    """animation default_data: for each node, what its frames do not animate: rotation (four shorts),
    translation (three floats), scale (a float), as animation_get_node_orientations reads them"""
    a = animation_fields(slot, layouts, element)
    at = offset
    for node in range(max(a["node_count"], 0)):
        if not node_flag(a["rotation"], node):
            plan.add(at, 2, 4, "data_animation")
            at += 8
        if not node_flag(a["translation"], node):
            plan.add(at, 4, 3, "data_animation")
            at += 12
        if not node_flag(a["scale"], node):
            plan.add(at, 4, 1, "data_animation")
            at += 4
        if at > offset + size:
            raise StageError("animation default data shorter than its nodes")
    plan.bytes_(at, offset + size - at, "data_animation")


COMPRESSED_REGIONS = [  # (header member, element width) of a compressed animation's parts
    ("rotation_keyframe_frame_indices_offset", 2), ("default_rotations_offset", 2), ("rotation_keyframes_offset", 2),
    ("translation_node_headers_offset", 4), ("translation_keyframe_frame_indices_offset", 2),
    ("default_translations_offset", 4), ("translation_keyframes_offset", 4), ("scale_node_headers_offset", 4),
    ("scale_keyframe_frame_indices_offset", 2), ("default_scales_offset", 4), ("scale_keyframes_offset", 4)]


def rule_animation_data(plan, slot, layouts, offset, size, element):
    """animation data: uncompressed frames (per node, as the flags say: rotation four shorts, translation
    three floats, scale a float), and, for a compressed animation, its compressed part from
    compressed_data_offset: a header of 32-bit offsets and node headers, then parts of 16-bit keyframe
    frame indices and 6-byte quaternions (three 16-bit words), 32-bit node headers, and floats"""
    a = animation_fields(slot, layouts, element)
    compressed = a["flags"] & 1
    frames_end = size
    if compressed:
        frames_end = a["compressed_data_offset"]
        if not 0 <= frames_end <= size:
            raise StageError("compressed animation offset outside its data")
    frame = []
    for node in range(max(a["node_count"], 0)):
        if node_flag(a["rotation"], node):
            frame.append((2, 4))
        if node_flag(a["translation"], node):
            frame.append((4, 3))
        if node_flag(a["scale"], node):
            frame.append((4, 1))
    frame_bytes = sum(width * count for width, count in frame)
    frames = 0
    if frame_bytes and a["frame_size"] == frame_bytes and a["frame_count"] > 0:
        frames = min(a["frame_count"], frames_end // frame_bytes)
    elif frames_end and not compressed:
        raise StageError("animation frame size disagrees with its nodes")
    at = offset
    for _ in range(frames):
        for width, count in frame:
            plan.add(at, width, count, "data_animation")
            at += width * count
    plan.bytes_(at, offset + frames_end - at, "data_animation")
    if not compressed:
        return
    base = offset + frames_end
    available = size - frames_end
    header_end = 44
    parts = []
    for name, width in COMPRESSED_REGIONS:
        part = s32(slot, base + member(layouts, "struct compressed_animation_header", name))
        parts.append((part, width))
    first_part = min(part for part, _ in parts)
    if available < header_end or not header_end <= first_part <= available:
        raise StageError("compressed animation header outside its data")
    # the header's offsets, then its rotation node headers (32-bit) up to the first part
    plan.add(base, 4, 11, "data_animation")
    plan.add(base + 44, 4, (first_part - 44) // 4, "data_animation")
    plan.bytes_(base + 44 + (first_part - 44) // 4 * 4, (first_part - 44) % 4, "data_animation")
    ordered = sorted(parts)
    for index, (part, width) in enumerate(ordered):
        end = ordered[index + 1][0] if index + 1 < len(ordered) else available
        if not part <= end <= available:
            raise StageError("compressed animation parts out of order")
        count = (end - part) // width
        plan.add(base + part, width, count, "data_animation")
        plan.bytes_(base + part + count * width, end - part - count * width, "data_animation")


def rule_syntax_data(plan, slot, layouts, offset, size, element):
    """scenario hs_syntax_data: a struct data_array, then its struct hs_syntax_node elements. A node's
    last word is a union whose member the compiler wrote as the node's type says (hs_compile.c): a
    constant boolean's byte, a constant short's short, a constant real's float, otherwise a long
    (a node index, a global index, a long). Constants of types from string on are parsed again from
    their source as the map loads (hs_compile_postprocess), and written natively."""
    header = layouts["types"]["struct data_array"]
    node = layouts["types"]["struct hs_syntax_node"]
    if size < header["size"]:
        raise StageError("script data shorter than its header")
    for run_offset, width, count in header["swaps"]:
        plan.add(offset + run_offset, width, count, "data_script")
    fill_bytes(plan, offset, header["size"], "data_script")
    maximum = s16(slot, offset + member(layouts, "struct data_array", "maximum_count"))
    element_size = s16(slot, offset + member(layouts, "struct data_array", "size"))
    if element_size != node["size"] or header["size"] + maximum * element_size > size:
        raise StageError("script data's array is not of script nodes")
    flags_at = member(layouts, "struct hs_syntax_node", "flags")
    type_at = member(layouts, "struct hs_syntax_node", "type")
    value_at = 16
    for index in range(maximum):
        at = offset + header["size"] + index * element_size
        for run_offset, width, count in node["swaps"]:
            plan.add(at + run_offset, width, count, "data_script")
        node_type, flags = s16(slot, at + type_at), u16(slot, at + flags_at)
        primitive = flags & 1 and not flags & 4
        if primitive and node_type == 1:          # _hs_type_boolean: boolean_value
            plan.bytes_(at + value_at, 4, "data_script")
        elif primitive and node_type == 3:        # _hs_type_short_integer: short_value
            plan.add(at + value_at, 2, 1, "data_script")
            plan.bytes_(at + value_at + 2, 2, "data_script")
        else:                                     # real_value, or the long data
            plan.add(at + value_at, 4, 1, "data_script")
    rest = size - header["size"] - maximum * element_size
    plan.bytes_(offset + size - rest, rest, "data_script")


def fill_bytes(plan, offset, size, what):
    position, end = offset, offset + size
    while position < end:
        if plan.owner[position]:
            position += 1
            continue
        start = position
        while position < end and not plan.owner[position]:
            position += 1
        plan.bytes_(start, position - start, what)


# (definition name, field ordinal among its data fields) -> rule
DATA_RULES = {
    ("animation", 0): rule_words32,                 # frame_info: dx, dy[, dz], dyaw floats
    ("animation", 1): rule_animation_default,       # default_data
    ("animation", 2): rule_animation_data,          # data
    ("structure_bsp", 0): rule_words32,             # cluster_data: unsigned long bit vectors
    ("structure_bsp", 1): rule_bytes,               # sound_cluster_data: a byte per cluster pair
    ("structure_material", 0): rule_material_uncompressed,
    ("structure_material", 1): rule_material_compressed,
    ("sound_permutation", 0): rule_bytes,           # mouth_data: a byte per tick
    ("sound_permutation", 1): rule_bytes,           # subtitle_data
    ("hud_message_text", 0): rule_words16,          # text_data: 16-bit text
    ("font", 0): rule_bytes,                        # pixels
    ("string_list_entry", 0): rule_bytes,           # string: 8-bit text
    ("unicode_string_list_entry", 0): rule_words16,  # string: 16-bit text
    ("scenario", 0): rule_syntax_data,              # hs_syntax_data
    ("scenario", 1): rule_bytes,                    # hs_string_constants: 8-bit text
    ("recorded_animation", 0): rule_bytes,          # event_stream: read a byte at a time (unconverted values)
    ("hs_source_file", 0): rule_bytes,              # source: text
}


def model_part_indices(plan, slot, schema, element, definition):
    """a model part's triangle indices (16-bit), which the validator's model_geometry_part_check reads to
    count them against its vertices (and the renderer draws): its triangle buffer
    (struct triangle_buffer: short type, word pad, long count, void *base_address, void *hardware_format,
    whose last two the schema's supplementary pointers name) names its index buffer descriptor in the tag
    header's table, whose Data is the indices. Its count of indices is the check's: 3 a triangle, or a
    strip's count plus 2. Returns the indices planned"""
    offsets = sorted(offset for owner, offset in schema.pointers if owner == definition)
    hardware_format = offsets[1]           # triangle_buffer.hardware_format
    buffer_type = s16(slot, element + hardware_format - 12)
    count = s32(slot, element + hardware_format - 8)
    descriptor = u32(slot, element + hardware_format)
    if descriptor < TAG_BASE or count <= 0:
        return 0
    index_count = 3 * count if buffer_type == 0 else count + 2 if buffer_type == 1 else 0
    data = u32(slot, descriptor - TAG_BASE + 4)
    if not index_count or data < TAG_BASE:
        return 0
    at = data - TAG_BASE
    span = plan.owner[at:at + 2 * index_count]
    if span.count(0) == len(span):
        plan.add(at, 2, index_count, "model_indices")
        return index_count
    if span.count(1) == len(span) and all((at + 2 * index, 2) in plan.halfwords for index in range(index_count)):
        return 0                            # (indices another part shares, planned already)
    raise StageError("a model part's indices overlap other planned bytes")


def data_rule(schema, field_id):
    definition = schema.field_definition(field_id)
    _, first, count, name = schema.definitions[definition]
    ordinal = [f for f in range(first, first + count) if schema.fields[f][0] == F_DATA].index(field_id)
    rule = DATA_RULES.get((name, ordinal))
    if rule is None:
        raise StageError("a tag data field has no conversion rule")
    return name, ordinal, schema.fields[field_id][4], rule


# ---------- the walk (HWI-007's walker, built for the host)

def build_walker(work, cc=None):
    """map_stage_walk built with WSL Debian's clang, or the given native compiler (the walker is portable
    C11); returns (program, runs natively)"""
    import game_state_census as census
    work.mkdir(parents=True, exist_ok=True)
    program = work / "map_stage_walk"
    if cc:
        run = subprocess.run([cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", str(program),
                              *(str(ROOT / source) for source in WALKER_SOURCES)], capture_output=True, text=True)
        if run.returncode != 0:
            raise StageError(f"walker build failed: {run.stderr[-2000:]}")
        return program, True
    sources = " ".join(census.repr_sh(census.wsl_path(ROOT / source)) for source in WALKER_SOURCES)
    script = (f"clang -O2 -std=c11 -Wall -Wextra -Werror -o {census.repr_sh(census.wsl_path(program))} {sources}")
    run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c", script], capture_output=True, text=True)
    if run.returncode != 0:
        raise StageError(f"walker build failed: {run.stderr[-2000:]}")
    return program, False


def run_walker(walker, work, slot, tag_bytes, bsp):
    """walker: (program, runs natively)"""
    import game_state_census as census
    program, native = walker
    image = work / "slot.bin"
    records_path = work / "records.bin"
    end = bsp["slot_offset"] + bsp["bytes"] if bsp else tag_bytes
    image.write_bytes(bytes(slot[:end]))
    path = (lambda value: str(value)) if native else census.wsl_path
    command = [path(program), path(image), str(tag_bytes),
               str(bsp["slot_offset"] if bsp else 0), str(bsp["bytes"] if bsp else 0),
               str(bsp["tag_index"] if bsp else 0), path(records_path)]
    run = subprocess.run(command if native else ["wsl", "-d", "Debian", "--", *command], capture_output=True,
                         text=True)
    image.unlink()
    if run.returncode != 0:
        raise StageError(f"walk refused the map: {run.stdout.strip()[:200]}")
    data = records_path.read_bytes()
    records_path.unlink()
    records = [struct.unpack_from("<5I", data, at) for at in range(0, len(data), 20)]
    report = [line for line in run.stdout.splitlines() if line.startswith("GRAPH ")]
    return records, report


# ---------- the map

def read_map(path):
    with open(path, "rb") as stream:
        header, body, facts = halo_cache.inflate_stream(stream)
    return header, body


def structure_bsps(tags):
    instances = u32(tags, 0) - TAG_BASE
    scenario = u32(tags, 4)
    root = u32(tags, instances + (scenario & 0xFFFF) * 32 + 20) - TAG_BASE
    count = s32(tags, root + SCENARIO_BSP_BLOCK)
    address = u32(tags, root + SCENARIO_BSP_BLOCK + 4) - TAG_BASE
    result = []
    for index in range(count):
        element = address + index * 32
        result.append({"file_offset": s32(tags, element), "bytes": s32(tags, element + 4),
                       "slot_offset": u32(tags, element + 8) - TAG_BASE, "tag_index": u32(tags, element + 28)})
    return result


def engine_header(source, big_endian):
    """the engine's struct cache_file_header (cache_files.c) from the Xbox header, field by field"""
    order = ">" if big_endian else "<"
    out = bytearray(source[:HEADER_BYTES])
    for offset in (0x0, 0x4, 0x8, 0x10, 0x14, 0x64, 0x7FC):   # signatures, version, lengths, checksum
        struct.pack_into(order + "I", out, offset, u32(source, offset))
    # (0x60 is the scenario type, a short in the reserved bytes the Xbox cache files read)
    struct.pack_into(order + "H", out, 0x60, u16(source, 0x60))
    return bytes(out)


def stage(map_path, walker, work, report_only=False):
    started = time.perf_counter()
    schema = Schema(TABLES.read_text(encoding="utf-8"))
    layouts = load_layouts(schema)
    header, body = read_map(map_path)
    tag_offset, tag_bytes = header["tag_offset"], header["tag_bytes"]
    tags = bytes(body[tag_offset:tag_offset + tag_bytes])
    bsps = structure_bsps(tags)
    if not bsps:
        raise StageError("the scenario has no structure bsp")
    slot = bytearray(SLOT_BYTES)
    slot[:tag_bytes] = tags
    walks = []
    for bsp in bsps:
        if not (tag_bytes <= bsp["slot_offset"] and bsp["slot_offset"] + bsp["bytes"] <= SLOT_BYTES and
                0 <= bsp["file_offset"] <= len(body) - bsp["bytes"]):
            raise StageError("a structure bsp does not fit the slot")
        slot[bsp["slot_offset"]:bsp["slot_offset"] + bsp["bytes"]] = body[bsp["file_offset"]:
                                                                            bsp["file_offset"] + bsp["bytes"]]
        records, graph = run_walker(walker, work, slot, tag_bytes, bsp)
        walks.append((bsp, records, graph))
        if len(bsps) > 1:  # (each bsp loads alone, over the others' bytes)
            raise StageError("maps with more than one structure bsp are not staged yet")

    plan = Plan(SLOT_BYTES)
    # the tag header and tag table
    plan.add(0, 4, 9, "header")
    count = s32(tags, 12)
    instances = u32(tags, 0) - TAG_BASE
    plan.add(instances, 4, 8 * count, "tag_table")
    for count_at, pointer_at in ((16, 20), (24, 28)):
        buffers = s32(tags, count_at)
        if buffers:
            plan.add(u32(tags, pointer_at) - TAG_BASE, 4, 3 * buffers, "buffers")
    bsp, records, graph = walks[0]
    at = bsp["slot_offset"]
    plan.add(at, 4, 6, "bsp_header")
    for count_at, pointer_at in ((4, 8), (12, 16)):
        buffers = s32(slot, at + count_at)
        if buffers:
            plan.add(u32(slot, at + pointer_at) - TAG_BASE, 4, 3 * buffers, "buffers")

    elements = data_fields = 0
    rules_used = {}
    pointers, nulls = [], []
    cache, run_cache = {}, {}
    for kind, a, b, c, d in records:
        if kind == 1:
            add_definition(plan, schema, layouts, a, b, cache, run_cache)
            elements += 1
    for kind, a, b, c, d in records:
        if kind == 2:
            name, ordinal, field_offset, rule = data_rule(schema, c)
            rule(plan, slot, layouts, a, b, d - field_offset)
            key = f"{name}.{ordinal}:{rule.__name__}"
            rules_used[key] = rules_used.get(key, 0) + 1
            data_fields += 1
        elif kind == 3:
            pointers.append((a, b))
        elif kind == 4:
            nulls.append(a)
    model_part = next(index for index, item in enumerate(schema.definitions) if item[3] == "model_geometry_part")
    model_indices = sum(model_part_indices(plan, slot, schema, a, b) for kind, a, b, c, d in records
                        if kind == 1 and b == model_part)
    for offset, _ in pointers:
        if offset not in plan.words:
            raise StageError("a pointer field is not a planned 32-bit word")
    for offset in nulls:
        if offset not in plan.words:
            raise StageError("a nulled field is not a planned 32-bit word")

    # roots of groups the schema has no definition for: their bytes are not converted
    unconverted_groups = {}
    for ordinal in range(count):
        group = u32(tags, instances + ordinal * 32)
        if group != SBSP and schema.groups.get(group, -1) < 0:
            name = struct.pack(">I", group).decode("latin-1")
            unconverted_groups[name] = unconverted_groups.get(name, 0) + 1

    loaded = [(0, tag_bytes)] + [(b["slot_offset"], b["bytes"]) for b, _, _ in walks]
    unplanned = sum(size - plan.owner[start:start + size].count(1) for start, size in loaded)

    # pointer words hold their target's slot offset; nulled ones hold zero
    for offset, target in pointers:
        struct.pack_into("<I", slot, offset, target)
    for offset in nulls:
        struct.pack_into("<I", slot, offset, 0)
    relocations = sorted(offset for offset, _ in pointers)

    little = bytes(slot)
    plan.apply(slot)
    big = bytes(slot)
    # the conversion only reorders bytes inside scalars: each loaded range holds the same bytes in both orders
    for start, size in loaded:
        if sorted(little[start:start + size]) != sorted(big[start:start + size]):
            raise StageError("the conversion changed a loaded range's bytes, not only their order")
    outputs = {}
    for big_endian, image in ((True, big), (False, little)):
        outputs["be" if big_endian else "le"] = write_wmap(header_bytes=body[:HEADER_BYTES], image=image,
                                                           tag_bytes=tag_bytes, tag_offset=tag_offset,
                                                           bsps=[b for b, _, _ in walks], relocations=relocations,
                                                           file_length=header["declared_bytes"],
                                                           big_endian=big_endian)
    report = {
        "format": "HWIMAP01", "format_version": FORMAT_VERSION,
        "map": {"name": header["name"], "build": header["build"], "category": header["category"],
                "declared_bytes": header["declared_bytes"], "tag_bytes": tag_bytes, "tags": count,
                "bsps": [{"bytes": b["bytes"], "slot_offset": b["slot_offset"]} for b, _, _ in walks]},
        "walk": walks[0][2],
        "elements": elements, "data_fields": data_fields, "model_indices": model_indices, "data_rules": dict(sorted(rules_used.items())),
        "relocations": len(relocations), "nulled": len(nulls),
        "planned_bytes": dict(sorted(plan.counts.items())),
        "swap_runs": len(plan.runs),
        "unplanned_loaded_bytes": unplanned,
        "byte_multisets_equal": True,
        "unconverted_groups": unconverted_groups,
        "outputs": {key: {"bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}
                    for key, value in outputs.items()},
        "seconds": round(time.perf_counter() - started, 1),
    }
    return report, outputs


def write_wmap(header_bytes, image, tag_bytes, tag_offset, bsps, relocations, file_length, big_endian):
    order = ">" if big_endian else "<"
    segments = [(SEGMENT_TAGS, tag_offset, tag_bytes, 0)]
    segments += [(SEGMENT_BSP, b["file_offset"], b["bytes"], b["slot_offset"]) for b in bsps]
    table_end = 0x20 + 32 * len(segments)
    header_at = (table_end + 31) & ~31
    position = header_at + HEADER_BYTES
    blobs = []
    entries = []
    for kind, file_offset, size, slot_offset in segments:
        data_at = (position + 31) & ~31
        position = data_at + size
        own = [r for r in relocations if slot_offset <= r < slot_offset + size]
        relocation_at = (position + 3) & ~3
        position = relocation_at + 4 * len(own)
        entries.append((kind, file_offset, size, slot_offset, data_at, len(own), relocation_at, 0))
        blobs.append((data_at, image[slot_offset:slot_offset + size]))
        blobs.append((relocation_at, struct.pack(f"{order}{len(own)}I", *own)))
    out = bytearray(position)
    out[0:8] = MAGIC
    struct.pack_into(order + "IIIIII", out, 8, 0x01020304, len(segments), HEADER_BYTES, header_at, file_length, 0)
    for index, entry in enumerate(entries):
        struct.pack_into(order + "8I", out, 0x20 + 32 * index, *entry)
    out[header_at:header_at + HEADER_BYTES] = engine_header(header_bytes, big_endian)
    for at, blob in blobs:
        out[at:at + len(blob)] = blob
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--map", type=Path, required=True, help="an owned Xbox .map (private)")
    parser.add_argument("--output", type=Path, required=True, help="a private directory")
    parser.add_argument("--walker", type=Path, help="a built map_stage_walk to run in WSL (built in --output otherwise)")
    parser.add_argument("--cc", help="build and run the walker natively with this compiler (no WSL)")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = out / "work"
    work.mkdir(exist_ok=True)
    walker = (args.walker, False) if args.walker else build_walker(work, args.cc)
    try:
        report, outputs = stage(args.map, walker, work)
    except (StageError, halo_cache.CacheError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 1
    stem = args.map.stem
    for key, data in outputs.items():
        target = out / (f"{stem}.wmap" if key == "be" else f"{stem}.le.wmap")
        partial = target.with_suffix(target.suffix + ".partial")
        partial.write_bytes(data)
        os.replace(partial, target)
    (out / f"{stem}.stage.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({key: report[key] for key in ("elements", "data_fields", "relocations", "nulled",
                                                   "unplanned_loaded_bytes", "unconverted_groups", "seconds")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
