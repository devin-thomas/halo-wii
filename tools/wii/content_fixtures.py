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

    def __init__(self):
        self.data = bytearray(36)

    def alloc(self, size, align=4):
        while len(self.data) % align:
            self.data.append(0)
        offset = len(self.data)
        self.data += bytes(size)
        return offset

    def address(self, offset):
        return BASE + offset

    def string(self, text):
        offset = self.alloc(len(text) + 1, 1)
        self.data[offset:offset + len(text)] = text.encode("ascii")
        return offset


def build_map(name="fixture", category=0, bitmaps=None, animation_streams=None, sounds=None, mutate=None):
    """Return stored map bytes for an authored minimal Xbox v5 cache.

    bitmaps: list of (format, width, height, depth, type, mipmap_count, linear)
    animation_streams: list of (version, unit_control_version, ticks, stream bytes)
    sounds: list of (rate_id, encoding, compression, payload bytes)
    mutate: optional callable(state dict) applied before compression to author malformed cases.
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
