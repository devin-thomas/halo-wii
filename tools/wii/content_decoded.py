"""Host reference of the Wii loaders' decoded forms and digests (HWI-008E).

The C loaders in port/wii/content (hwm_model.c, hwl_lightmap.c,
hwc_collision.c, hma_graph.c, hwf_font.c, hus_strings.c) decode each
sectioned container into native records and expose a canonical digest: the
SHA-256 of every decoded field serialized little-endian, in the order the C
header documents. This module computes the same digest independently from the
container bytes, so a guest run can be checked against the host.

Decoded values beyond a byte-order change:
  packed vectors  11:11:10 normal, binormal, tangent and incident radiosity,
                  expanded by the engine's uncompress_int32_to_real_vector3d:
                  each component (2c + 1) * f32(1 / (2^bits - 1)), rounded once
                  to f32. Python computes the product in double, which is exact
                  (24 + 12 bits), then rounds to f32, so the bits equal the C
                  single-precision result.
Everything else is kept as stored.

For model animations, unicode string lists and fonts the decoded data can
also be tied to the converter's own source hash (source_identity), which is
the SHA-256 of the original Xbox bytes.
"""
import hashlib
import struct

import be_records as br
import content_animation as ca
import content_geometry as cg
import content_text as ct

_PACK_F32 = struct.Struct("<f")


def f32(value):
    return _PACK_F32.unpack(_PACK_F32.pack(value))[0]


INV_2047 = f32(1.0 / 2047.0)
INV_1023 = f32(1.0 / 1023.0)


def _signed(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def unpack_vector(packed):
    """The engine's 11:11:10 expansion, as f32 values."""
    i = _signed(packed & 0x7FF, 11)
    j = _signed((packed >> 11) & 0x7FF, 11)
    k = _signed((packed >> 22) & 0x3FF, 10)
    return f32((2 * i + 1) * INV_2047), f32((2 * j + 1) * INV_2047), f32((2 * k + 1) * INV_1023)


def _le(view, fmt, count):
    return br.to_little_endian(bytes(view), fmt, count)


def _rows(values, width):
    return [values[i:i + width] for i in range(0, len(values), width)]


# ---- HWM1 -------------------------------------------------------------------------
def model_digest(blob):
    cg.unpack_model(blob)
    s = br.unpack_container(blob, b"HWM1", cg.HWM_SPEC)
    sha = hashlib.sha256(_le(s[1][1], cg.PART_FMT, s[1][0]))
    vertices = br.decode_records(s[2][1], cg.MODEL_VERTEX_FMT, s[2][0], ">")
    out = bytearray()
    pack = struct.Struct("<III9fhhBBh").pack
    for v in _rows(vertices, 11):
        out += pack(v[0], v[1], v[2], *unpack_vector(v[3]), *unpack_vector(v[4]), *unpack_vector(v[5]), *v[6:])
        if len(out) > 1 << 16:
            sha.update(out)
            out.clear()
    sha.update(out)
    sha.update(_le(s[3][1], cg.INDEX_FMT, s[3][0]))
    return sha.hexdigest(), {"parts": s[1][0], "vertices": s[2][0], "indices": s[3][0]}


# ---- HWL1 -------------------------------------------------------------------------
def lightmap_digest(blob):
    s = cg.unpack_lightmaps(blob)
    sha = hashlib.sha256(_le(s[1][1], cg.BSP_FMT, 1))
    sha.update(_le(s[2][1], cg.LIGHTMAP_FMT, s[2][0]))
    sha.update(_le(s[3][1], cg.MATERIAL_FMT, s[3][0]))
    out = bytearray()
    pack = struct.Struct("<III9fII").pack
    for v in _rows(br.decode_records(s[4][1], cg.ENV_VERTEX_FMT, s[4][0], ">"), 8):
        out += pack(v[0], v[1], v[2], *unpack_vector(v[3]), *unpack_vector(v[4]), *unpack_vector(v[5]), v[6], v[7])
    sha.update(out)
    out = bytearray()
    pack = struct.Struct("<3fhh").pack
    for v in _rows(br.decode_records(s[5][1], cg.LIGHTMAP_VERTEX_FMT, s[5][0], ">"), 3):
        out += pack(*unpack_vector(v[0]), v[1], v[2])
    sha.update(out)
    sha.update(_le(s[6][1], cg.SURFACE_FMT, s[6][0]))
    return sha.hexdigest(), {"lightmaps": s[2][0], "materials": s[3][0], "vertices": s[4][0],
                             "lightmap_vertices": s[5][0], "surfaces": s[6][0]}


# ---- HWC1 -------------------------------------------------------------------------
def collision_digest(blob):
    s = cg.unpack_collision(blob)
    sha = hashlib.sha256(_le(s[1][1], cg.BSPS_FMT, s[1][0]))
    for k, (_, fmt, _) in enumerate(cg.COLLISION_ARRAYS):
        sha.update(_le(s[k + 2][1], fmt, s[k + 2][0]))
    return sha.hexdigest(), {"bsps": s[1][0], "elements": sum(s[k + 2][0] for k in range(8))}


# ---- HMA1 -------------------------------------------------------------------------
def _flag_counts(row):
    nodes, translation, rotation, scale = row[5], row[9:11], row[11:13], row[13:15]
    return (ca._flag_count(rotation, nodes), ca._flag_count(translation, nodes), ca._flag_count(scale, nodes))


def graph_parts(blob):
    """Per animation: (record values, little-endian frame info, defaults, frames, compressed block)."""
    rows = ca.unpack_graph(blob)
    s = br.unpack_container(blob, b"HMA1", ca.HMA_SPEC)
    parts = []
    for row in rows:
        frame_size, nodes, flags = row[3], row[5], row[6]
        translation, rotation, scale = row[9:11], row[11:13], row[13:15]
        spans = row[16:24]
        info = _le(s[2][1][spans[0]:spans[0] + spans[1]], "I", spans[1] // 4)
        default_fmt = ca._node_layout(translation, rotation, scale, nodes, False)
        frame_fmt = ca._node_layout(translation, rotation, scale, nodes, True)
        defaults = _le(s[3][1][spans[2]:spans[2] + spans[3]], default_fmt, 1) if spans[3] else b""
        frame_view = s[4][1][spans[4]:spans[4] + spans[5]]
        count = spans[5] // frame_size if frame_size else 0
        frame_bytes = _le(frame_view, frame_fmt, count) if spans[5] else b""
        block = b""
        if flags & ca.COMPRESSED_BIT:
            big = bytes(s[5][1][spans[6]:spans[6] + spans[7]])
            little = bytearray(len(big))
            for offset, n, fmt in ca._compressed_spans_be(big, nodes, _flag_counts(row)):
                size = n * br.record_size(fmt)
                little[offset:offset + size] = br.to_little_endian(big[offset:offset + size], fmt, n)
            block = bytes(little)
        parts.append((row, info, defaults, frame_bytes, block))
    return parts


def graph_digest(blob):
    sha = hashlib.sha256()
    facts = {"animations": 0, "compressed": 0, "expanded_bytes": 0}
    for row, info, defaults, frames, block in graph_parts(blob):
        sha.update(struct.pack("<" + ca.ANIMATION_FMT, *row) + info + defaults + frames + block)
        facts["animations"] += 1
        if row[6] & ca.COMPRESSED_BIT:
            facts["compressed"] += 1
            if not frames:
                facts["expanded_bytes"] += row[2] * row[3]
    return sha.hexdigest(), facts


def graph_source_identity(blob):
    """SHA-256 of the rebuilt Xbox frame info, defaults and data: the converter's source hash."""
    sha = hashlib.sha256()
    for _, info, defaults, frames, block in graph_parts(blob):
        sha.update(info + defaults + frames + block)
    return sha.hexdigest()


# ---- HWF1 -------------------------------------------------------------------------
def font_digest(blob):
    head, s = ct.unpack_font(blob)
    indices = _le(s[3][1], "h", s[3][0])
    characters = _le(s[4][1], ct.CHARACTER_FMT, s[4][0])
    if any(not -1 <= v < s[4][0] for v in br.decode_records(s[3][1], "h", s[3][0], ">")):
        raise ct.TextError("character index outside the font")
    sha = hashlib.sha256(struct.pack("<" + ct.FONT_FMT, *head))
    sha.update(_le(s[2][1], ct.TABLE_FMT, s[2][0]))
    sha.update(indices)
    sha.update(characters)
    sha.update(bytes(s[5][1]))
    return sha.hexdigest(), {"tables": s[2][0], "characters": s[4][0], "pixel_bytes": s[5][0]}


def font_source_identity(blob):
    head, s = ct.unpack_font(blob)
    sha = hashlib.sha256(struct.pack("<Ihhhh", *head[:5]))
    sha.update(_le(s[4][1], ct.CHARACTER_FMT, s[4][0]))
    sha.update(bytes(s[5][1]))
    sha.update(_le(s[3][1], "h", s[3][0]))
    return sha.hexdigest()


# ---- HUS1 -------------------------------------------------------------------------
def strings_digest(blob):
    strings = ct.unpack_strings(blob)
    sha = hashlib.sha256()
    for units in strings:
        sha.update(struct.pack("<I%dH" % len(units), 2 * len(units), *units))
    return sha.hexdigest(), {"strings": len(strings), "code_units": sum(len(u) for u in strings)}


def strings_source_identity(blob, group):
    """The converter's source hash: length-prefixed strings for a unicode
    string list, the bare text for a HUD message text."""
    strings = ct.unpack_strings(blob)
    if group.strip() == "hmt":
        return hashlib.sha256(b"".join(struct.pack("<%dH" % len(u), *u) for u in strings)).hexdigest()
    return strings_digest(blob)[0]


DIGESTS = {"model": model_digest, "lightmap": lightmap_digest, "collision": collision_digest,
           "model_animation": graph_digest, "font": font_digest, "strings": strings_digest}
