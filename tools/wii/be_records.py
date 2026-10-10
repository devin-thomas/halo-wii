"""Explicit record conversion and the sectioned big-endian container (HWI-008C).

Xbox tag data is little-endian. Every converted record is decoded field by
field with an explicit little-endian struct format and re-encoded big-endian
with the same format; nothing relies on host byte order, struct layout or
compiler bit-fields (ADR-018). Floats are carried as their raw IEEE-754 bit
patterns (format code I), so no value is ever re-rounded.

Sectioned container (all fields big-endian):

  header, 32 bytes   magic (4 ASCII), version u16 = 1, header bytes u16 = 32,
                     section count u16, flags u16 = 0, total file bytes u32,
                     16 reserved bytes = 0
  section table      one 16-byte entry per section, in ascending id order:
                     id u16, record bytes u16, record count u32, offset u32,
                     bytes u32 (= count * record bytes)
  sections           each starts at the next 32-byte boundary after the
                     table or the previous section; padding bytes are zero and
                     the file ends at a 32-byte boundary

Each container type fixes its section ids, record sizes and record formats
(content_geometry.py, content_animation.py, content_text.py). A reader
rejects a missing, extra or reordered section, a size or count that does not
match, a misplaced offset, non-zero padding or reserved bytes, and a wrong
total length.
"""
import struct

HEADER = struct.Struct(">4sHHHHI16x")
SECTION = struct.Struct(">HHIII")
ALIGN = 32
CHUNK = 1024
_STRUCTS = {}


class RecordError(ValueError):
    """A bounded validation failure; the message holds no input bytes."""


def _struct(order, fmt, count):
    key = (order, fmt, count)
    cached = _STRUCTS.get(key)
    if cached is None:
        cached = _STRUCTS[key] = struct.Struct(order + fmt * count)
    return cached


def record_size(fmt):
    return struct.calcsize("<" + fmt)


def decode_records(data, fmt, count, order="<"):
    """Values of count fmt records (explicit byte order), as one flat tuple."""
    size = record_size(fmt)
    if len(data) != size * count:
        raise RecordError("record array length differs from its count")
    values = []
    view = memoryview(data)
    done = 0
    while done < count:
        n = min(CHUNK, count - done)
        values.extend(_struct(order, fmt, n).unpack_from(view, done * size))
        done += n
    return tuple(values)


def encode_records(values, fmt, count, order=">"):
    per = len(struct.unpack("<" + fmt, bytes(record_size(fmt))))
    if len(values) != per * count:
        raise RecordError("value count differs from the record count")
    out = bytearray()
    done = 0
    while done < count:
        n = min(CHUNK, count - done)
        out += _struct(order, fmt, n).pack(*values[done * per:(done + n) * per])
        done += n
    return bytes(out)


def to_big_endian(data, fmt, count):
    """Explicit little-endian decode, big-endian encode; the result is checked
    by rebuilding the exact source bytes from it."""
    out = encode_records(decode_records(data, fmt, count, "<"), fmt, count, ">")
    if to_little_endian(out, fmt, count) != bytes(data):
        raise RecordError("big-endian records do not rebuild their source")
    return out


def to_little_endian(data, fmt, count):
    return encode_records(decode_records(data, fmt, count, ">"), fmt, count, "<")


def _pad(n):
    return -n & (ALIGN - 1)


def pack_container(magic, sections):
    """sections: [(id, record_bytes, count, payload)] in ascending id order."""
    ids = [s[0] for s in sections]
    if ids != sorted(set(ids)):
        raise RecordError("section ids must ascend without repeats")
    cursor = HEADER.size + SECTION.size * len(sections)
    table, body = bytearray(), bytearray()
    cursor += _pad(cursor)
    for section_id, record_bytes, count, payload in sections:
        if len(payload) != record_bytes * count:
            raise RecordError("section payload length differs from its records")
        table += SECTION.pack(section_id, record_bytes, count, cursor, len(payload))
        body += payload + bytes(_pad(len(payload)))
        cursor += len(payload) + _pad(len(payload))
    head = HEADER.size + len(table)
    blob = bytearray(HEADER.pack(magic, 1, HEADER.size, len(sections), 0, cursor))
    blob += table + bytes(_pad(head)) + body
    if len(blob) != cursor:
        raise RecordError("container layout inconsistent")
    return bytes(blob)


def unpack_container(blob, magic, spec):
    """Validate a container; spec maps section id -> record bytes.

    Returns {id: (count, memoryview payload)}.
    """
    blob = memoryview(blob)
    if len(blob) < HEADER.size:
        raise RecordError("container header truncated")
    found, version, header_bytes, count, flags, total = HEADER.unpack_from(blob, 0)
    if found != magic:
        raise RecordError("container magic")
    if version != 1 or header_bytes != HEADER.size or flags != 0:
        raise RecordError("container version, header size or flags")
    if any(blob[16:32]):
        raise RecordError("container reserved bytes not zero")
    if total != len(blob):
        raise RecordError("container total length differs from the file")
    ids = sorted(spec)
    if count != len(ids):
        raise RecordError("container section count")
    head = HEADER.size + SECTION.size * count
    if head > len(blob):
        raise RecordError("section table truncated")
    cursor = head + _pad(head)
    if any(blob[head:cursor]):
        raise RecordError("container padding not zero")
    sections = {}
    for index, expected_id in enumerate(ids):
        section_id, record_bytes, records, offset, size = SECTION.unpack_from(blob, HEADER.size + SECTION.size * index)
        if section_id != expected_id or record_bytes != spec[expected_id]:
            raise RecordError("section id or record size")
        if size != records * record_bytes or offset != cursor or offset + size > len(blob):
            raise RecordError("section count, offset or length")
        end = offset + size
        padded = end + _pad(end)
        if padded > len(blob) or any(blob[end:padded]):
            raise RecordError("section padding")
        sections[section_id] = (records, blob[offset:end])
        cursor = padded
    if cursor != len(blob):
        raise RecordError("container has bytes after its last section")
    return sections
