"""Prepare one owned Halo map for the Wii engine build's SD card (HWI-015B, HWI-016C).

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
    (uncompressed and compressed), script syntax nodes (each constant by its
    type's cell: a boolean's byte, a short cell's short), recorded animation
    event streams (by their own layout).
  Bytes no schema field claims are left as they are, counted by kind
  (Xbox vertex and index payloads, which only the unsupported renderer
  reads; roots of tags whose group has no schema definition; anything else,
  which is reported).
- Structure BSPs: a campaign scenario has several, each loading alone to its
  own place at the top of the slot, over the others' bytes. The tag data is
  walked with each in turn (its records must be the same every time), and
  each BSP is converted and relocated as its own segment.
- Relocation (ADR-015): the HWI-007 walker (cache_schema_graph.c, through
  tools/wii/map_stage_walk.c) names every pointer field the game reads, as
  it relocates them on the Wii. Each holds its target's offset in the slot,
  and the file lists them; the loader adds the slot's run-time base. The
  fields the walker nulls are zero.

Output (private: owned data): <out>/<map>.wmap (big-endian, the Wii's) and
<out>/<map>.le.wmap (little-endian, for the i686 host reference), which
differ only in byte order; <out>/manifest.txt and <out>/manifest.le.txt,
listing each staged map of the directory (the card's maps directory holds
manifest.txt beside the maps); and <out>/<map>.stage.json, a report of
counts and hashes with no map content.

File format "HWIMAP02", every number in the file's own byte order:
    0x00  char[8] "HWIMAP02" (a file of another version is refused by name)
    0x08  u32 0x01020304 (in the file's order: tells the order)
    0x0c  u32 segment count (1 + the scenario's structure BSPs, at most 17)
    0x10  u32 header bytes (0x800): the engine's cache_file_header, converted
    0x14  u32 offset of that header in this file
    0x18  u32 source map file length (the header's file_length)
    0x1c  u32 this file's length
    0x20  u32 CRC of the header's 0x800 bytes
    0x24  u32 CRC of the table (bytes 0x00 up to the segments' end, this word
          taken as 0): the staging digest the manifest lists
    0x28  u32 format version (2)
    0x2c  reserved (0) up to 0x40
    0x40  segments, 32 bytes each:
          u32 kind (1 tag data, 2 structure BSP)
          u32 map file offset it stands for, u32 bytes
          u32 slot offset it loads to (tag data 0; a BSP its load address's)
          u32 offset in this file of its bytes
          u32 relocation count, u32 offset in this file of its relocations
            (u32 slot offsets of the words that hold slot offsets)
          u32 CRC of its bytes and then its relocations, as in this file
Each CRC is the engine's (source/memory/crc.c: crc_new, then
crc_checksum_buffer), so the Wii checks them with the engine's own code.

Manifest (ASCII, LF):
    HWIMAP-MANIFEST 2
    <map name> <format version> <file bytes> <staging digest: 8 hex digits> <SHA-256: 64 hex digits>
one line a staged map, sorted by name. The Wii's cache layer refuses a map
whose line is missing or disagrees with its file before reading any of it.

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
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import halo_cache  # noqa: E402
import recorded_animation  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TABLES = ROOT / "tools/wii/cache_schema_tables.c"
LAYOUTS = ROOT / "tools/wii/map_layouts.json"
WALKER_SOURCES = ["tools/wii/map_stage_walk.c", "tools/wii/cache_schema_graph.c", "tools/wii/cache_schema_tables.c"]
TAG_BASE = 0x803A6000
SLOT_BYTES = 0x01600000
MAGIC = b"HWIMAP02"
FORMAT_VERSION = 2
MANIFEST_MAGIC = "HWIMAP-MANIFEST"
TABLE_BYTES = 0x40
SEGMENT_BYTES = 0x20
MAXIMUM_STRUCTURE_BSPS = 16         # MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO (scenario.h)
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
    """Byte-order plan of one loaded range of the slot (the tag data, or one structure BSP, which load over
    each other's bytes: each has its own plan): runs of (offset, width, count) in slot offsets; each byte
    is planned once."""

    def __init__(self, size, base=0):
        self.base = base
        self.owner = bytearray(size)   # 1 where a byte is planned (index: slot offset - base)
        self.runs = []
        self.words = set()             # offsets of planned 4-byte scalars (pointer fields must be one)
        self.halfwords = set()         # (offset, 2) of planned 2-byte scalars
        self.counts = {}

    def span(self, offset, count):
        return self.owner[offset - self.base:offset - self.base + count]

    def add(self, offset, width, count, what):
        span = width * count
        if span <= 0:
            return
        at = offset - self.base
        if at < 0 or at + span > len(self.owner):
            raise StageError(f"{what}: outside its loaded range")
        if self.owner[at:at + span].count(0) != span:
            raise StageError(f"{what}: bytes planned twice")
        self.owner[at:at + span] = b"\x01" * span
        if width > 1:
            self.runs.append((offset, width, count))
        if width == 4:
            self.words.update(range(offset, offset + span, 4))
        elif width == 2 and what == "model_indices":
            self.halfwords.update((at, 2) for at in range(offset, offset + span, 2))
        self.counts[what] = self.counts.get(what, 0) + span

    def bytes_(self, offset, count, what):
        self.add(offset, 1, count, what)

    def unplanned(self):
        return len(self.owner) - self.owner.count(1)

    def apply(self, data, data_base=None):
        """data (holding slot offsets from data_base, the plan's base by default) swapped to the other
        byte order in place, run by run"""
        shift = self.base if data_base is None else data_base
        for offset, width, count in self.runs:
            start = offset - shift
            end = start + width * count
            if width == 8:
                values = array("Q")
            elif width == 4:
                values = array("I")
            else:
                values = array("H")
            values.frombytes(bytes(data[start:end]))
            values.byteswap()
            data[start:end] = values.tobytes()


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
    widths = bytearray(b"\x01" * item["size"])
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


# script value types (hs.h enum hs_type) whose value is a boolean cell (its first byte) or a short cell (its
# first two bytes: hs_runtime.c hs_global_reconcile_read's short types, hs.h HS_CELL_SHORT): a short integer,
# a script, the scenario elements (trigger volume, cutscene flag, camera point, title and recording, device
# group, ai command list, starting profile, conversation, navpoint, hud message), the five enums and the six
# object names. Every other type's value is a whole long (a real, a long, a string, an ai, an object list, a
# tag, an object).
HS_TYPE_BOOLEAN = 5
HS_SHORT_CELL_TYPES = frozenset([7, *range(10, 17), *range(18, 23), *range(32, 37), *range(43, 49)])


def rule_syntax_data(plan, slot, layouts, offset, size, element):
    """scenario hs_syntax_data: a struct data_array, then its struct hs_syntax_node elements. A node's
    last word is a union. A constant's value (a primitive node that is not a global's name) is a cell
    of its type (HS_TYPE_BOOLEAN, HS_SHORT_CELL_TYPES): a boolean's byte (the other three bytes left as
    they are), a short cell's first two bytes as a short (the other two as they are: January wrote some
    as a long, some as the short alone), otherwise a long (a real's float too). Any other node's last word
    is a long: a node index, or a global's index (read as a long cast). Constants of types from string on
    are parsed again from their source as the map loads (hs_compile_postprocess), and written natively."""
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
        constant = flags & 1 and not flags & 4    # primitive, not a global's name
        if constant and node_type == HS_TYPE_BOOLEAN:
            plan.bytes_(at + value_at, 4, "data_script")
        elif constant and node_type in HS_SHORT_CELL_TYPES:
            plan.add(at + value_at, 2, 1, "data_script")
            plan.bytes_(at + value_at + 2, 2, "data_script")
        else:
            plan.add(at + value_at, 4, 1, "data_script")
    rest = size - header["size"] - maximum * element_size
    plan.bytes_(offset + size - rest, rest, "data_script")


RECORDED_FIELD_WIDTHS = {"u8": 1, "s8": 1, "pad8": 1, "s16": 2, "u16": 2, "s32": 4, "f32": 4}


def recorded_animation_fields(stream, animation_version, unit_control_version):
    """(offset, width) of every field of a recorded animation's event stream, in order, by the stream's own
    layout (tools/wii/recorded_animation.py's tables, decoded with explicit little-endian reads): its unit
    control's fields, the current codec's animation state (six shorts), then each event's header and payload,
    up to its end event. A width of 1 is bytes: 8-bit fields, and the current codec's event header and time
    delta, which the engine decodes byte by byte (recorded_animation_playback.c
    recorded_animation_decode_event_header); the v1 codec's header is a native {short type; word delta}."""
    fields = recorded_animation.unit_control_fields(unit_control_version)
    if fields is None or animation_version not in (1, 2, 3, 4):
        raise StageError("a recorded animation of a version the engine does not play")
    current = animation_version == 4
    result = []
    position = 0
    for code, size in fields:
        result.append((position, RECORDED_FIELD_WIDTHS[code]))
        position += size
    if current:
        result += [(position + 2 * index, 2) for index in range(6)]
        position += 12
    while True:
        if position >= len(stream):
            raise StageError("a recorded animation's event stream ends before its end event")
        if current:
            head = stream[position]
            kind, event_type = head & 3, head >> 2
            size = 1 if kind < 2 else 2 if kind == 2 else 3
            result.append((position, 1))
            if size > 1:
                result.append((position + 1, 1))
            if size > 2:
                result.append((position + 2, 1))
            payload = recorded_animation.CURRENT_PAYLOADS.get(event_type)
            end = event_type == recorded_animation.CURRENT_END
        else:
            if position + 4 > len(stream):
                raise StageError("a recorded animation's event stream ends inside an event")
            event_type = struct.unpack_from("<h", stream, position)[0]
            size = 4
            result += [(position, 2), (position + 2, 2)]
            payload = recorded_animation.V1_PAYLOADS.get(event_type)
            end = event_type == recorded_animation.V1_END
        if payload is None:
            raise StageError("a recorded animation's event stream has an event the engine does not play")
        position += size
        for code in payload:
            result.append((position, RECORDED_FIELD_WIDTHS[code]))
            position += RECORDED_FIELD_WIDTHS[code]
        if position > len(stream):
            raise StageError("a recorded animation's event stream ends inside an event")
        if end:
            return result, position


def rule_recorded_animation(plan, slot, layouts, offset, size, element):
    """recorded_animation event_stream: the engine reads its unit control, its animation state and its
    events' payloads natively (memcpy: recorded_animation_initialize.c, recorded_animation_playback*.c), so
    each multi-byte field is put in the target's order, where recorded_animation_fields finds it; the rest
    (8-bit fields, the current codec's headers, what follows the end event) is bytes. The versions are the
    element's (struct recorded_animation_definition)."""
    animation_version = slot[element + member(layouts, "struct recorded_animation_definition", "version")]
    unit_control_version = slot[element + member(layouts, "struct recorded_animation_definition",
                                                 "unit_control_data_version")]
    fields, end = recorded_animation_fields(bytes(slot[offset:offset + size]), animation_version,
                                            unit_control_version)
    for at, width in fields:
        if width == 1:
            plan.bytes_(offset + at, 1, "data_recorded_animation")
        else:
            plan.add(offset + at, width, 1, "data_recorded_animation")
    plan.bytes_(offset + end, size - end, "data_recorded_animation")


def fill_bytes(plan, offset, size, what):
    position, end = offset, offset + size
    owner, base = plan.owner, plan.base
    while position < end:
        if owner[position - base]:
            position += 1
            continue
        start = position
        while position < end and not owner[position - base]:
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
    ("recorded_animation", 0): rule_recorded_animation,  # event_stream: its fields by its layout (HWI-016C)
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
    span = plan.span(at, 2 * index_count)
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


def split_records(records, tag_bytes, bsp):
    """a walk's records: those in the tag data, and those in its structure bsp (every one must lie in one)"""
    tag_records, bsp_records = [], []
    low, high = bsp["slot_offset"], bsp["slot_offset"] + bsp["bytes"]
    for record in records:
        if record[1] < tag_bytes:
            tag_records.append(record)
        elif low <= record[1] < high:
            bsp_records.append(record)
        else:
            raise StageError("the walk visited bytes outside the tag data and its structure bsp")
    return tag_records, bsp_records


def plan_records(plan, schema, layouts, slot, records, cache, run_cache):
    """the byte-order plan of a range's elements and tag data fields; returns (elements, data fields,
    rules used, pointers, nulled fields)"""
    elements = data_fields = 0
    rules_used = {}
    pointers, nulls = [], []
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
    for offset, _ in pointers:
        if offset not in plan.words:
            raise StageError("a pointer field is not a planned 32-bit word")
    for offset in nulls:
        if offset not in plan.words:
            raise StageError("a nulled field is not a planned 32-bit word")
    return elements, data_fields, rules_used, pointers, nulls


def buffer_payloads(slot, tables):
    """the slot offsets of the Xbox vertex and index buffer payloads that buffer tables name (each entry
    Common, Data, Lock: Data the payload's Xbox address); tables: (count offset, table pointer offset)"""
    starts = set()
    for count_at, pointer_at in tables:
        count = s32(slot, count_at)
        table = u32(slot, pointer_at) - TAG_BASE
        for index in range(max(count, 0)):
            payload = u32(slot, table + 12 * index + 4)
            if payload >= TAG_BASE:
                starts.add(payload - TAG_BASE)
    return starts


def payload_area(starts):
    """(first, last) payload start, or None"""
    return (min(starts), max(starts)) if starts else None


def unplanned_runs(plan):
    """(slot offset, bytes) of each run of bytes the plan leaves unconverted"""
    runs, owner, position, size = [], plan.owner, 0, len(plan.owner)
    while position < size:
        position = owner.find(0, position)
        if position < 0:
            break
        end = owner.find(1, position)
        end = size if end < 0 else end
        runs.append((plan.base + position, end - position))
        position = end
    return runs


def classify_unplanned(plan, markers, data, data_base, payload_area=None):
    """the unconverted bytes by what they are, so that none is unaccounted for. markers: (kind, slot offsets)
    in order of precedence; a run of unconverted bytes holding one of a kind's offsets is of that kind:
    - "tag_names": a tag's name (8-bit text) that the tag table points at;
    - "buffer_payloads": a vertex or index buffer payload a header's buffer table names (read only by the
      renderer, unsupported on the Wii);
    - "nulled_targets": what a pointer field the walk nulls led to (nothing the game reads reaches it);
    - "roots_without_schema": the root of a tag whose group has no schema definition;
    - "unwalked_targets": what a converted word holding a slot address leads to that the walk neither
      relocates nor nulls: a block the validator's schema does not list (an editor's: a scenario's child
      scenario references, editor data and decals), which the game does not read.
    Then runs inside the area the buffer payloads span ("buffer_payloads": an index buffer's tail of
    strip-restart indices and fill), runs of zero bytes ("zero": the same in either byte order, such as a
    tag group's unused fields past the part its schema definition covers), padding ("alignment"): runs of 1-3 bytes ending on a 4-byte boundary, of under 32 bytes ending on
    a 32-byte boundary (where the cache aligns a block), or of under 4 KiB ending the range; and any others
    ("other", counted and reported, never hidden)."""
    import bisect
    kinds = [(kind, sorted(offsets)) for kind, offsets in markers]
    result = {kind: 0 for kind, _ in kinds}
    result.update({"zero": 0, "alignment": 0, "other": 0, "other_runs": 0})
    for start, size in unplanned_runs(plan):
        for kind, offsets in kinds:
            index = bisect.bisect_left(offsets, start)
            if index < len(offsets) and offsets[index] < start + size:
                result[kind] += size
                break
        else:
            end = start + size
            if payload_area and payload_area[0] <= start and end <= payload_area[1]:
                result["buffer_payloads"] += size
            elif not any(data[start - data_base:end - data_base]):
                result["zero"] += size
            elif (size < 4 and end % 4 == 0) or (size < 32 and end % 32 == 0) or (
                    end == plan.base + len(plan.owner) and size < 4096):
                result["alignment"] += size
            else:
                result["other"] += size
                result["other_runs"] += 1
    return result


def tag_name_offsets(tags, instances, count, tag_bytes):
    """the slot offsets of the tags' names, as the tag table points at them"""
    names = set()
    for ordinal in range(count):
        name = u32(tags, instances + ordinal * 32 + 16)
        if TAG_BASE <= name < TAG_BASE + tag_bytes:
            names.add(name - TAG_BASE)
    return names


def nulled_targets(data, data_base, nulls, low, high):
    """where the pointer fields the walk nulls pointed (data: the range's bytes before they are nulled)"""
    targets = set()
    for offset in nulls:
        target = u32(data, offset - data_base)
        if TAG_BASE + low <= target < TAG_BASE + high:
            targets.add(target - TAG_BASE)
    return targets


def roots_without_schema(tags, instances, count, schema, tag_bytes):
    """the slot offsets of the roots of tags whose group has no schema definition"""
    roots = set()
    for ordinal in range(count):
        group = u32(tags, instances + ordinal * 32)
        root = u32(tags, instances + ordinal * 32 + 20)
        if group != SBSP and schema.groups.get(group, -1) < 0 and TAG_BASE <= root < TAG_BASE + tag_bytes:
            roots.add(root - TAG_BASE)
    return roots


def unwalked_targets(data, data_base, plan, walked, low, high):
    """where the converted 32-bit words of a range (data: its bytes, from slot offset data_base) that hold a
    slot address in [low, high) lead, of those the walk neither relocates nor nulls (walked)"""
    targets = set()
    words = plan.words
    for index, (value,) in enumerate(struct.iter_unpack("<I", data[:len(data) // 4 * 4])):
        if TAG_BASE + low <= value < TAG_BASE + high:
            at = data_base + 4 * index
            if at in words and at not in walked:
                targets.add(value - TAG_BASE)
    return targets


def engine_crc(data, crc=0xFFFFFFFF):
    """the engine's crc_checksum_buffer (source/memory/crc.c) continued from crc (crc_new's 0xFFFFFFFF):
    the reflected CRC-32 without its final inversion"""
    return zlib.crc32(data, crc ^ 0xFFFFFFFF) ^ 0xFFFFFFFF


def stage(map_path, walker, work):
    started = time.perf_counter()
    schema = Schema(TABLES.read_text(encoding="utf-8"))
    layouts = load_layouts(schema)
    header, body = read_map(map_path)
    tag_offset, tag_bytes = header["tag_offset"], header["tag_bytes"]
    tags = bytes(body[tag_offset:tag_offset + tag_bytes])
    bsps = structure_bsps(tags)
    if not bsps:
        raise StageError("the scenario has no structure bsp")
    if len(bsps) > MAXIMUM_STRUCTURE_BSPS:
        raise StageError("the scenario has more structure bsps than the engine holds")
    for bsp in bsps:
        if not (tag_bytes <= bsp["slot_offset"] and 0 < bsp["bytes"] and
                bsp["slot_offset"] + bsp["bytes"] <= SLOT_BYTES and
                0 <= bsp["file_offset"] <= len(body) - bsp["bytes"]):
            raise StageError("a structure bsp does not fit the slot")
    slot = bytearray(SLOT_BYTES)
    slot[:tag_bytes] = tags

    def place(bsp):
        """the slot as the engine has it with this bsp loaded: each loads alone, over the others' bytes"""
        slot[tag_bytes:] = bytes(SLOT_BYTES - tag_bytes)
        slot[bsp["slot_offset"]:bsp["slot_offset"] + bsp["bytes"]] = body[bsp["file_offset"]:
                                                                            bsp["file_offset"] + bsp["bytes"]]

    # the tag data walked with each bsp in turn: its records must be the same in every walk
    walks = []
    tag_records = None
    for bsp in bsps:
        place(bsp)
        records, graph = run_walker(walker, work, slot, tag_bytes, bsp)
        own_tags, own_bsp = split_records(records, tag_bytes, bsp)
        if tag_records is None:
            tag_records = own_tags
        elif own_tags != tag_records:
            raise StageError("the tag data walks differently with another structure bsp")
        walks.append((bsp, own_bsp, graph))

    count = s32(tags, 12)
    instances = u32(tags, 0) - TAG_BASE
    # roots of groups the schema has no definition for: their bytes are not converted
    unconverted_groups = {}
    for ordinal in range(count):
        group = u32(tags, instances + ordinal * 32)
        if group != SBSP and schema.groups.get(group, -1) < 0:
            name = struct.pack(">I", group).decode("latin-1")
            unconverted_groups[name] = unconverted_groups.get(name, 0) + 1

    cache, run_cache = {}, {}
    # the tag data: its header, tag table, buffer tables, then what the walk visited
    tag_plan = Plan(tag_bytes)
    tag_plan.add(0, 4, 9, "header")
    tag_plan.add(instances, 4, 8 * count, "tag_table")
    tag_tables = ((16, 20), (24, 28))
    for count_at, pointer_at in tag_tables:
        buffers = s32(tags, count_at)
        if buffers:
            tag_plan.add(u32(tags, pointer_at) - TAG_BASE, 4, 3 * buffers, "buffers")
    place(bsps[0])
    elements, data_fields, rules_used, pointers, nulls = plan_records(tag_plan, schema, layouts, slot, tag_records,
                                                                     cache, run_cache)
    model_part = next(index for index, item in enumerate(schema.definitions) if item[3] == "model_geometry_part")
    model_indices = sum(model_part_indices(tag_plan, slot, schema, a, b) for kind, a, b, c, d in tag_records
                        if kind == 1 and b == model_part)
    walked = {offset for offset, _ in pointers} | set(nulls)
    tag_unplanned = classify_unplanned(
        tag_plan, [("tag_names", tag_name_offsets(tags, instances, count, tag_bytes)),
                   ("buffer_payloads", buffer_payloads(slot, tag_tables)),
                   ("nulled_targets", nulled_targets(tags, 0, nulls, 0, tag_bytes)),
                   ("roots_without_schema", roots_without_schema(tags, instances, count, schema, tag_bytes)),
                   ("unwalked_targets", unwalked_targets(tags, 0, tag_plan, walked, 0, tag_bytes))],
        tags, 0, payload_area(buffer_payloads(slot, tag_tables)))
    little_tags = bytearray(tags)
    for offset, target in pointers:
        struct.pack_into("<I", little_tags, offset, target)
    for offset in nulls:
        struct.pack_into("<I", little_tags, offset, 0)
    big_tags = bytearray(little_tags)
    tag_plan.apply(big_tags)
    # the conversion only reorders bytes inside scalars: each loaded range holds the same bytes in both orders
    if sorted(little_tags) != sorted(big_tags):
        raise StageError("the conversion changed a loaded range's bytes, not only their order")
    segments = [{"kind": SEGMENT_TAGS, "file_offset": tag_offset, "bytes": tag_bytes, "slot_offset": 0,
                 "little": bytes(little_tags), "big": bytes(big_tags),
                 "relocations": sorted(offset for offset, _ in pointers)}]
    planned = dict(tag_plan.counts)
    total = {"elements": elements, "data_fields": data_fields, "relocations": len(pointers), "nulled": len(nulls)}
    segment_reports = [{"kind": "tags", "bytes": tag_bytes, "elements": elements, "data_fields": data_fields,
                        "relocations": len(pointers), "nulled": len(nulls), "unplanned": tag_unplanned}]

    # each structure bsp: its header, buffer tables and what its walk visited, planned on its own
    for bsp, bsp_records, graph in walks:
        place(bsp)
        at = bsp["slot_offset"]
        plan = Plan(bsp["bytes"], at)
        plan.add(at, 4, 6, "bsp_header")
        bsp_tables = ((at + 4, at + 8), (at + 12, at + 16))
        for count_at, pointer_at in bsp_tables:
            buffers = s32(slot, count_at)
            if buffers:
                plan.add(u32(slot, pointer_at) - TAG_BASE, 4, 3 * buffers, "buffers")
        b_elements, b_data, b_rules, b_pointers, b_nulls = plan_records(plan, schema, layouts, slot, bsp_records,
                                                                        cache, run_cache)
        for key, value in b_rules.items():
            rules_used[key] = rules_used.get(key, 0) + value
        for key, value in plan.counts.items():
            planned[key] = planned.get(key, 0) + value
        little = bytearray(slot[at:at + bsp["bytes"]])
        for offset, target in b_pointers:
            struct.pack_into("<I", little, offset - at, target)
        for offset in b_nulls:
            struct.pack_into("<I", little, offset - at, 0)
        big = bytearray(little)
        plan.apply(big)
        if sorted(little) != sorted(big):
            raise StageError("the conversion changed a loaded range's bytes, not only their order")
        segments.append({"kind": SEGMENT_BSP, "file_offset": bsp["file_offset"], "bytes": bsp["bytes"],
                         "slot_offset": at, "little": bytes(little), "big": bytes(big),
                         "relocations": sorted(offset for offset, _ in b_pointers)})
        for key, value in (("elements", b_elements), ("data_fields", b_data), ("relocations", len(b_pointers)),
                           ("nulled", len(b_nulls))):
            total[key] += value
        segment_reports.append({"kind": "bsp", "bytes": bsp["bytes"], "slot_offset": at, "elements": b_elements,
                                "data_fields": b_data, "relocations": len(b_pointers), "nulled": len(b_nulls),
                                "unplanned": classify_unplanned(
                                    plan, [("buffer_payloads", buffer_payloads(slot, bsp_tables)),
                                           ("nulled_targets", nulled_targets(slot, 0, b_nulls, at, at + bsp["bytes"])),
                                           ("unwalked_targets", unwalked_targets(
                                               slot[at:at + bsp["bytes"]], at, plan,
                                               {offset for offset, _ in b_pointers} | set(b_nulls), at,
                                               at + bsp["bytes"]))],
                                    slot, 0, payload_area(buffer_payloads(slot, bsp_tables)))})

    outputs = {}
    for big_endian in (True, False):
        outputs["be" if big_endian else "le"] = write_wmap(header_bytes=body[:HEADER_BYTES], segments=segments,
                                                           file_length=header["declared_bytes"],
                                                           big_endian=big_endian)
    unplanned = {}
    for item in segment_reports:
        for key, value in item["unplanned"].items():
            unplanned[key] = unplanned.get(key, 0) + value
    report = {
        "format": MAGIC.decode("ascii"), "format_version": FORMAT_VERSION,
        "map": {"name": header["name"], "build": header["build"], "category": header["category"],
                "declared_bytes": header["declared_bytes"], "tag_bytes": tag_bytes, "tags": count,
                "bsps": [{"bytes": b["bytes"], "slot_offset": b["slot_offset"]} for b in bsps]},
        "walks": [graph for _, _, graph in walks],
        "elements": total["elements"], "data_fields": total["data_fields"], "model_indices": model_indices,
        "data_rules": dict(sorted(rules_used.items())),
        "relocations": total["relocations"], "nulled": total["nulled"],
        "segments": segment_reports,
        "planned_bytes": dict(sorted(planned.items())),
        "unplanned_loaded_bytes": sum(value for key, value in unplanned.items() if key != "other_runs"),
        "unplanned_by_kind": unplanned,
        "byte_multisets_equal": True,
        "unconverted_groups": unconverted_groups,
        "outputs": {key: {"bytes": len(value), "sha256": hashlib.sha256(value).hexdigest(),
                          "staging_digest": f"{table_digest(value):08x}"}
                    for key, value in outputs.items()},
        "seconds": round(time.perf_counter() - started, 1),
    }
    return report, outputs


def write_wmap(header_bytes, segments, file_length, big_endian):
    """the staged file (format HWIMAP02, above) in one byte order; segments: dicts of kind, file_offset,
    bytes, slot_offset, relocations and the segment's "big" and "little" images"""
    order = ">" if big_endian else "<"
    image_key = "big" if big_endian else "little"
    table_end = TABLE_BYTES + SEGMENT_BYTES * len(segments)
    header_at = (table_end + 31) & ~31
    position = header_at + HEADER_BYTES
    blobs = []
    entries = []
    for segment in segments:
        data_at = (position + 31) & ~31
        image = segment[image_key]
        relocations = struct.pack(f"{order}{len(segment['relocations'])}I", *segment["relocations"])
        position = data_at + len(image)
        relocation_at = (position + 3) & ~3
        position = relocation_at + len(relocations)
        entries.append((segment["kind"], segment["file_offset"], segment["bytes"], segment["slot_offset"], data_at,
                        len(segment["relocations"]), relocation_at, engine_crc(relocations, engine_crc(image))))
        blobs.append((data_at, image))
        blobs.append((relocation_at, relocations))
    out = bytearray(position)
    header = engine_header(header_bytes, big_endian)
    out[0:8] = MAGIC
    struct.pack_into(order + "9I", out, 8, 0x01020304, len(segments), HEADER_BYTES, header_at, file_length,
                     len(out), engine_crc(header), 0, FORMAT_VERSION)
    for index, entry in enumerate(entries):
        struct.pack_into(order + "8I", out, TABLE_BYTES + SEGMENT_BYTES * index, *entry)
    out[header_at:header_at + HEADER_BYTES] = header
    for at, blob in blobs:
        out[at:at + len(blob)] = blob
    struct.pack_into(order + "I", out, 0x24, engine_crc(bytes(out[:table_end])))
    return bytes(out)


def table_digest(data):
    """a staged file's staging digest: the CRC its table holds of itself, which the manifest lists (it
    covers the header's and every segment's CRC, so it names the staging)"""
    order = ">" if data[8:12] == b"\x01\x02\x03\x04" else "<"
    return struct.unpack_from(order + "I", data, 0x24)[0]


def manifest_line(name, data):
    if not re.fullmatch(r"[a-z0-9_]{1,31}", name):
        raise StageError("a staged map's name must be 1-31 lower-case letters, digits or underscores")
    return f"{name} {FORMAT_VERSION} {len(data)} {table_digest(data):08x} {hashlib.sha256(data).hexdigest()}"


def manifest_update(path, name, data):
    """adds or replaces name's line in the manifest at path, written whole and then moved over the old one
    (format: the module's docstring); lines of another format version are dropped"""
    lines = {}
    if path.exists():
        rows = path.read_text(encoding="ascii").splitlines()
        if rows and rows[0] == f"{MANIFEST_MAGIC} {FORMAT_VERSION}":
            for row in rows[1:]:
                words = row.split()
                if len(words) == 5 and words[1] == str(FORMAT_VERSION):
                    lines[words[0]] = row
    lines[name] = manifest_line(name, data)
    text = f"{MANIFEST_MAGIC} {FORMAT_VERSION}\n" + "".join(lines[key] + "\n" for key in sorted(lines))
    partial = path.with_suffix(path.suffix + ".partial")
    partial.write_text(text, encoding="ascii", newline="\n")
    os.replace(partial, path)


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
        # the card's manifest, and the little-endian staging's (manifest.le.txt: the host reference's)
        manifest_update(out / ("manifest.txt" if key == "be" else "manifest.le.txt"), stem, data)
    (out / f"{stem}.stage.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({key: report[key] for key in ("elements", "data_fields", "relocations", "nulled",
                                                   "unplanned_loaded_bytes", "unplanned_by_kind",
                                                   "unconverted_groups", "seconds")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
