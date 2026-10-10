"""Fonts, unicode text and the shader in-place census (HWI-008C).

HWF1 font (one per font tag), a sectioned big-endian container (be_records.py):
  1 FONT        24 bytes: flags u32, ascending height s16, descending height
                s16, leading height s16, leading width s16, character tables
                u32, characters u32, pixel bytes u32
  2 TABLES      8 bytes: first index u32, index count u32 (into INDICES)
  3 INDICES     2 bytes: character index s16 (-1: none)
  4 CHARACTERS  20 bytes: character u16, character width s16, bitmap width
                s16, bitmap height s16, bitmap origin x s16, origin y s16,
                hardware character index s16, pad u16, pixel offset s32 (a
                glyph covers max(width, 0) x max(height, 0) pixels; blank
                glyphs may carry a negative width with height 0)
  5 PIXELS      8-bit glyph coverage, verbatim (row-major per glyph)

The four style-font references stay tag fields read in place.

HUS1 unicode text (one per unicode string list tag, and one per HUD
message text tag whose text blob is its single entry):
  1 STRINGS     8 bytes: first code unit u32, code units u32
  2 UNITS       2 bytes: UTF-16 code unit u16, big-endian

Each string keeps its exact code units, terminating zero included. The
engine reads these as 16-bit units (ADR-018: 16-bit text on Wii), so the
little-endian Xbox units are decoded explicitly and re-encoded big-endian.

Shaders are not converted. Their parameters are typed tag fields (enums,
flags, colours, floats, tag references and blocks); no shader group has a
tag-data field or file-offset payload (the upstream validator schema), and
the Xbox pixel programs are in the executable, not the map. The engine
reads shader parameters in place once the tag cache is relocated (ADR-015)
and byte-ordered. shader_census() checks every shader tag's root lies in
tag data and records the counts.
"""
import hashlib
import struct

import be_records as br
import halo_cache as hc

FONT_FMT = "Ihhhh" + "III"
TABLE_FMT = "II"
CHARACTER_FMT = "Hhhhhhhhi"
HWF_SPEC = {1: br.record_size(FONT_FMT), 2: 8, 3: 2, 4: br.record_size(CHARACTER_FMT), 5: 1}
STRING_FMT = "II"
HUS_SPEC = {1: 8, 2: 2}

FONT_ROOT_BYTES, FONT_TABLES, FONT_CHARACTERS, FONT_PIXELS = 156, 48, 124, 136
MAX_TABLES, MAX_TABLE_ENTRIES, MAX_CHARACTERS, MAX_PIXELS = 256, 256, 32767, 8388608
STRING_LIST_ROOT_BYTES, MAX_STRINGS, MAX_STRING_BYTES = 12, 32767, 65534
HUD_TEXT_ROOT_BYTES, MAX_HUD_TEXT_BYTES = 128, 131072
# shader groups and their root sizes (upstream validator schema)
SHADER_ROOTS = {"senv": 836, "soso": 440, "schi": 108, "scex": 120, "sgla": 480, "smet": 260, "sotr": 108,
                "spla": 332, "swat": 320}


class TextError(ValueError):
    """A bounded validation failure; the message names no tag or input bytes."""


def _block(cache, field, stride, maximum):
    try:
        return cache.block(field, stride, maximum)
    except hc.CacheError:
        raise TextError("block count or span outside bounds") from None


def _data(cache, field, maximum):
    try:
        return cache.tag_data_bytes(field, maximum)
    except hc.CacheError:
        raise TextError("tag data outside tag data or above bound") from None


def convert_font(cache, instance):
    root = cache.root_offset(instance, FONT_ROOT_BYTES)
    if root is None:
        raise TextError("font root outside tag data")
    flags, ascending, descending, leading_height, leading_width = struct.unpack_from("<Ihhhh", cache.tags, root)
    characters = _block(cache, root + FONT_CHARACTERS, 20, MAX_CHARACTERS)
    pixels = _data(cache, root + FONT_PIXELS, MAX_PIXELS)
    raw_characters = bytes(cache.tags[characters[0]:characters[0] + 20 * len(characters)]) if characters else b""
    values = br.decode_records(raw_characters, CHARACTER_FMT, len(characters))
    for i in range(len(characters)):
        width, height, offset = values[9 * i + 2], values[9 * i + 3], values[9 * i + 8]
        if offset < 0 or offset + max(width, 0) * max(height, 0) > len(pixels):
            raise TextError("glyph pixels outside the font's pixel data")
    tables, indices, source = [], [], [raw_characters, pixels]
    for table in _block(cache, root + FONT_TABLES, 12, MAX_TABLES):
        entries = _block(cache, table, 2, MAX_TABLE_ENTRIES)
        raw = bytes(cache.tags[entries[0]:entries[0] + 2 * len(entries)]) if entries else b""
        found = br.decode_records(raw, "h", len(entries))
        if any(not -1 <= v < len(characters) for v in found):
            raise TextError("character table names a character the font lacks")
        tables.append(len(entries))
        indices.append(br.to_big_endian(raw, "h", len(entries)))
        source.append(raw)
    firsts, cursor = [], 0
    for count in tables:
        firsts += [cursor, count]
        cursor += count
    head = (flags, ascending, descending, leading_height, leading_width, len(tables), len(characters), len(pixels))
    blob = br.pack_container(b"HWF1", [
        (1, HWF_SPEC[1], 1, br.encode_records(head, FONT_FMT, 1)),
        (2, HWF_SPEC[2], len(tables), br.encode_records(firsts, TABLE_FMT, len(tables))),
        (3, HWF_SPEC[3], cursor, b"".join(indices)),
        (4, HWF_SPEC[4], len(characters), br.to_big_endian(raw_characters, CHARACTER_FMT, len(characters))),
        (5, HWF_SPEC[5], len(pixels), bytes(pixels))])
    digest = hashlib.sha256(struct.pack("<Ihhhh", *head[:5]))
    for part in source:
        digest.update(part)
    return blob, {"tables": len(tables), "characters": len(characters), "pixel_bytes": len(pixels),
                  "source_sha256": digest.hexdigest()}


def unpack_font(blob):
    sections = br.unpack_container(blob, b"HWF1", HWF_SPEC)
    if sections[1][0] != 1:
        raise TextError("font record count")
    head = br.decode_records(sections[1][1], FONT_FMT, 1, ">")
    if head[5:] != (sections[2][0], sections[4][0], sections[5][0]):
        raise TextError("font counts differ from the sections")
    tables = br.decode_records(sections[2][1], TABLE_FMT, sections[2][0], ">")
    cursor = 0
    for i in range(sections[2][0]):
        if tables[2 * i] != cursor:
            raise TextError("character tables out of order")
        cursor += tables[2 * i + 1]
    if cursor != sections[3][0]:
        raise TextError("character tables do not cover the indices")
    characters = br.decode_records(sections[4][1], CHARACTER_FMT, sections[4][0], ">")
    for i in range(sections[4][0]):
        width, height, offset = characters[9 * i + 2], characters[9 * i + 3], characters[9 * i + 8]
        if offset < 0 or offset + max(width, 0) * max(height, 0) > sections[5][0]:
            raise TextError("glyph pixels outside the font's pixel data")
    return head, sections


def _pack_strings(strings):
    rows, units, cursor = [], [], 0
    for raw in strings:
        if len(raw) % 2:
            raise TextError("UTF-16 string with an odd byte count")
        count = len(raw) // 2
        rows += [cursor, count]
        units.append(br.to_big_endian(raw, "H", count))
        cursor += count
    return br.pack_container(b"HUS1", [(1, HUS_SPEC[1], len(strings), br.encode_records(rows, STRING_FMT,
                                                                                        len(strings))),
                                       (2, HUS_SPEC[2], cursor, b"".join(units))])


def convert_string_list(cache, instance):
    root = cache.root_offset(instance, STRING_LIST_ROOT_BYTES)
    if root is None:
        raise TextError("string list root outside tag data")
    strings = [_data(cache, entry, MAX_STRING_BYTES) for entry in _block(cache, root, 20, MAX_STRINGS)]
    blob = _pack_strings(strings)
    digest = hashlib.sha256()
    for raw in strings:
        digest.update(struct.pack("<I", len(raw)) + raw)
    return blob, {"strings": len(strings), "code_units": sum(len(s) // 2 for s in strings),
                  "terminated": sum(1 for s in strings if s[-2:] == b"\0\0"), "source_sha256": digest.hexdigest()}


def convert_hud_text(cache, instance):
    root = cache.root_offset(instance, HUD_TEXT_ROOT_BYTES)
    if root is None:
        raise TextError("HUD message text root outside tag data")
    text = _data(cache, root, MAX_HUD_TEXT_BYTES)
    return _pack_strings([text]), {"strings": 1, "code_units": len(text) // 2,
                                   "source_sha256": hashlib.sha256(text).hexdigest()}


def unpack_strings(blob):
    sections = br.unpack_container(blob, b"HUS1", HUS_SPEC)
    rows = br.decode_records(sections[1][1], STRING_FMT, sections[1][0], ">")
    cursor = 0
    for i in range(sections[1][0]):
        if rows[2 * i] != cursor:
            raise TextError("strings out of order")
        cursor += rows[2 * i + 1]
    if cursor != sections[2][0]:
        raise TextError("strings do not cover the code units")
    units = br.decode_records(sections[2][1], "H", cursor, ">")
    return [units[rows[2 * i]:rows[2 * i] + rows[2 * i + 1]] for i in range(sections[1][0])]


def shader_census(cache):
    """Per shader group: tags and root bytes, every root checked inside tag data."""
    census = {}
    for group, size in sorted(SHADER_ROOTS.items()):
        tags = cache.by_group(group)
        for instance in tags:
            if cache.root_offset(instance, size) is None:
                raise TextError("shader root outside tag data")
        if tags:
            census[group] = {"tags": len(tags), "root_bytes": size * len(tags)}
    return census
