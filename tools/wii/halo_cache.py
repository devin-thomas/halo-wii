"""Bounded, read-only access to an Xbox version-5 Halo cache (map) file.

A stored Xbox map is a 2,048-byte header followed by one zlib stream that
inflates to the rest of the declared file. Tag data, bitmap pixels and sound
samples are addressed by offsets into that inflated file; tag-internal
pointers are Xbox virtual addresses based at TAG_BASE. Every count, address
and span read here is checked against the bytes actually present before it is
used, and nothing is ever written to the source.

Errors raise CacheError, whose text never contains input bytes, tag names or
file paths.
"""
import hashlib
import os
from pathlib import Path
import struct
import zlib

TAG_BASE = 0x803A6000
HEADER_BYTES = 0x800
MAP_LIMIT = 0x11600000          # largest declared (inflated) map accepted
TAG_LIMIT = 0x01600000          # Xbox tag reservation (22 MiB)
INDEX_BYTES = 36
INSTANCE_BYTES = 32
MAX_INSTANCES = 65535
CHUNK = 1 << 16

HEAD, FOOT, VERSION = 0x68656164, 0x666F6F74, 5
TAGS_SIGNATURE = 0x74616773
SCENARIO_TYPES = {0: "campaign", 1: "multiplayer", 2: "ui"}


class CacheError(ValueError):
    """A bounded validation failure; the message holds no input text or path."""


def group_name(word):
    """The four-character group code of a tag group word (big-endian text)."""
    return struct.pack(">I", word).decode("latin-1")


def _ascii_field(data, offset, size):
    raw = data[offset:offset + size]
    end = raw.find(0)
    if end < 0:
        raise CacheError("header string lacks terminator")
    text = raw[:end]
    if any(c < 0x20 or c > 0x7E for c in text):
        raise CacheError("header string not printable ASCII")
    return text.decode("ascii")


def parse_header(data):
    """Validate and decode the 2,048-byte Xbox v5 cache header."""
    if len(data) < HEADER_BYTES:
        raise CacheError("truncated cache header")
    head, version, declared, _, tag_offset, tag_size = struct.unpack_from("<6i", data, 0)
    if (head & 0xFFFFFFFF) != HEAD or struct.unpack_from("<I", data, 0x7FC)[0] != FOOT:
        raise CacheError("cache head/foot signature")
    if version != VERSION:
        raise CacheError("only Xbox cache version 5 is supported")
    if not HEADER_BYTES <= declared <= MAP_LIMIT:
        raise CacheError("declared map length outside bounded profile")
    if not INDEX_BYTES <= tag_size <= TAG_LIMIT:
        raise CacheError("tag data size outside Xbox reservation")
    if tag_offset < HEADER_BYTES or tag_offset > declared - tag_size:
        raise CacheError("tag data extent outside declared map")
    scenario_type = struct.unpack_from("<h", data, 0x60)[0]
    return {
        "version": version, "declared_bytes": declared, "tag_offset": tag_offset, "tag_bytes": tag_size,
        "name": _ascii_field(data, 0x20, 32), "build": _ascii_field(data, 0x40, 32),
        "scenario_type": scenario_type, "category": SCENARIO_TYPES.get(scenario_type, "unknown"),
        "checksum_observed": struct.unpack_from("<I", data, 0x64)[0],
    }


def _identity(st):
    return (st.st_size, st.st_mtime_ns)


def inflate_stream(stream, max_declared=MAP_LIMIT):
    """Inflate an open binary stream; returns (header dict, inflated bytes, facts).

    The inflated bytes include the stored header at offset 0, so every file
    offset in the map indexes them directly.
    """
    header_bytes = stream.read(HEADER_BYTES)
    header = parse_header(header_bytes)
    declared = header["declared_bytes"]
    if declared > max_declared:
        raise CacheError("declared map length above caller bound")
    body = bytearray(declared)
    body[:HEADER_BYTES] = header_bytes
    position = HEADER_BYTES
    decoder = zlib.decompressobj()
    compressed = 0
    trailing = 0
    try:
        while True:
            chunk = stream.read(CHUNK)
            if not chunk:
                break
            if decoder.eof:
                trailing += len(chunk)
                continue
            pending = chunk
            while pending:
                output = decoder.decompress(pending, CHUNK)
                if position + len(output) > declared:
                    raise CacheError("inflated output exceeds declared map length")
                body[position:position + len(output)] = output
                position += len(output)
                if decoder.eof:
                    compressed += len(pending) - len(decoder.unused_data)
                    trailing += len(decoder.unused_data)
                    break
                compressed += len(pending) - len(decoder.unconsumed_tail)
                pending = decoder.unconsumed_tail
    except zlib.error:
        raise CacheError("compressed stream is corrupt") from None
    if not decoder.eof:
        raise CacheError("compressed stream truncated before its end")
    if position != declared:
        raise CacheError("inflated length differs from declared length")
    return header, bytes(body), {"compressed_stream_bytes": compressed, "trailing_stored_bytes": trailing}


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def open_map(path, expected_sha256=None, max_declared=MAP_LIMIT):
    """Read and inflate one stored map file without modifying it.

    Returns a CacheMap. The source is hashed while it is read; a changed size
    or modification time, or a hash that differs from expected_sha256, rejects.
    """
    path = Path(path)
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        digest = hashlib.sha256()

        class _Hashing:
            def read(self, size):
                data = stream.read(size)
                digest.update(data)
                return data

        header, body, facts = inflate_stream(_Hashing(), max_declared)
        rest = stream.read()
        digest.update(rest)
        facts["trailing_stored_bytes"] += len(rest)
        after = os.fstat(stream.fileno())
    if _identity(before) != _identity(after):
        raise CacheError("source changed while it was read")
    source_sha = digest.hexdigest()
    if expected_sha256 is not None and source_sha != expected_sha256:
        raise CacheError("source hash differs from its recorded identity")
    facts.update(source_sha256=source_sha, stored_bytes=before.st_size)
    return CacheMap(header, body, facts)


class CacheMap:
    """An inflated map plus a checked view of its tag index."""

    def __init__(self, header, body, facts=None, tag_base=TAG_BASE):
        self.header = header
        self.body = body
        self.facts = facts or {}
        self.tag_base = tag_base
        start = header["tag_offset"]
        self.tags = memoryview(body)[start:start + header["tag_bytes"]]
        words = struct.unpack_from("<9I", self.tags, 0)
        (instances_address, scenario_datum, checksum, count, _vc, _va, _ic, _ia, signature) = words
        if signature != TAGS_SIGNATURE:
            raise CacheError("tag index signature")
        if not 1 <= count <= MAX_INSTANCES:
            raise CacheError("tag instance count outside bounds")
        self.scenario_datum = scenario_datum
        self.index_checksum = checksum
        table = self.offset(instances_address, count * INSTANCE_BYTES)
        self.instances = []
        for ordinal in range(count):
            group, parent0, parent1, datum, name_address, root, _u0, _u1 = struct.unpack_from(
                "<8I", self.tags, table + ordinal * INSTANCE_BYTES)
            if (datum & 0xFFFF) != ordinal:
                raise CacheError("tag datum does not match its ordinal")
            self.instances.append({"ordinal": ordinal, "group": group, "parents": (parent0, parent1),
                                   "datum": datum, "name_address": name_address, "root": root})
        ordinal = scenario_datum & 0xFFFF
        if ordinal >= count or self.instances[ordinal]["datum"] != scenario_datum or \
                self.instances[ordinal]["group"] != 0x73636E72:
            raise CacheError("scenario datum does not name a scenario tag")
        self.scenario_ordinal = ordinal

    # -- tag-space addressing -------------------------------------------------
    def offset(self, address, length, alignment=1):
        """Offset within the tag bytes of [address, address+length), checked."""
        if length < 0 or address < self.tag_base or address % alignment:
            raise CacheError("tag address below base, misaligned or negative length")
        offset = address - self.tag_base
        if offset > len(self.tags) or length > len(self.tags) - offset:
            raise CacheError("tag span outside tag data")
        return offset

    def root_offset(self, instance, length):
        """Offset of a tag's root struct, or None when the root lies outside tag data."""
        root = instance["root"]
        if not self.tag_base <= root < self.tag_base + len(self.tags):
            return None
        return self.offset(root, length)

    def name(self, instance, limit=256):
        start = self.offset(instance["name_address"], 1)
        end = bytes(self.tags[start:start + limit]).find(0)
        if end < 0:
            raise CacheError("tag name lacks a bounded terminator")
        return bytes(self.tags[start:start + end]).decode("latin-1")

    def u16(self, offset):
        return struct.unpack_from("<H", self.tags, offset)[0]

    def s16(self, offset):
        return struct.unpack_from("<h", self.tags, offset)[0]

    def u32(self, offset):
        return struct.unpack_from("<I", self.tags, offset)[0]

    def s32(self, offset):
        return struct.unpack_from("<i", self.tags, offset)[0]

    def block(self, field_offset, stride, max_count):
        """Element offsets of a tag_block {count, address, definition} field."""
        count, address = struct.unpack_from("<iI", self.tags, field_offset)
        if count < 0 or count > max_count:
            raise CacheError("tag block count outside bounds")
        if count == 0:
            return []
        base = self.offset(address, count * stride)
        return [base + i * stride for i in range(count)]

    def tag_data(self, field_offset):
        """Decode a tag_data {size, flags, file_offset, address, definition} field."""
        size, flags, file_offset, address = struct.unpack_from("<iIiI", self.tags, field_offset)
        if size < 0:
            raise CacheError("tag data size negative")
        return {"size": size, "flags": flags, "file_offset": file_offset, "address": address}

    def tag_data_bytes(self, field_offset, max_size):
        """Bytes of a tag_data field resident in tag space (address-based)."""
        data = self.tag_data(field_offset)
        if data["size"] > max_size:
            raise CacheError("tag data size above bound")
        if data["size"] == 0:
            return b""
        start = self.offset(data["address"], data["size"])
        return bytes(self.tags[start:start + data["size"]])

    def file_span(self, file_offset, size):
        """Bytes [file_offset, file_offset+size) of the inflated map, checked."""
        if file_offset < 0 or size < 0 or file_offset > len(self.body) or size > len(self.body) - file_offset:
            raise CacheError("file span outside inflated map")
        return memoryview(self.body)[file_offset:file_offset + size]

    def by_group(self, code):
        word = struct.unpack(">I", code.encode("latin-1"))[0]
        return [i for i in self.instances if i["group"] == word]

    def group_counts(self):
        counts = {}
        for instance in self.instances:
            key = group_name(instance["group"])
            counts[key] = counts.get(key, 0) + 1
        return dict(sorted(counts.items()))
