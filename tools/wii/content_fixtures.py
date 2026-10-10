"""Authored synthetic fixtures for the content-pipeline tests.

Everything here is generated from arithmetic patterns: no game data, names or
bytes from any dump. build_map() lays out a minimal Xbox version-5 cache file
(header, zlib body, tag index, one scenario with recorded animations, one
bitmap group, one sound) so the converter and inventory run end to end.
"""
import hashlib
import json
from pathlib import Path
import struct
import zlib

import gx_texture as gt

BASE = 0x803A6000
V4_TICKS = 575  # sum of the authored v4 stream deltas plus one


def pattern(n, seed):
    """Deterministic pseudo-random bytes (an LCG; not derived from any data)."""
    state = (seed * 2654435761 + 12345) & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = state >> 24
    return bytes(out)


def swizzle_level(texels, width, height, depth, bpp):
    """Place row-major texels (bpp bytes each) at their Xbox Morton positions."""
    level = {"w": width, "h": height, "d": depth}
    out = bytearray(len(texels))
    for z in range(depth):
        for y in range(height):
            for x in range(width):
                src = ((z * height + y) * width + x) * bpp
                dst = gt.source_texel_index(level, x, y, z, True) * bpp
                out[dst:dst + bpp] = texels[src:src + bpp]
    return bytes(out)


def dxt1_blocks(blocks, seed, three_colour_every=4):
    """Authored DXT1 blocks; every Nth block uses the three-colour mode."""
    raw = bytearray(pattern(8 * blocks, seed))
    for b in range(blocks):
        c0, c1 = struct.unpack_from("<HH", raw, 8 * b)
        if c0 == c1:
            c1 ^= 0x8410
        if (b % three_colour_every == three_colour_every - 1) == (c0 > c1):
            c0, c1 = c1, c0
        struct.pack_into("<HH", raw, 8 * b, c0, c1)
    return bytes(raw)


def source_pixels(format_name, width, height, depth=1, type_name="2d", mipmap_count=0, seed=1, linear=False):
    """Authored hardware-layout pixels for a bitmap plus its bitmap_data fields."""
    fid = gt.FORMAT_IDS[format_name]
    tid = {"2d": 0, "3d": 1, "cube": 2}[type_name]
    flags = gt.FLAG_COMPRESSED if format_name in gt.COMPRESSED else 0
    if linear:
        flags |= gt.FLAG_LINEAR
    elif not any(v & (v - 1) for v in (width, height, depth)):
        flags |= gt.FLAG_POW2
    if format_name not in gt.COMPRESSED and not linear:
        flags |= gt.FLAG_SWIZZLED
    layout = gt.describe(width, height, depth, tid, fid, flags, mipmap_count)
    out = bytearray(layout["bytes"])
    for face in range(layout["faces"]):
        for index, level in enumerate(layout["levels"]):
            seed_here = seed * 131 + face * 17 + index
            start = face * layout["face_bytes"] + level["offset"]
            if format_name == "dxt1":
                data = dxt1_blocks(level["bytes"] // 8, seed_here)
            elif format_name in gt.COMPRESSED:
                data = pattern(level["bytes"], seed_here)
            elif linear:
                data = pattern(level["bytes"], seed_here)
            else:
                bpp = gt.BITS[format_name] // 8
                data = swizzle_level(pattern(level["bytes"], seed_here), level["w"], level["h"], level["d"], bpp)
            out[start:start + level["bytes"]] = data
    fields = {"width": width, "height": height, "depth": depth, "type": tid, "format": fid, "flags": flags,
              "mipmap_count": mipmap_count}
    return fields, bytes(out), layout


def recorded_stream_v4():
    """A version-4 stream using every event type and delta kind."""
    unit = bytearray()
    unit += bytes([3, 200]) + struct.pack("<hhh", -2, 7, 0x1234)
    unit += struct.pack("<2f", 0.5, -0.25) + struct.pack("<9f", *[i / 8 - 0.5 for i in range(9)])
    unit += struct.pack("<i", -70000) + struct.pack("<hh", 300, -1)
    state = struct.pack("<6h", 10, -20, 30, -40, 999, -999)
    events = bytearray()
    events += bytes([2 << 2 | 0, 5])                     # animation state, delta 0
    events += bytes([3 << 2 | 1, 9])                     # aiming speed, delta 1
    events += bytes([4 << 2 | 2, 2]) + struct.pack("<h", -3)          # control flags, byte delta 2
    events += bytes([5 << 2 | 3]) + struct.pack("<H", 300) + struct.pack("<h", 4)  # weapon, word delta
    events += bytes([6 << 2 | 2, 255]) + struct.pack("<2f", 1.0, -1.0)  # throttle
    for t in range(7, 15):
        events += bytes([t << 2 | 1]) + struct.pack("<bb", -t, t)
    for t in range(15, 23):
        events += bytes([t << 2 | 0]) + struct.pack("<hh", -1000 * t, 1000 + t)
    events += bytes([0 << 2 | 1])                        # nothing event
    events += bytes([1 << 2 | 2, 7])                     # end, byte delta 7
    return bytes(unit + state + events)


def recorded_stream_v1():
    unit = bytes([1, 2]) + struct.pack("<hhh", 3, 4, 5) + struct.pack("<11f", *[float(i) for i in range(11)])
    events = bytearray()
    events += struct.pack("<hH", 2, 1) + bytes([7, 0])
    events += struct.pack("<hH", 4, 2) + struct.pack("<H", 0xBEEF)
    events += struct.pack("<hH", 6, 3) + struct.pack("<2f", 0.25, 0.75)
    events += struct.pack("<hH", 7, 0)
    events += struct.pack("<hH", 12, 4) + struct.pack("<3f", 1.0, 2.0, 3.0)
    events += struct.pack("<hH", 19, 5) + struct.pack("<2f", -1.0, 1.5)
    events += struct.pack("<hH", 1, 6)
    return unit + bytes(events)


def adpcm_payload(blocks, channels, seed):
    raw = bytearray(pattern(36 * channels * blocks, seed))
    for b in range(blocks):
        for c in range(channels):
            raw[36 * channels * b + 4 * c + 2] %= 89   # valid step index
            raw[36 * channels * b + 4 * c + 3] = 0
    return bytes(raw)


class _Tags:
    """Builds a tag-data region at TAG_BASE with simple bump allocation."""

    def __init__(self, base=BASE, reserved=36):
        self.base = base
        self.data = bytearray(reserved)

    def alloc(self, size, align=4):
        while len(self.data) % align:
            self.data.append(0)
        offset = len(self.data)
        self.data += bytes(size)
        return offset

    def address(self, offset):
        return self.base + offset

    def string(self, text):
        offset = self.alloc(len(text) + 1, 1)
        self.data[offset:offset + len(text)] = text.encode("ascii")
        return offset


def build_map(name="fixture", category=0, bitmaps=None, animation_streams=None, sounds=None, mutate=None,
              categories=None):
    """Return stored map bytes for an authored minimal Xbox v5 cache.

    bitmaps: list of (format, width, height, depth, type, mipmap_count, linear)
    animation_streams: list of (version, unit_control_version, ticks, stream bytes)
    sounds: list of (rate_id, encoding, compression, payload bytes)
    mutate: optional callable(state dict) applied before compression to author malformed cases.
    categories: None, or a set of defect names (empty for none) to add the HWI-008C
    category tags (category_tags) with those defects.
    """
    if bitmaps is None:
        bitmaps = [("dxt1", 16, 16, 1, "2d", 2, False), ("a8r8g8b8", 8, 8, 1, "2d", 3, False),
                   ("ay8", 8, 4, 1, "2d", 0, False), ("r5g6b5", 8, 8, 1, "cube", 1, False)]
    if animation_streams is None:
        stream = recorded_stream_v4()
        animation_streams = [(4, 4, V4_TICKS, stream)]
    if sounds is None:
        sounds = [(0, 0, 1, adpcm_payload(3, 1, 5)), (1, 1, 1, adpcm_payload(2, 2, 6))]
    # pixel region after the header
    body = bytearray(0x800)
    pixel_records = []
    for i, (fmt, w, h, d, kind, mips, linear) in enumerate(bitmaps):
        fields, pixels, _ = source_pixels(fmt, w, h, d, kind, mips, seed=i + 1, linear=linear)
        while len(body) % 512:
            body.append(0)
        pixel_records.append((fields, len(body), len(pixels)))
        body += pixels
    sound_records = []
    for rate_id, encoding, compression, payload in sounds:
        sound_records.append((rate_id, encoding, compression, len(body), len(payload)))
        body += payload
    tags = _Tags()
    instances = tags.alloc(32 * 3)
    names = [tags.string("fixture\\scenario"), tags.string("fixture\\bitmaps"), tags.string("fixture\\sound")]
    # scenario with recorded animations
    scenario = tags.alloc(0x5B0)
    anim_table = tags.alloc(0x40 * len(animation_streams)) if animation_streams else 0
    for i, (version, ucv, ticks, stream) in enumerate(animation_streams):
        element = anim_table + 0x40 * i
        tags.data[element:element + 9] = b"anim%04d\0" % i
        tags.data[element + 32] = version
        tags.data[element + 34] = ucv
        struct.pack_into("<h", tags.data, element + 36, ticks)
        data_offset = tags.alloc(len(stream), 1)
        tags.data[data_offset:data_offset + len(stream)] = stream
        struct.pack_into("<iIiI", tags.data, element + 44, len(stream), 0, 0, tags.address(data_offset))
    struct.pack_into("<iI", tags.data, scenario + 0x36C, len(animation_streams),
                     tags.address(anim_table) if animation_streams else 0)
    # bitmap group
    group = tags.alloc(0x6C)
    table = tags.alloc(0x30 * len(pixel_records))
    for i, (fields, offset, size) in enumerate(pixel_records):
        element = table + 0x30 * i
        struct.pack_into("<IhhhhhH", tags.data, element, 0x6269746D, fields["width"], fields["height"],
                         fields["depth"], fields["type"], fields["format"], fields["flags"] | 0x80)
        struct.pack_into("<hhii", tags.data, element + 0x14, fields["mipmap_count"], 0, offset, size)
    struct.pack_into("<iI", tags.data, group + 0x60, len(pixel_records), tags.address(table))
    # sound
    sound = tags.alloc(0xA4)
    pitch = tags.alloc(0x48)
    perms = tags.alloc(0x7C * len(sound_records))
    rate_id, encoding, compression = sound_records[0][:3] if sound_records else (0, 0, 1)
    struct.pack_into("<h", tags.data, sound + 6, rate_id)
    struct.pack_into("<hh", tags.data, sound + 0x6C, encoding, compression)
    struct.pack_into("<iI", tags.data, sound + 0x98, 1, tags.address(pitch))
    struct.pack_into("<iI", tags.data, pitch + 0x3C, len(sound_records), tags.address(perms))
    for i, (_, _, p_compression, offset, size) in enumerate(sound_records):
        element = perms + 0x7C * i
        struct.pack_into("<h", tags.data, element + 0x28, p_compression)
        struct.pack_into("<iIiI", tags.data, element + 0x40, size, 0, offset, 0)
    rows = [(0x73636E72, 0, scenario), (0x6269746D, 1, group), (0x736E6421, 2, sound)]
    for ordinal, (group_word, name_index, root) in enumerate(rows):
        datum = 0xE1740000 | ordinal
        struct.pack_into("<8I", tags.data, instances + 32 * ordinal, group_word, 0xFFFFFFFF, 0xFFFFFFFF, datum,
                         tags.address(names[name_index]), tags.address(root), 0, 0)
    struct.pack_into("<9I", tags.data, 0, tags.address(instances), 0xE1740000, 0x12345678, len(rows), 0, 0, 0, 0,
                     0x74616773)
    if categories is not None:
        extra, buffers = category_tags(tags, body, scenario, len(rows), set(categories))
        table_all = tags.alloc(32 * (len(rows) + len(extra)))
        all_rows = [(g, tags.address(names[n]), tags.address(r)) for g, n, r in rows]
        all_rows += [(g, tags.address(names[0]), r) for g, r in extra]
        for ordinal, (group_word, name_address, root_address) in enumerate(all_rows):
            struct.pack_into("<8I", tags.data, table_all + 32 * ordinal, group_word, 0xFFFFFFFF, 0xFFFFFFFF,
                             0xE1740000 | ordinal, name_address, root_address, 0, 0)
        struct.pack_into("<9I", tags.data, 0, tags.address(table_all), 0xE1740000, 0x12345678, len(all_rows),
                         *buffers, 0x74616773)
    while len(body) % 512:
        body.append(0)
    state = {"body": body, "tags": tags, "scenario": scenario, "group": group, "table": table, "sound": sound,
             "perms": perms, "anim_table": anim_table, "pixel_records": pixel_records}
    if mutate is not None:
        mutate(state)
    body = state["body"]
    tag_offset = len(body)
    body = body + tags.data
    declared = state.get("declared", len(body))
    header = bytearray(0x800)
    struct.pack_into("<6i", header, 0, 0x68656164, state.get("version", 5), declared, 0,
                     state.get("tag_offset_field", tag_offset), state.get("tag_bytes", len(tags.data)))
    header[0x20:0x20 + len(name)] = name.encode("ascii")
    header[0x40:0x40 + 13] = b"01.10.12.2276"
    struct.pack_into("<h", header, 0x60, category)
    struct.pack_into("<I", header, 0x7FC, state.get("foot", 0x666F6F74))
    compressed = zlib.compress(bytes(body[0x800:]), 9)
    return bytes(header) + compressed + state.get("trailer", b"")


def group_word(code):
    return struct.unpack(">I", code.encode("latin-1"))[0]


def _block(space, field, count, stride):
    """Allocate count elements and point the block field at them."""
    offset = space.alloc(stride * count) if count else 0
    struct.pack_into("<iI", space.data, field, count, space.address(offset) if count else 0)
    return offset


def _collision_bsp(space, bsp, defects):
    """One small authored collision BSP whose indices are consistent: 3D node
    children are nodes, NONE or leaves (sign bit), 2D references and 2D node
    children name 2D nodes or surfaces (sign bit), and a 2D reference plane
    may carry the flip bit, as the engine reads them."""
    leaf, surface = -0x80000000, -0x80000000
    arrays = {0: ("<3i", [(0, 1, -1), (1, leaf, leaf + 1)]), 12: ("<4f", [(0, 0, 1, 2.5), (1, 0, 0, -1)]),
              24: ("<Hhi", [(1, 2, 0), (0, 0, -1)]), 36: ("<2i", [(0, 0), (-0x80000000 + 1, surface)]),
              48: ("<3f2i", [(0.5, 0.5, 1.0, surface, surface + 1)]),
              60: ("<2iBbh", [(0, 0, 3, -1, 2), (1, 2, 0, 0, -1)]),
              72: ("<6i", [(0, 1, 1, 2, 0, -1), (1, 2, 2, 0, 1, 0), (2, 3, 0, 1, -1, 1)]),
              84: ("<3fi", [(0, 0, 0, 0), (1, 0, 0, 1), (1, 1, 0, 2), (0, 1, 0, 2)])}
    if "collision_edge" in defects:
        arrays[72][1][0] = (0, 9, 1, 2, 0, -1)
    for field, (fmt, rows) in arrays.items():
        size = struct.calcsize(fmt)
        offset = _block(space, bsp + field, len(rows), size)
        for i, row in enumerate(rows):
            struct.pack_into(fmt, space.data, offset + i * size, *row)


def _animation(tags, element, kind, frames, nodes, rotation, translation, scale, info_type, compressed=None):
    struct.pack_into("<hhhh", tags.data, element + 32, kind, frames, 0, info_type)
    struct.pack_into("<h", tags.data, element + 44, nodes)
    struct.pack_into("<II", tags.data, element + 92, translation, 0)
    struct.pack_into("<II", tags.data, element + 108, rotation, 0)
    struct.pack_into("<II", tags.data, element + 124, scale, 0)
    counts = [bin(m & ((1 << nodes) - 1)).count("1") for m in (rotation, translation, scale)]
    frame_size = counts[0] * 8 + counts[1] * 12 + counts[2] * 4
    struct.pack_into("<h", tags.data, element + 36, frame_size)

    def data(field, blob):
        offset = tags.alloc(len(blob), 4) if blob else 0
        tags.data[offset:offset + len(blob)] = blob
        struct.pack_into("<iIiI", tags.data, element + field, len(blob), 0, 0, tags.address(offset) if blob else 0)
    info = {0: 0, 1: 2, 2: 3, 3: 4}[info_type]
    data(72, struct.pack("<%df" % (frames * info), *[i * 0.125 for i in range(frames * info)]))
    if compressed is None:
        frame = b""
        for f in range(frames):
            for n in range(nodes):
                if rotation >> n & 1:
                    frame += struct.pack("<4h", f, -n, 300 + f, -32000)
                if translation >> n & 1:
                    frame += struct.pack("<3f", f, n, -1.5)
                if scale >> n & 1:
                    frame += struct.pack("<f", 1.0 + f / 8)
        defaults = b""
        for n in range(nodes):
            if not rotation >> n & 1:
                defaults += struct.pack("<4h", n, 2, -3, 32767)
            if not translation >> n & 1:
                defaults += struct.pack("<3f", n, 0.25, 0.5)
            if not scale >> n & 1:
                defaults += struct.pack("<f", 1.0)
        data(140, defaults)
        data(160, frame)
    else:
        struct.pack_into("<H", tags.data, element + 58, 1)
        data(140, b"")
        data(160, compressed)


def compressed_animation_block(nodes, rotation_keys, translation_keys, gap=False):
    """A compressed block: per rotated node rotation_keys keyframes, one translated node."""
    rotation_count = len(rotation_keys)
    header_end = 0x2C + 4 * rotation_count
    words = []
    first = 0
    for keys in rotation_keys:
        words.append(first << 12 | keys)
        first += keys
    rot_total, trans_total = first, translation_keys
    layout = {}
    cursor = header_end
    for name, size in (("rot_indices", 2 * rot_total), ("rot_defaults", 6 * nodes), ("rot_keys", 6 * rot_total),
                       ("trans_headers", 4), ("trans_indices", 2 * trans_total), ("trans_defaults", 12 * nodes),
                       ("trans_keys", 12 * trans_total)):
        layout[name] = cursor
        cursor += size + (2 if gap and name == "rot_keys" else 0)
    blob = bytearray(cursor)
    header = [layout["rot_indices"], layout["rot_defaults"], layout["rot_keys"], layout["trans_headers"],
              layout["trans_indices"], layout["trans_defaults"], layout["trans_keys"], 0, 0, 0, 0]
    struct.pack_into("<11i", blob, 0, *header)
    struct.pack_into("<%dI" % rotation_count, blob, 0x2C, *words)
    for i in range(rot_total):
        struct.pack_into("<H", blob, layout["rot_indices"] + 2 * i, i)
        struct.pack_into("<3H", blob, layout["rot_keys"] + 6 * i, 0x1234 + i, 0xFEDC, i)
    for n in range(nodes):
        struct.pack_into("<3H", blob, layout["rot_defaults"] + 6 * n, n, 0x8001, 0x7FFF)
        struct.pack_into("<3f", blob, layout["trans_defaults"] + 12 * n, n, -n, 0.5)
    struct.pack_into("<I", blob, layout["trans_headers"], translation_keys)
    for i in range(trans_total):
        struct.pack_into("<H", blob, layout["trans_indices"] + 2 * i, 2 * i)
        struct.pack_into("<3f", blob, layout["trans_keys"] + 12 * i, i, 2.0, -3.0)
    if gap:
        blob[layout["rot_keys"] + 6 * rot_total] = 0x5A
    return bytes(blob)


def category_tags(tags, body, scenario, first_ordinal, defects=frozenset()):
    """HWI-008C category tags: a model, collision model, animation graph, font,
    unicode string list, HUD message text, environment shader and one structure
    BSP (lightmaps, materials, collision) in the map body. Returns the extra
    instance rows [(group word, root address)] and the tag header's vertex and
    index buffer table words. defects names deliberate faults for rejection tests."""
    rows = []
    # -- model: one geometry, a strip part and a triangle-list part
    vertices = (5, 4)
    strips = [(1, [0, 1, 2, 3, 4, 2, 9 if "model_index" in defects else 1]), (0, [0, 1, 2, 2, 3, 0])]
    vtable = tags.alloc(12 * 2)
    itable = tags.alloc(12 * 2)
    model = tags.alloc(232)
    geometry = _block(tags, model + 208, 1, 48)
    parts = _block(tags, geometry + 36, 2, 104)
    for i, ((strip_type, indices), count) in enumerate(zip(strips, vertices)):
        part = parts + 104 * i
        struct.pack_into("<IhbbhhII3f", tags.data, part, 0, 0, -1 if i == 0 else 0, 1 if i == 0 else -1, 0, 0,
                         0x3F800000, 0, 0.5, -0.5, 2.0)
        vdata = tags.alloc(32 * count)
        tags.data[vdata:vdata + 32 * count] = pattern(32 * count, 40 + i)
        idata = tags.alloc(2 * len(indices))
        struct.pack_into("<%dH" % len(indices), tags.data, idata, *indices)
        struct.pack_into("<IIi", tags.data, vtable + 12 * i, 0x00040001, tags.address(vdata), 0)
        struct.pack_into("<IIi", tags.data, itable + 12 * i, 0x00010001, tags.address(idata), 0)
        triangles = len(indices) - 2 if strip_type == 1 else len(indices) // 3
        struct.pack_into("<hHiII", tags.data, part + 68, strip_type, 0, triangles, 0, tags.address(itable + 12 * i))
        vertex_type = 4 if "model_vertex_type" in defects else 5
        struct.pack_into("<hHiiII", tags.data, part + 84, vertex_type, 0, count, 0, 0, tags.address(vtable + 12 * i))
    rows.append((group_word("mode"), tags.address(model)))
    # -- collision model: one node with one BSP
    coll = tags.alloc(664)
    node = _block(tags, coll + 652, 1, 64)
    bsp = _block(tags, node + 52, 1, 96)
    _collision_bsp(tags, bsp, defects)
    rows.append((group_word("coll"), tags.address(coll)))
    # -- animation graph: uncompressed base, compressed, empty overlay
    graph = tags.alloc(128)
    animations = _block(tags, graph + 116, 3, 180)
    _animation(tags, animations, 0, 4, 3, 0b011, 0b001, 0b100, 2)
    if "animation_frame_size" in defects:
        struct.pack_into("<h", tags.data, animations + 36, 99)
    block = compressed_animation_block(2, [3, 6], 2, gap="animation_compressed_gap" in defects)
    _animation(tags, animations + 180, 0, 6, 2, 0b11, 0b01, 0, 1, compressed=block)
    _animation(tags, animations + 360, 1, 1, 0, 0, 0, 0, 0)
    rows.append((group_word("antr"), tags.address(graph)))
    # -- font: two character tables, three characters (one blank), 64 pixel bytes
    font = tags.alloc(156)
    struct.pack_into("<Ihhhh", tags.data, font, 1, 12, 3, 2, 1)
    tables = _block(tags, font + 48, 2, 12)
    for t in range(2):
        entries = _block(tags, tables + 12 * t, 4, 2)
        struct.pack_into("<4h", tags.data, entries, 0, 1, -1, 2 - t)
    characters = _block(tags, font + 124, 3, 20)
    glyphs = [(0x41, 6, 4, 8, 0, 8, -1, 0, 0), (0x20, 3, -2, 0, 2, -5, -1, 0, 0),
              (0x3A9, 7, 5, 6, 1, 7, -1, 0, 64 if "font_glyph" in defects else 32)]
    for i, glyph in enumerate(glyphs):
        struct.pack_into("<Hhhhhhhhi", tags.data, characters + 20 * i, *glyph)
    pixels = tags.alloc(64)
    tags.data[pixels:pixels + 64] = pattern(64, 77)
    struct.pack_into("<iIiI", tags.data, font + 136, 64, 0, 0, tags.address(pixels))
    rows.append((group_word("font"), tags.address(font)))
    # -- unicode string list and HUD message text
    strings = ["Halo éΩ\0", "two\0"]
    if "string_odd" in defects:
        strings[1] = "two"
    ustr = tags.alloc(12)
    entries = _block(tags, ustr, len(strings), 20)
    for i, text in enumerate(strings):
        raw = text.encode("utf-16-le") + (b"x" if "string_odd" in defects and i == 1 else b"")
        offset = tags.alloc(len(raw), 2)
        tags.data[offset:offset + len(raw)] = raw
        struct.pack_into("<iIiI", tags.data, entries + 20 * i, len(raw), 0, 0, tags.address(offset))
    rows.append((group_word("ustr"), tags.address(ustr)))
    hmt = tags.alloc(128)
    text = "hud\0text\0".encode("utf-16-le")
    offset = tags.alloc(len(text), 2)
    tags.data[offset:offset + len(text)] = text
    struct.pack_into("<iIiI", tags.data, hmt, len(text), 0, 0, tags.address(offset))
    rows.append((group_word("hmt "), tags.address(hmt)))
    # -- environment shader (parameters in place)
    shader = tags.alloc(836)
    rows.append((group_word("senv"), 0x7FFFFFF0 if "shader_root" in defects else tags.address(shader)))
    # -- structure BSP in the map body, loaded at its own base
    bsp_ordinal = first_ordinal + len(rows)
    space = _Tags(base=0x81200000, reserved=24)
    root = space.alloc(648)
    struct.pack_into("<6I", space.data, 0, space.address(root), 0, 0, 0, 0, group_word("sbsp"))
    struct.pack_into("<I8xI", space.data, root, group_word("bitm"), 0xE1740001)
    collision = _block(space, root + 176, 1, 96)
    _collision_bsp(space, collision, defects)
    surfaces = [(0, 1, 2), (2, 1, 3), (0, 2, 1)]
    surface_offset = _block(space, root + 248, len(surfaces), 6)
    for i, tri in enumerate(surfaces):
        struct.pack_into("<3H", space.data, surface_offset + 6 * i, *tri)
    lightmap = _block(space, root + 260, 1, 32)
    struct.pack_into("<hH", space.data, lightmap, 0, 0)
    materials = _block(space, lightmap + 20, 2, 256)
    for m, (first, count, vcount, lcount) in enumerate(((0, 2, 4, 4), (2, 1, 3, 0))):
        material = materials + 256 * m
        struct.pack_into("<I8xI", space.data, material, group_word("senv"), 0xE1740000 | (first_ordinal + 6))
        struct.pack_into("<hHii3f", space.data, material + 16, 0, 0, first, count, 1.0, 2.0, 3.0)
        struct.pack_into("<3f", space.data, material + 40, 0.1, 0.2, 0.3)
        struct.pack_into("<hH", space.data, material + 52, 1, 0)
        struct.pack_into("<4f", space.data, material + 156, 0, 0, 1, 4)
        struct.pack_into("<h", space.data, material + 172, -1)
        struct.pack_into("<hHi", space.data, material + 176, 1, 0, vcount)
        struct.pack_into("<hHi", space.data, material + 196, 3 if lcount else 0, 0, lcount)
        size = 32 * vcount + 8 * lcount + (4 if "bsp_data_size" in defects else 0)
        data = space.alloc(size)
        space.data[data:data + size] = pattern(size, 90 + m)
        struct.pack_into("<iIiI", space.data, material + 236, size, 0, 0, space.address(data))
    while len(body) % 512:
        body.append(0)
    file_offset = len(body)
    body += space.data
    references = _block(tags, scenario + 0x5A4, 1, 32)
    struct.pack_into("<iiII", tags.data, references, file_offset, len(space.data), space.base, 0)
    struct.pack_into("<I8xI", tags.data, references + 16, group_word("sbsp"), 0xE1740000 | bsp_ordinal)
    rows.append((group_word("sbsp"), 0))
    return rows, (2, tags.address(vtable), 2, tags.address(itable))


def write_staging(directory, maps):
    """Write maps/<name>.map and a staging.json listing their hashes."""
    directory = Path(directory)
    (directory / "maps").mkdir(parents=True, exist_ok=True)
    files = []
    for name, blob in sorted(maps.items()):
        (directory / "maps" / (name + ".map")).write_bytes(blob)
        files.append({"path": name + ".map", "size": len(blob), "sha256": hashlib.sha256(blob).hexdigest()})
    record = {"scope": "authored synthetic fixture", "image_sha256": "0" * 64, "status": "complete", "files": files}
    (directory / "staging.json").write_text(json.dumps(record, indent=2), encoding="ascii")
    return directory
