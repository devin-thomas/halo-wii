"""Stage converted content for the Wii loader self-test (HWI-008C, HWI-008E).

Picks a deterministic sample of a published content generation (every
texture format and type, sound codec class and recorded-animation version;
for models, lightmap geometry, collision, model animations, fonts and
unicode text the smallest, median and largest file and the extremes: the
largest collision model and structure BSP, compressed animations, the font
with the most glyphs, the longest string list and a HUD text), computes the
SHA-256 of each file's decoded contents with the host decoders, derives
malformed and truncated variants per format and wrong-format controls, and
writes:

  <out>/sd/halo-wii-content/cases.txt   case list for the guest (sd: paths)
  <out>/sd/halo-wii-content/c/<id>.*    the sampled and malformed files
  <out>/cases-host.txt                  the same list with local paths (host_check)
  <out>/stage.txt                       LOCAL=sd:/path lines for run_dolphin.py --stage
  <out>/cases-private.json              sample identity (map, tag) - private
  <out>/cases-public.json               formats, sizes and expectations only

Decoded digests:
  texture    RGBA8 of every texel of every image (face, level, slice; rows), GX decode rules
  animation  the rebuilt Xbox event stream (equals the manifest's source stream hash)
  sound      decoded samples as big-endian PCM16, interleaved
  model, lightmap, collision, model_animation, font, strings
             the loaders' canonical digest of the decoded form (content_decoded.py);
             model animations, unicode string lists and fonts are also tied to
             the manifest's source hash

The output directory must be new and outside the checkout; nothing here is
published. Exit status 0 on success, 1 on any failure.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import be_records as br  # noqa: E402
import content_animation as ca  # noqa: E402
import content_decoded as cd  # noqa: E402
import content_geometry as cg  # noqa: E402
import content_text as ct  # noqa: E402
import gx_texture as gt  # noqa: E402
import media_formats as mf  # noqa: E402
import recorded_animation as ra  # noqa: E402

CHECKOUT = Path(__file__).resolve().parents[2]
SD_DIR = "halo-wii-content"
MAX_FILE_BYTES = 8 * 1024 * 1024
TEXTURE_TEXEL_BUDGET = 400_000
SOUND_BYTE_BUDGET = 400_000
CHUNK_CASES = 200
EXTENSIONS = {"texture": "hwt", "animation": "hra", "sound": "hws", "model": "hwm", "lightmap": "hwl",
              "collision": "hwc", "model_animation": "hma", "font": "hwf", "strings": "hus"}
PREFIX = {"texture": "t", "animation": "a", "sound": "s", "model": "g", "lightmap": "l", "collision": "c",
          "model_animation": "n", "font": "f", "strings": "u"}
# loader kind -> manifest kind for the HWI-008C sectioned containers
DECODED_KINDS = {"model": "model", "lightmap": "lightmap_geometry", "collision": "collision",
                 "model_animation": "model_animation", "font": "font", "strings": "strings"}
KINDS = ("texture", "animation", "sound") + tuple(DECODED_KINDS)


class CaseError(ValueError):
    pass


def external(path):
    resolved = Path(path).resolve()
    if resolved == CHECKOUT or CHECKOUT in resolved.parents:
        raise CaseError("output must be outside the checkout")
    return resolved


def sha(data):
    return hashlib.sha256(bytes(data)).hexdigest()


# ---- decoded digests --------------------------------------------------------
def hwt_images(header):
    """(face, level, slice, width, height, offset, bytes) in container order."""
    images, offset = [], gt.HWT_HEADER_BYTES
    gx = header["gx_format"]
    for face in range(header["faces"]):
        for level in range(header["levels"]):
            w, h = max(header["width"] >> level, 1), max(header["height"] >> level, 1)
            size = gt.gx_level_bytes(gx, w, h)
            for z in range(max(header["depth"] >> level, 1)):
                images.append((face, level, z, w, h, offset, size))
                offset += size
    return images


def texture_digest(blob):
    header, _ = gt.unpack_hwt(blob)
    digest = hashlib.sha256()
    for _, _, _, w, h, offset, size in hwt_images(header):
        image = bytes(blob[offset:offset + size])
        row = bytearray()
        for y in range(h):
            for x in range(w):
                row += bytes(gt.gx_decode_texel(header["gx_format"], image, w, x, y))
            digest.update(row)
            row.clear()
    return digest.hexdigest()


def texels(header):
    return sum(w * h for _, _, _, w, h, _, _ in hwt_images(header))


def sound_digest(blob):
    codec, channels = struct.unpack_from(">H", blob, 6)[0], struct.unpack_from(">H", blob, 12)[0]
    payload = bytes(blob[32:])
    if codec == 2:
        return sha(payload)
    pcm = mf.decode_xbox_adpcm(payload, channels)
    return sha(struct.pack(">%dh" % len(pcm), *pcm))


def animation_digest(blob):
    return sha(ra.encode_xbox(ra.unpack_canonical(bytes(blob))))


DIGESTS = {"texture": texture_digest, "animation": animation_digest, "sound": sound_digest}
DIGESTS.update({kind: (lambda blob, kind=kind: cd.DIGESTS[kind](blob)[0]) for kind in DECODED_KINDS})


def source_identity(kind, blob, source):
    """For kinds whose decoded data rebuilds the Xbox bytes: whether it matches
    the converter's source hash (None when the kind has no such identity)."""
    if kind == "model_animation":
        return cd.graph_source_identity(blob) == source["source_sha256"]
    if kind == "font":
        return cd.font_source_identity(blob) == source["source_sha256"]
    if kind == "strings":
        return cd.strings_source_identity(blob, source.get("group", "")) == source["source_sha256"]
    return None


# ---- sample selection ---------------------------------------------------------
def select(outputs, generation_dir):
    """Deterministic sample: (kind, entry) pairs."""
    chosen = []

    def pick(group, budget_key):
        within = [e for e in group if e["bytes"] <= MAX_FILE_BYTES and budget_key(e)]
        within.sort(key=lambda e: (e["bytes"], e["path"]))
        if not within:
            return []
        picks = [within[0]]
        if len(within) > 2:
            picks.append(within[len(within) // 2])
        return picks

    textures = {}
    for e in outputs:
        if e["kind"] == "texture":
            textures.setdefault((e["source"]["format"], e["source"]["type"]), []).append(e)

    def texture_budget(e):
        s = e["source"]
        total = sum(max(s["width"] >> l, 1) * max(s["height"] >> l, 1) * max(s["depth"] >> l, 1)
                    for l in range(s["levels"]))
        return total * (6 if s["type"] == "cube" else 1) <= TEXTURE_TEXEL_BUDGET

    for key in sorted(textures):
        group = textures[key]
        mipped = [e for e in group if e["source"]["levels"] > 1]
        chosen += [("texture", e) for e in (pick(mipped, texture_budget) or pick(group, texture_budget))]
    oversize = [e for e in outputs if e["kind"] == "texture" and not e["gx"]["loadable_dimensions"]
                and e["bytes"] <= MAX_FILE_BYTES]
    if oversize:
        chosen.append(("texture", min(oversize, key=lambda e: (e["bytes"], e["path"]))))
    sounds = {}
    for e in outputs:
        if e["kind"] == "sound":
            s = e["source"]
            sounds.setdefault((s["codec"], s["channels"], s["rate"]), []).append(e)
    for key in sorted(sounds):
        chosen += [("sound", e) for e in pick(sounds[key], lambda e: e["bytes"] <= SOUND_BYTE_BUDGET)]
    animations = {}
    for e in outputs:
        if e["kind"] == "animation":
            s = e["source"]
            animations.setdefault((s["version"], s["unit_control_version"]), []).append(e)
    for key in sorted(animations):
        group = sorted(animations[key], key=lambda e: (e["bytes"], e["path"]))
        chosen.append(("animation", group[0]))
        if len(group) > 1:
            chosen.append(("animation", group[-1]))
    seen, unique = set(), []
    for kind, e in chosen:
        if e["path"] not in seen:
            seen.add(e["path"])
            unique.append((kind, e))
    return unique


def select_decoded(outputs):
    """Deterministic sample of the sectioned containers: (kind, entry, roles)."""
    chosen = []
    for kind, manifest_kind in DECODED_KINDS.items():
        group = sorted((e for e in outputs if e["kind"] == manifest_kind and e["bytes"] <= MAX_FILE_BYTES),
                       key=lambda e: (e["bytes"], e["path"]))
        if not group:
            continue
        roles = [("smallest", group[0]), ("median", group[len(group) // 2]), ("largest", group[-1])]
        if kind == "collision":
            for code, role in (("coll", "largest_collision_model"), ("sbsp", "largest_structure_bsp")):
                sub = [e for e in group if e["source"].get("group") == code]
                if sub:
                    roles.append((role, sub[-1]))
        elif kind == "model_animation":
            compressed = [e for e in group if e["source"].get("compressed")]
            if compressed:
                roles += [("compressed_smallest", compressed[0]), ("compressed_largest", compressed[-1]),
                          ("most_compressed", max(compressed, key=lambda e: (e["source"]["compressed"], e["path"])))]
        elif kind == "font":
            roles.append(("most_glyphs", max(group, key=lambda e: (e["source"]["characters"], e["path"]))))
        elif kind == "strings":
            roles.append(("longest_list", max(group, key=lambda e: (e["source"]["code_units"], e["path"]))))
            roles.append(("most_strings", max(group, key=lambda e: (e["source"]["strings"], e["path"]))))
            hud = [e for e in group if e["source"].get("group", "").strip() == "hmt"]
            if hud:
                roles.append(("hud_text", hud[-1]))
        merged = {}
        for role, e in roles:
            merged.setdefault(e["path"], (e, []))[1].append(role)
        chosen += [(kind, e, names) for e, names in sorted(merged.values(),
                                                            key=lambda v: (v[0]["bytes"], v[0]["path"]))]
    return chosen


# ---- malformed variants ---------------------------------------------------------
def put16(blob, offset, value):
    struct.pack_into(">H", blob, offset, value & 0xFFFF)


def put32(blob, offset, value):
    struct.pack_into(">I", blob, offset, value & 0xFFFFFFFF)


def texture_variants(blob):
    """(name, bytes, expected error) for one valid 2D texture with mip levels."""
    b = bytes(blob)
    header, _ = gt.unpack_hwt(b)
    out = [("header_truncated", b[:40], "truncated"), ("data_truncated", b[:-32], "length"),
           ("trailing_bytes", b + bytes(32), "length")]

    def edit(name, error, fn):
        m = bytearray(b)
        fn(m)
        out.append((name, bytes(m), error))
    edit("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HWX1"))
    edit("bad_version", "version", lambda m: put16(m, 4, 2))
    edit("gx_format_unknown", "enum", lambda m: put16(m, 6, 7))
    wrong = gt.GX_RGBA8 if header["gx_format"] != gt.GX_RGBA8 else gt.GX_I8
    edit("gx_format_against_profile", "enum", lambda m: put16(m, 6, wrong))
    edit("width_zero", "dimensions", lambda m: put16(m, 8, 0))
    edit("levels_above_chain", "dimensions", lambda m: put16(m, 16, 20))
    edit("levels_short", "count", lambda m: put16(m, 16, header["levels"] - 1))
    edit("images_extra", "count", lambda m: put16(m, 18, header["images"] + 1))
    edit("data_bytes_extra", "length", lambda m: put32(m, 28, len(b) - 64 + 32))
    edit("reserved_nonzero", "reserved", lambda m: m.__setitem__(40, 1))
    edit("type_unknown", "enum", lambda m: put16(m, 22, 7))
    return out


def animation_variants(blob):
    b = bytes(blob)
    decoded = ra.unpack_canonical(b)
    fields, events = len(decoded["unit_control"]), len(decoded["events"])
    events_at = ra.HRA_HEADER.size + 4 * (fields + len(decoded["state"]))
    out = [("header_truncated", b[:20], "truncated"), ("events_truncated", b[:-16 - len(decoded["tail"])],
                                                                        "truncated"),
           ("trailing_bytes", b + bytes(4), "length")]

    def edit(name, error, fn):
        m = bytearray(b)
        fn(m)
        out.append((name, bytes(m), error))
    edit("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HRX1"))
    edit("bad_animation_version", "version", lambda m: m.__setitem__(6, 9))
    edit("field_count_extra", "count", lambda m: put16(m, 10, fields + 1))
    edit("total_ticks_off", "count", lambda m: put32(m, 20, decoded["total_ticks"] + 1))
    edit("unknown_event_type", "event", lambda m: m.__setitem__(events_at, 63))
    edit("end_event_not_last", "event", lambda m: m.__setitem__(events_at + 16 * (events - 1), 0))
    current = decoded["codec"] == ra.CODEC_CURRENT
    for index, (event_type, kind, delta, payload) in enumerate(decoded["events"]):
        codes = (ra.CURRENT_PAYLOADS if current else ra.V1_PAYLOADS)[event_type]
        at = events_at + 16 * index
        if len(codes) < 3:
            edit("unused_payload_word", "reserved", lambda m, at=at, n=len(codes): put32(m, at + 4 + 4 * n, 1))
            break
    for index, (event_type, kind, delta, payload) in enumerate(decoded["events"]):
        codes = (ra.CURRENT_PAYLOADS if current else ra.V1_PAYLOADS)[event_type]
        if codes and codes[0] in ("u8", "s8", "s16", "u16"):
            at = events_at + 16 * index
            edit("payload_outside_width", "value", lambda m, at=at: put32(m, at + 4, 0x12345))
            break
    if current:
        for index, (event_type, kind, delta, payload) in enumerate(decoded["events"]):
            if kind == 1:
                at = events_at + 16 * index
                edit("delta_against_kind", "delta", lambda m, at=at: put16(m, at + 2, 7))
                break
    # A float unit-control word changed: still a valid record, but the
    # rebuilt stream no longer matches the recorded source tag.
    float_index = next(i for i, (code, _) in enumerate(ra.unit_control_fields(decoded["unit_control_version"]))
                       if code == "f32")
    edit("source_tag_mismatch", "identity",
         lambda m: put32(m, ra.HRA_HEADER.size + 4 * float_index,
                         struct.unpack_from(">I", m, ra.HRA_HEADER.size + 4 * float_index)[0] ^ 1))
    return out


def sound_variants(blob):
    b = bytes(blob)
    channels = struct.unpack_from(">H", b, 12)[0]
    frames = struct.unpack_from(">I", b, 16)[0]
    out = [("header_truncated", b[:24], "truncated"), ("payload_truncated", b[:-1], "length"),
           ("trailing_bytes", b + bytes(36 * channels), "length")]

    def edit(name, error, fn, base=b):
        m = bytearray(base)
        fn(m)
        out.append((name, bytes(m), error))
    edit("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HWX1"))
    edit("bad_version", "version", lambda m: put16(m, 4, 0))
    edit("codec_unknown", "enum", lambda m: put16(m, 6, 3))
    edit("rate_unknown", "enum", lambda m: put32(m, 8, 32000))
    edit("channels_three", "enum", lambda m: put16(m, 12, 3))
    edit("reserved_nonzero", "reserved", lambda m: put16(m, 14, 1))
    edit("frames_extra", "count", lambda m: put32(m, 16, frames + 1))
    edit("partial_block", "count", lambda m: put32(m, 20, len(b) - 32 - 1), base=b[:-1])
    return out


def _table(b):
    return [br.SECTION.unpack_from(b, br.HEADER.size + br.SECTION.size * i)
            for i in range(struct.unpack_from(">H", b, 8)[0])]


def _record(b, section_id, index, fmt, field=0):
    """Byte offset of one field of one record (formats as be_records, no padding)."""
    _, record_bytes, _, offset, _ = _table(b)[section_id - 1]
    return offset + record_bytes * index + struct.calcsize("<" + fmt[:field])


def _get(b, section_id, index, fmt, field):
    return struct.unpack_from(">" + fmt[field], b, _record(b, section_id, index, fmt, field))[0]


def _set(m, section_id, index, fmt, field, value):
    struct.pack_into(">" + fmt[field], m, _record(m, section_id, index, fmt, field), value)


def _edits(b, specs, out):
    for name, error, fn in specs:
        m = bytearray(b)
        fn(m)
        out.append((name, bytes(m), error))
    return out


def container_variants(b):
    """Header and section-table defects shared by every sectioned container."""
    table = _table(b)
    out = [("header_truncated", b[:20], "truncated"), ("trailing_bytes", b + bytes(32), "length")]
    specs = [("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HWZ1")),
             ("bad_version", "version", lambda m: put16(m, 4, 2)),
             ("flags_nonzero", "reserved", lambda m: put16(m, 10, 1)),
             ("total_bytes_off", "length", lambda m: put32(m, 12, len(b) + 32)),
             ("section_count_off", "section", lambda m: put16(m, 8, len(table) + 1)),
             ("record_size_off", "section", lambda m: put16(m, br.HEADER.size + 2, table[0][1] + 1)),
             ("section_offset_off", "section", lambda m: put32(m, br.HEADER.size + 8, table[0][3] + 32))]
    padded = [offset + size for _, _, _, offset, size in table if size % 32]
    if padded:
        specs.append(("section_padding_nonzero", "reserved", lambda m: m.__setitem__(padded[0], 1)))
    return _edits(b, specs, out)


def _model_part(b):
    rows, _, _ = cg.unpack_model(b)
    return next((i for i, r in enumerate(rows) if r[15] and r[17]), None)


def model_variants(b):
    fmt, p = cg.PART_FMT, _model_part(b)
    vertex_count, first_index = _get(b, 1, p, fmt, 15), _get(b, 1, p, fmt, 18)
    return _edits(b, [
        ("vertex_type_unknown", "enum", lambda m: _set(m, 1, 0, fmt, 13, 4)),
        ("strip_type_unknown", "enum", lambda m: _set(m, 1, 0, fmt, 14, 2)),
        ("part_order", "range", lambda m: _set(m, 1, 0, fmt, 1, 1)),
        ("vertex_range_gap", "range", lambda m: _set(m, 1, 0, fmt, 16, 1)),
        ("index_past_vertices", "range", lambda m: _set(m, 3, first_index, "H", 0, vertex_count))],
        container_variants(b))


def _materials(b):
    s = cg.unpack_lightmaps(b)
    width = len(struct.unpack("<" + cg.MATERIAL_FMT, bytes(cg.HWL_SPEC[3])))
    values = br.decode_records(s[3][1], cg.MATERIAL_FMT, s[3][0], ">")
    rows = [values[i * width:(i + 1) * width] for i in range(s[3][0])]
    # (first surface 4, surface count 5, vertex count 48)
    return s, rows, next((i for i, r in enumerate(rows) if r[5] > 0 and r[48] > 0), None)


def lightmap_variants(b):
    fmt = cg.MATERIAL_FMT
    s, rows, k = _materials(b)
    return _edits(b, [
        ("bsp_counts_off", "count", lambda m: _set(m, 1, 0, cg.BSP_FMT, 2, s[3][0] + 1)),
        ("lightmap_material_gap", "range", lambda m: _set(m, 2, 0, cg.LIGHTMAP_FMT, 2, 1)),
        ("material_vertex_type", "enum", lambda m: _set(m, 3, 0, fmt, 46, 2)),
        ("material_surfaces_outside", "range", lambda m: _set(m, 3, k, fmt, 5, s[6][0] + 1)),
        ("surface_index_past", "range", lambda m: _set(m, 6, rows[k][4], cg.SURFACE_FMT, 0, rows[k][48]))],
        container_variants(b))


def _first_bsp(b):
    s = cg.unpack_collision(b)
    counts = br.decode_records(s[1][1][:cg.HWC_SPEC[1]], cg.BSPS_FMT, 1, ">")[3::2]
    leaves = br.decode_records(s[4][1][:8 * counts[2]], "Hhi", counts[2], ">")
    return counts, [i for i in range(counts[2]) if leaves[3 * i + 1] > 0]


def collision_variants(b):
    counts, leaves = _first_bsp(b)
    return _edits(b, [
        ("bsp_range_gap", "range", lambda m: _set(m, 1, 0, cg.BSPS_FMT, 2, 1)),
        ("node_child_outside", "range", lambda m: _set(m, 2, 0, "iii", 1, counts[0])),
        ("leaf_reference_outside", "range", lambda m: _set(m, 4, leaves[0], "Hhi", 2, counts[3])),
        ("surface_plane_outside", "range", lambda m: _set(m, 7, 0, "iiBbh", 0, counts[1])),
        ("edge_vertex_outside", "range", lambda m: _set(m, 8, 0, "iiiiii", 0, counts[7]))],
        container_variants(b))


def _compressed(b):
    rows = ca.unpack_graph(b)
    return rows, next((i for i, r in enumerate(rows) if r[6] & ca.COMPRESSED_BIT), None)


def graph_variants(b):
    fmt = ca.ANIMATION_FMT
    rows, c = _compressed(b)
    block_at = _table(b)[4][3] + rows[c][22]
    header = struct.unpack_from(">11i", b, block_at)
    block = bytes(b[block_at:block_at + rows[c][23]])
    word = None
    for k in (0, 1, 2, 4, 5, 6, 8, 9, 10, 3, 7):
        # the first header word whose move by 2 breaks the block's exact tiling
        moved = bytearray(block)
        struct.pack_into(">i", moved, 4 * k, header[k] + 2)
        try:
            ca._compressed_spans_be(bytes(moved), rows[c][5], cd._flag_counts(rows[c]))
        except ca.AnimationDataError:
            word = k
            break
    return _edits(b, [
        ("index_wrong", "range", lambda m: _set(m, 1, 0, fmt, 0, 7)),
        ("pad_nonzero", "reserved", lambda m: _set(m, 1, 0, fmt, 7, 1)),
        ("node_count_above", "count", lambda m: _set(m, 1, 0, fmt, 5, 65)),
        ("frame_info_type_unknown", "enum", lambda m: _set(m, 1, 0, fmt, 4, 4)),
        ("frame_size_off", "count", lambda m: _set(m, 1, 0, fmt, 3, rows[0][3] + 4)),
        ("frame_info_offset_off", "range", lambda m: _set(m, 1, 0, fmt, 16, 4)),
        ("compressed_array_moved", "range", lambda m: put32(m, block_at + 4 * word, header[word] + 2))],
        container_variants(b))


def _glyph(b):
    head, s = ct.unpack_font(b)
    chars = br.decode_records(s[4][1], ct.CHARACTER_FMT, s[4][0], ">")
    glyph = next((i for i in range(head[6]) if chars[9 * i + 2] > 0 and chars[9 * i + 3] > 0), None)
    return head, (glyph if head[5] >= 2 else None)


def font_variants(b):
    head, glyph = _glyph(b)
    second = _get(b, 2, 1, ct.TABLE_FMT, 0)
    return _edits(b, [
        ("font_counts_off", "count", lambda m: _set(m, 1, 0, ct.FONT_FMT, 6, head[6] + 1)),
        ("table_gap", "range", lambda m: _set(m, 2, 1, ct.TABLE_FMT, 0, second + 1)),
        ("table_too_long", "count", lambda m: _set(m, 2, 0, ct.TABLE_FMT, 1, 257)),
        ("index_outside", "range", lambda m: _set(m, 3, 0, "h", 0, head[6])),
        ("glyph_outside", "range", lambda m: _set(m, 4, glyph, ct.CHARACTER_FMT, 8, head[7]))],
        container_variants(b))


def strings_variants(b):
    strings = ct.unpack_strings(b)
    last, second = len(strings) - 1, _get(b, 1, 1, ct.STRING_FMT, 0)
    return _edits(b, [
        ("string_gap", "range", lambda m: _set(m, 1, 1, ct.STRING_FMT, 0, second + 1)),
        ("units_short", "range", lambda m: _set(m, 1, last, ct.STRING_FMT, 1, len(strings[last]) - 1)),
        ("string_too_long", "count", lambda m: _set(m, 1, 0, ct.STRING_FMT, 1, 65537))],
        container_variants(b))


VARIANTS = {"texture": texture_variants, "animation": animation_variants, "sound": sound_variants,
            "model": model_variants, "lightmap": lightmap_variants, "collision": collision_variants,
            "model_animation": graph_variants, "font": font_variants, "strings": strings_variants}
# Whether a file has the structure its kind's variants edit.
READY = {"model": lambda b: _model_part(b) is not None,
         "lightmap": lambda b: _materials(b)[0][2][0] > 0 and _materials(b)[2] is not None,
         "collision": lambda b: all(_first_bsp(b)[0][k] for k in (0, 1, 5, 6, 7)) and bool(_first_bsp(b)[1]),
         "model_animation": lambda b: _compressed(b)[1] is not None,
         "font": lambda b: _glyph(b)[1] is not None,
         "strings": lambda b: len(ct.unpack_strings(b)) >= 2 and len(ct.unpack_strings(b)[-1]) >= 1}


def variant_base(kind, outputs, gen_dir):
    """The smallest published file of a decoded kind with the structure its variants edit."""
    group = sorted((e for e in outputs if e["kind"] == DECODED_KINDS[kind] and e["bytes"] <= MAX_FILE_BYTES),
                   key=lambda e: (e["bytes"], e["path"]))
    for e in group:
        blob = (gen_dir / e["path"]).read_bytes()
        if READY[kind](blob):
            return blob
    return None


def wrong_format_controls(valid):
    """(name, bytes, loader kind, expected error): a file given to another format's loader."""
    out = []
    if "model" in valid:
        out.append(("model_as_lightmap", valid["model"], "lightmap", "magic"))
    if "collision" in valid:
        relabelled = bytearray(valid["collision"])
        relabelled[0:4] = b"HWL1"
        out.append(("collision_relabelled_lightmap", bytes(relabelled), "lightmap", "section"))
    if "font" in valid:
        out.append(("font_as_strings", valid["font"], "strings", "magic"))
    return out


def build(root, out, prefix="sd:/" + SD_DIR + "/"):
    root, out = Path(root).resolve(), external(out)
    if out.exists():
        raise CaseError("output directory must be new")
    generation, manifest_sha = (root / "CURRENT").read_text(encoding="ascii").split()
    gen_dir = root / "generations" / generation
    raw = (gen_dir / "manifest.json").read_bytes()
    if sha(raw) != manifest_sha:
        raise CaseError("manifest hash differs from CURRENT")
    manifest = json.loads(raw)
    files_dir = out / "sd" / SD_DIR / "c"
    files_dir.mkdir(parents=True)
    rows, private, public = [], [], []
    counters = {kind: 0 for kind in KINDS}
    first_valid, smallest_valid = {}, {}
    sample = [(kind, entry, None) for kind, entry in select(manifest["outputs"], gen_dir)]
    sample += select_decoded(manifest["outputs"])
    for kind, entry, roles in sample:
        blob = (gen_dir / entry["path"]).read_bytes()
        if sha(blob) != entry["sha256"] or len(blob) != entry["bytes"]:
            raise CaseError("published file differs from its manifest entry")
        case_id = "%s%02d" % (PREFIX[kind], counters[kind])
        counters[kind] += 1
        decoded = DIGESTS[kind](blob)
        if kind == "animation" and decoded != entry["source"]["stream_sha256"]:
            raise CaseError("rebuilt animation stream differs from the manifest source hash")
        identity = source_identity(kind, blob, entry["source"])
        if identity is False:
            raise CaseError("decoded %s differs from the manifest source hash" % kind)
        name = "%s.%s" % (case_id, EXTENSIONS[kind])
        (files_dir / name).write_bytes(blob)
        rows.append((case_id, kind, "ok", name, len(blob), entry["sha256"], decoded))
        detail = {k: v for k, v in entry["source"].items() if k not in ("map", "tag", "bitmap", "index",
                                                                         "source_sha256", "stream_sha256")}
        if kind in DECODED_KINDS:
            detail.update(cd.DIGESTS[kind](blob)[1], roles=roles)
            if identity is not None:
                detail["source_identity"] = identity
            smallest_valid.setdefault(kind, blob)
        elif kind == "texture":
            detail.update(gx=entry["gx"]["format"], images=entry["gx"]["images"],
                          gx_loadable=entry["gx"]["loadable_dimensions"])
            header, _ = gt.unpack_hwt(blob)
            detail["texels"] = texels(header)
            if header["type"] == "2d" and header["levels"] > 1 and "texture" not in first_valid \
                    and entry["gx"]["loadable_dimensions"]:
                first_valid["texture"] = blob
        elif kind not in first_valid and (kind != "animation" or entry["source"]["version"] == 4):
            first_valid[kind] = blob
        public.append(dict(detail, id=case_id, kind=kind, bytes=len(blob), expect="ok"))
        private.append({"id": case_id, "path": entry["path"], "map": entry["source"]["map"],
                        "sha256": entry["sha256"], "decoded_sha256": decoded})
    sampled = {kind for kind, _, _ in sample}
    for kind in DECODED_KINDS:
        if kind in sampled:
            first_valid[kind] = variant_base(kind, manifest["outputs"], gen_dir)
    for kind in KINDS:
        if kind in DECODED_KINDS and kind not in sampled:
            continue   # the generation holds no outputs of this kind
        if first_valid.get(kind) is None:
            raise CaseError("no valid %s to derive malformed variants from" % kind)
        for index, (variant, data, error) in enumerate(VARIANTS[kind](first_valid[kind])):
            case_id = "m%s%02d" % (PREFIX[kind], index)
            name = "%s.%s" % (case_id, EXTENSIONS[kind])
            (files_dir / name).write_bytes(data)
            rows.append((case_id, kind, error, name, len(data), sha(data), "-"))
            public.append({"id": case_id, "kind": kind, "variant": variant, "bytes": len(data), "expect": error})
    for index, (variant, data, kind, error) in enumerate(wrong_format_controls(smallest_valid)):
        case_id = "w%02d" % index
        name = "%s.%s" % (case_id, EXTENSIONS[kind])
        (files_dir / name).write_bytes(data)
        rows.append((case_id, kind, error, name, len(data), sha(data), "-"))
        public.append({"id": case_id, "kind": kind, "variant": variant, "bytes": len(data), "expect": error,
                       "control": "wrong_format"})
    sd_lines, host_lines, stage = [], [], []
    for case_id, kind, expect, name, size, file_sha, decoded in rows:
        sd_lines.append("case %s %s %s %sc/%s %d %s %s" % (case_id, kind, expect, prefix, name, size, file_sha,
                                                             decoded))
        host_lines.append("case %s %s %s %s %d %s %s" % (case_id, kind, expect, (files_dir / name).as_posix(),
                                                           size, file_sha, decoded))
        stage.append("%s=%sc/%s" % ((files_dir / name).as_posix(), prefix, name))
    header = "# HWI-008C/E content loader cases: id kind expect path bytes file_sha256 decoded_sha256\n"
    (out / "sd" / SD_DIR / "cases.txt").write_text(header + "\n".join(sd_lines) + "\n", encoding="ascii",
                                                   newline="\n")
    stage.append("%s=%scases.txt" % ((out / "sd" / SD_DIR / "cases.txt").as_posix(), prefix))
    (out / "cases-host.txt").write_text(header + "\n".join(host_lines) + "\n", encoding="ascii", newline="\n")
    (out / "stage.txt").write_text("\n".join(stage) + "\n", encoding="ascii", newline="\n")
    summary = {"generation": generation, "manifest_sha256": manifest_sha, "cases": len(rows),
               "valid": sum(1 for r in rows if r[2] == "ok"), "malformed": sum(1 for r in rows if r[2] != "ok"),
               "bytes": sum(r[4] for r in rows)}
    (out / "cases-private.json").write_text(json.dumps(dict(summary, samples=private), indent=1, sort_keys=True)
                                            + "\n", encoding="utf-8")
    (out / "cases-public.json").write_text(json.dumps(dict(summary, cases=public), indent=1, sort_keys=True) + "\n",
                                           encoding="utf-8")
    return summary


def sweep(root, out, chunk=CHUNK_CASES):
    """Host case lists for every output of the decoded kinds, read in place
    from the generation (no copies): <out>/sweep-NNN.txt for host_check with
    the generation directory as its prefix. Returns the summary."""
    root, out = Path(root).resolve(), external(out)
    if out.exists():
        raise CaseError("output directory must be new")
    generation, manifest_sha = (root / "CURRENT").read_text(encoding="ascii").split()
    gen_dir = root / "generations" / generation
    raw = (gen_dir / "manifest.json").read_bytes()
    if sha(raw) != manifest_sha:
        raise CaseError("manifest hash differs from CURRENT")
    kinds = {manifest_kind: kind for kind, manifest_kind in DECODED_KINDS.items()}
    lines, counts, identities = [], {}, 0
    for entry in json.loads(raw)["outputs"]:
        kind = kinds.get(entry["kind"])
        if kind is None:
            continue
        if entry["bytes"] > MAX_FILE_BYTES:
            raise CaseError("an output is above the loader's file bound")
        path = gen_dir / entry["path"]
        blob = path.read_bytes()
        if sha(blob) != entry["sha256"]:
            raise CaseError("published file differs from its manifest entry")
        identity = source_identity(kind, blob, entry["source"])
        if identity is False:
            raise CaseError("decoded %s differs from the manifest source hash" % kind)
        identities += identity is True
        lines.append("case x%05d %s ok %s %d %s %s" % (len(lines), kind, path.as_posix(), len(blob), entry["sha256"],
                                                       DIGESTS[kind](blob)))
        counts[kind] = counts.get(kind, 0) + 1
    out.mkdir(parents=True)
    for index in range(0, len(lines), chunk):
        (out / ("sweep-%03d.txt" % (index // chunk))).write_text("\n".join(lines[index:index + chunk]) + "\n",
                                                                 encoding="ascii", newline="\n")
    return {"generation": generation, "files": len(lines), "by_kind": counts, "source_identity_matches": identities,
            "prefix": gen_dir.as_posix() + "/"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", required=True, type=Path, help="content_convert.py output root (private)")
    parser.add_argument("--out", required=True, type=Path, help="new private output directory")
    parser.add_argument("--sweep", action="store_true",
                        help="instead: host case lists of every model, lightmap, collision, model animation, font "
                             "and text output, read in place")
    args = parser.parse_args(argv)
    if args.sweep:
        try:
            summary = sweep(args.root, args.out)
        except (CaseError, br.RecordError, OSError, ValueError) as error:
            print("content loader sweep failed: %s" % error, file=sys.stderr)
            return 1
        print(json.dumps(summary, sort_keys=True))
        return 0
    try:
        summary = build(args.root, args.out)
    except (CaseError, gt.TextureError, ra.AnimationError, mf.MediaError, OSError, ValueError) as error:
        print("content loader cases failed: %s" % error, file=sys.stderr)
        return 1
    print("wrote %d cases (%d valid, %d malformed, %d bytes)" % (summary["cases"], summary["valid"],
                                                                 summary["malformed"], summary["bytes"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
