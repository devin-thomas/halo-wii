"""Model animation frame data (HWI-008C).

The engine reads an animation's frame info, default data and frame data
(source/models/model_animations.c) as native shorts and floats straight out
of tag data blobs that no tag field describes, so the Xbox little-endian
bytes need an explicit conversion before a big-endian reader can use them.

HMA1 model animations (one per animation graph tag), a sectioned big-endian
container (be_records.py). Sections:
  1 ANIMATIONS  80 bytes: animation index u16, type s16, frame count s16,
                frame size s16, frame info type s16, node count s16, flags
                u16, pad u16 = 0, node list checksum u32, translation,
                rotation and scale node flags (2 x u32 each), compressed
                data offset s32, then (byte offset, bytes) u32 pairs into
                FRAME_INFO, DEFAULTS, FRAMES and COMPRESSED
  2 FRAME_INFO  4-byte f32 values: per frame dx, dy (type 1); dx, dy, dyaw
                (type 2); dx, dy, dz, dyaw (type 3)
  3 DEFAULTS    bytes: per node in order, a rotation (4 x s16 quaternion)
                when the node has no rotation flag, a translation (3 x f32)
                when it has no translation flag, a scale (f32) when it has
                no scale flag; all big-endian
  4 FRAMES      bytes: per frame, per node in order, a rotation (4 x s16),
                translation (3 x f32) and scale (f32) for each flag the node
                has; all big-endian
  5 COMPRESSED  bytes: each compressed animation's block with every array at
                its original offset: the 11-word s32 header, rotation,
                translation and scale node headers (u32: first keyframe << 12
                | keyframe count), u16 keyframe frame indices, 6-byte
                rotations as 3 x u16, translations 3 x f32 and scales f32;
                all big-endian. The arrays must cover the block exactly.

Uncompressed data must be exactly frame count x frame size, and the
defaults exactly the size the node flags give; a compressed animation has
no separate defaults. Anything else rejects.
"""
import hashlib
import struct

import be_records as br
import halo_cache as hc

ANIMATION_FMT = "HhhhhhHHI" + "IIIIII" + "i" + "IIIIIIII"
HMA_SPEC = {1: br.record_size(ANIMATION_FMT), 2: 4, 3: 1, 4: 1, 5: 1}
GRAPH_ROOT_BYTES, GRAPH_ANIMATIONS, ANIMATION_BYTES, MAX_ANIMATIONS = 128, 116, 180, 256
MAX_NODES, FRAME_INFO_FLOATS = 64, {0: 0, 1: 2, 2: 3, 3: 4}
MAX_FRAME_INFO, MAX_DEFAULTS, MAX_DATA = 32768, 1536, 3145728
COMPRESSED_BIT, KEYFRAME_COUNT_BITS, COMPRESSED_HEADER_BYTES = 1, 12, 0x2C
ROTATION, TRANSLATION, SCALE = "hhhh", "III", "I"


class AnimationDataError(ValueError):
    """A bounded validation failure; the message names no tag or input bytes."""


def _flag_count(words, nodes):
    return sum((words[i // 32] >> (i % 32)) & 1 for i in range(nodes))


def _node_layout(words_t, words_r, words_s, nodes, flagged):
    """Record format for one frame (flagged) or the defaults (not flagged)."""
    fmt = ""
    for i in range(nodes):
        bit = lambda words: bool((words[i // 32] >> (i % 32)) & 1)  # noqa: E731
        if bit(words_r) == flagged:
            fmt += ROTATION
        if bit(words_t) == flagged:
            fmt += TRANSLATION
        if bit(words_s) == flagged:
            fmt += SCALE
    return fmt


def _compressed_spans(blob, nodes, counts):
    """(offset, count, record format) of every array of a compressed block."""
    if len(blob) < COMPRESSED_HEADER_BYTES:
        raise AnimationDataError("compressed block smaller than its header")
    header = struct.unpack_from("<11i", blob, 0)
    rotation_count, translation_count, scale_count = counts
    spans = [(0, 11, "i")]

    def node_headers(offset, count):
        if count and (offset < 0 or offset + 4 * count > len(blob)):
            raise AnimationDataError("compressed node headers outside the block")
        words = struct.unpack_from("<%dI" % count, blob, offset) if count else ()
        spans.append((offset, count, "I"))
        extent = 0
        for word in words:
            first, keyframes = word >> KEYFRAME_COUNT_BITS, word & ((1 << KEYFRAME_COUNT_BITS) - 1)
            if keyframes:
                extent = max(extent, first + keyframes)
        return extent

    rotation_keys = node_headers(COMPRESSED_HEADER_BYTES, rotation_count)
    translation_keys = node_headers(header[3], translation_count)
    scale_keys = node_headers(header[7], scale_count)
    spans += [(header[0], rotation_keys, "H"), (header[1], nodes, "HHH"), (header[2], rotation_keys, "HHH"),
              (header[4], translation_keys, "H"), (header[5], nodes, "III"), (header[6], translation_keys, "III"),
              (header[8], scale_keys, "H"), (header[9], scale_count, "I"), (header[10], scale_keys, "I")]
    spans = [s for s in spans if s[1]]
    cover = 0
    end = 0
    for offset, count, fmt in sorted(spans):
        size = count * br.record_size(fmt)
        if offset < end or offset + size > len(blob):
            raise AnimationDataError("compressed arrays overlap or leave the block")
        if offset != end:
            raise AnimationDataError("compressed block has bytes no array describes")
        end = offset + size
        cover += size
    if cover != len(blob):
        raise AnimationDataError("compressed block has bytes no array describes")
    return spans


def convert_compressed(blob, nodes, counts):
    out = bytearray(len(blob))
    for offset, count, fmt in _compressed_spans(blob, nodes, counts):
        size = count * br.record_size(fmt)
        out[offset:offset + size] = br.to_big_endian(blob[offset:offset + size], fmt, count)
    return bytes(out)


def convert_graph(cache, instance):
    """HMA1 bytes and facts for one animation graph tag."""
    root = cache.root_offset(instance, GRAPH_ROOT_BYTES)
    if root is None:
        raise AnimationDataError("animation graph root outside tag data")
    try:
        elements = cache.block(root + GRAPH_ANIMATIONS, ANIMATION_BYTES, MAX_ANIMATIONS)
    except hc.CacheError:
        raise AnimationDataError("animation block outside tag data") from None
    records, sections = [], {2: [], 3: [], 4: [], 5: []}
    cursors = {2: 0, 3: 0, 4: 0, 5: 0}
    source = hashlib.sha256()
    facts = {"animations": len(elements), "compressed": 0, "frames": 0}
    for index, a in enumerate(elements):
        kind, frames, frame_size, info_type = struct.unpack_from("<hhhh", cache.tags, a + 32)
        checksum, nodes = cache.u32(a + 40), cache.s16(a + 44)
        flags = cache.u16(a + 58)
        translation = struct.unpack_from("<II", cache.tags, a + 92)
        rotation = struct.unpack_from("<II", cache.tags, a + 108)
        scale = struct.unpack_from("<II", cache.tags, a + 124)
        compressed_offset = cache.s32(a + 136)
        try:
            frame_info = cache.tag_data_bytes(a + 72, MAX_FRAME_INFO)
            defaults = cache.tag_data_bytes(a + 140, MAX_DEFAULTS)
            data = cache.tag_data_bytes(a + 160, MAX_DATA)
        except hc.CacheError:
            raise AnimationDataError("animation data outside tag data or above bound") from None
        if not 0 <= nodes <= MAX_NODES or info_type not in FRAME_INFO_FLOATS or frames < 0:
            raise AnimationDataError("animation node count, frame count or frame info type outside bounds")
        counts = (_flag_count(rotation, nodes), _flag_count(translation, nodes), _flag_count(scale, nodes))
        expected_frame = counts[0] * 8 + counts[1] * 12 + counts[2] * 4
        if frame_size != expected_frame:
            raise AnimationDataError("animation frame size differs from its node flags")
        floats = frames * FRAME_INFO_FLOATS[info_type]
        if len(frame_info) != 4 * floats:
            raise AnimationDataError("animation frame info size differs from its frames")
        compressed = bool(flags & COMPRESSED_BIT)
        frame_fmt = _node_layout(translation, rotation, scale, nodes, True)
        default_fmt = _node_layout(translation, rotation, scale, nodes, False)
        if compressed:
            facts["compressed"] += 1
            uncompressed_bytes = compressed_offset
            if compressed_offset not in (0, frames * frame_size) or defaults:
                raise AnimationDataError("compressed animation offset or defaults inconsistent")
            frames_part = data[:uncompressed_bytes]
            block = convert_compressed(data[compressed_offset:], nodes, counts)
            default_out = b""
        else:
            if len(data) != frames * frame_size or len(defaults) != br.record_size(default_fmt or "x") * \
                    (1 if default_fmt else 0):
                raise AnimationDataError("animation frame or default data size differs from its flags")
            frames_part, block = data, b""
            default_out = br.to_big_endian(defaults, default_fmt, 1) if default_fmt else b""
        frame_count = len(frames_part) // frame_size if frame_size else 0
        frames_out = br.to_big_endian(frames_part, frame_fmt, frame_count) if frame_fmt else b""
        if frame_fmt and len(frames_out) != len(frames_part):
            raise AnimationDataError("animation frame data not a whole number of frames")
        info_out = br.to_big_endian(frame_info, "I", floats)
        placed = []
        for section, payload in ((2, info_out), (3, default_out), (4, frames_out), (5, block)):
            unit = HMA_SPEC[section]
            placed += [cursors[section] * unit, len(payload)]
            sections[section].append(payload)
            cursors[section] += len(payload) // unit
        records.append((index, kind, frames, frame_size, info_type, nodes, flags, 0, checksum) + translation +
                       rotation + scale + (compressed_offset,) + tuple(placed))
        for part in (frame_info, defaults, data):
            source.update(bytes(part))
        facts["frames"] += max(frames, 0)
    container = [(1, HMA_SPEC[1], len(records), br.encode_records([v for r in records for v in r], ANIMATION_FMT,
                                                                   len(records)))]
    for section in (2, 3, 4, 5):
        container.append((section, HMA_SPEC[section], cursors[section], b"".join(sections[section])))
    facts.update(source_sha256=source.hexdigest(), frame_bytes=cursors[4], compressed_bytes=cursors[5])
    return br.pack_container(b"HMA1", container), facts


def unpack_graph(blob):
    """Explicit decoder: every animation's frames, defaults and compressed arrays
    re-read big-endian; returns the decoded animation records."""
    sections = br.unpack_container(blob, b"HMA1", HMA_SPEC)
    width = len(struct.unpack("<" + ANIMATION_FMT, bytes(HMA_SPEC[1])))
    values = br.decode_records(sections[1][1], ANIMATION_FMT, sections[1][0], ">")
    rows = [values[i:i + width] for i in range(0, len(values), width)]
    cursors = {2: 0, 3: 0, 4: 0, 5: 0}
    for row in rows:
        frames, frame_size, nodes, flags = row[2], row[3], row[5], row[6]
        translation, rotation, scale = row[9:11], row[11:13], row[13:15]
        spans = row[16:24]
        for k, section in enumerate((2, 3, 4, 5)):
            offset, size = spans[2 * k], spans[2 * k + 1]
            if offset != cursors[section] * HMA_SPEC[section] or offset + size > len(sections[section][1]):
                raise AnimationDataError("animation data ranges out of order")
            cursors[section] += size // HMA_SPEC[section]
        frame_fmt = _node_layout(translation, rotation, scale, nodes, True)
        default_fmt = _node_layout(translation, rotation, scale, nodes, False)
        frames_view = sections[4][1][spans[4]:spans[4] + spans[5]]
        if frame_fmt:
            br.decode_records(frames_view, frame_fmt, len(frames_view) // br.record_size(frame_fmt), ">")
        elif len(frames_view):
            raise AnimationDataError("frame data without flagged nodes")
        defaults_view = sections[3][1][spans[2]:spans[2] + spans[3]]
        if len(defaults_view) and len(defaults_view) != br.record_size(default_fmt or "x"):
            raise AnimationDataError("default data size differs from its flags")
        if flags & COMPRESSED_BIT:
            block = bytes(sections[5][1][spans[6]:spans[6] + spans[7]])
            counts = (_flag_count(rotation, nodes), _flag_count(translation, nodes), _flag_count(scale, nodes))
            for offset, count, fmt in _compressed_spans_be(block, nodes, counts):
                br.decode_records(block[offset:offset + count * br.record_size(fmt)], fmt, count, ">")
        elif spans[7]:
            raise AnimationDataError("compressed data on an uncompressed animation")
        if frames * frame_size < len(frames_view):
            raise AnimationDataError("more frame data than frames")
    if [cursors[s] for s in (2, 3, 4, 5)] != [sections[s][0] for s in (2, 3, 4, 5)]:
        raise AnimationDataError("animation data ranges do not cover the sections")
    return rows


def _compressed_spans_be(block, nodes, counts):
    """The array spans of a big-endian compressed block (its header and node
    headers read big-endian), validated like the source block."""
    if len(block) < COMPRESSED_HEADER_BYTES:
        raise AnimationDataError("compressed block smaller than its header")
    header = struct.unpack_from(">11i", block, 0)
    little = bytearray(block)
    little[0:44] = struct.pack("<11i", *header)
    for offset, count in ((COMPRESSED_HEADER_BYTES, counts[0]), (header[3], counts[1]), (header[7], counts[2])):
        if count:
            if offset < 0 or offset + 4 * count > len(block):
                raise AnimationDataError("compressed node headers outside the block")
            words = struct.unpack_from(">%dI" % count, block, offset)
            struct.pack_into("<%dI" % count, little, offset, *words)
    return _compressed_spans(bytes(little), nodes, counts)
