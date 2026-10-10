"""Explicit decoding of Halo recorded-animation event streams (ADR-018).

A scenario's recorded animation keeps an Xbox-authored little-endian event
stream. The engine (source/cutscene/recorded_animation_*.c) reads its unit
control, animation state and event payloads with memcpy, i.e. in native byte
order, and its version-4 event header through a compiler bit-field. This module
decodes every field with explicit shifts, masks and little-endian unpacking,
never with host struct layout, and produces a canonical big-endian "HRA1"
record the Wii runtime can read without byte swapping or bit-field reliance.
encode_xbox() rebuilds the original stream from the decoded form, so every
conversion is checked by an exact round trip.

Stream versions (recorded_animations.c playback_codec): animation versions 1-3
use the v1 codec (each event a little-endian {short type; word time_delta}
followed by its fixed payload), version 4 the current codec (one header byte:
time-delta kind in bits 0-1, event type in bits 2-7, then a byte or
little-endian word delta, then the payload).
"""
import hashlib
import struct

MAX_STREAM_BYTES = 1 << 20
MAX_EVENTS = 1 << 18
CODEC_V1, CODEC_CURRENT = 1, 4

# unit control fields per version table (recorded_animation_initialize.c):
# (code, bytes) where code is "u8", "s16", "s32" or "f32"
UNIT_CONTROL_TABLES = (
    (("u8", 1), ("u8", 1), ("s16", 2), ("s16", 2), ("s16", 2), ("f32", 4), ("f32", 4),
     ("f32", 4), ("f32", 4), ("f32", 4), ("f32", 4), ("f32", 4), ("f32", 4), ("f32", 4), ("f32", 4), ("f32", 4)),
    (("s32", 4),),
    (("s16", 2),),
    (("s16", 2),),
)

# current codec: event type -> payload fields after the header
CURRENT_PAYLOADS = {0: (), 1: (), 2: ("u8",), 3: ("u8",), 4: ("s16",), 5: ("s16",), 6: ("f32", "f32")}
CURRENT_PAYLOADS.update({t: ("s8", "s8") for t in range(7, 15)})
CURRENT_PAYLOADS.update({t: ("s16", "s16") for t in range(15, 23)})
CURRENT_END = 1

# v1 codec: event type -> payload fields after the 4-byte {type, delta} header
V1_PAYLOADS = {0: (), 1: (), 2: ("u8", "pad8"), 3: ("u8", "pad8"), 4: ("u16",), 5: ("s16",),
               6: ("f32", "f32"), 7: (), 8: ()}
V1_PAYLOADS.update({t: ("f32", "f32", "f32") for t in range(9, 16)})
V1_PAYLOADS.update({t: ("f32", "f32") for t in range(16, 23)})
V1_END = 1

FIELD_BYTES = {"u8": 1, "s8": 1, "pad8": 1, "s16": 2, "u16": 2, "s32": 4, "f32": 4}
KIND_NONE = 0xFF  # canonical delta kind for v1 events (explicit 16-bit delta)
HRA_MAGIC = b"HRA1"
HRA_HEADER = struct.Struct(">4sHBBHHIIIII")  # 32 bytes, see pack_canonical


class AnimationError(ValueError):
    """A bounded stream validation failure; no stream bytes in the message."""


def unit_control_fields(unit_control_version):
    """Field list the engine reads for a unit-control data version, or None."""
    if max(unit_control_version, 1) > len(UNIT_CONTROL_TABLES):
        return None
    count = min(max(unit_control_version, 1), len(UNIT_CONTROL_TABLES))
    return [field for table in UNIT_CONTROL_TABLES[:count] for field in table]


def _read(code, data, offset):
    """Explicit little-endian decode of one field to an integer (floats as raw bits)."""
    if code in ("u8", "pad8"):
        return data[offset]
    if code == "s8":
        v = data[offset]
        return v - 256 if v & 0x80 else v
    if code in ("u16", "s16"):
        v = data[offset] | data[offset + 1] << 8
        return v - 65536 if code == "s16" and v & 0x8000 else v
    v = data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | data[offset + 3] << 24
    return v - (1 << 32) if code == "s32" and v & 0x80000000 else v  # f32 stays as raw bits


def _write_le(code, value):
    size = FIELD_BYTES[code]
    return (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")


def decode(stream, animation_version, unit_control_version, length_in_ticks=None):
    """Decode an Xbox event stream into explicit Python values.

    Rejects what the engine's playback would treat as damaged or unreadable:
    an unknown version, a truncated unit control/state/event, an unknown
    event type, a delta inconsistent with its kind, or no end event.
    """
    data = bytes(stream)
    if not 0 < len(data) <= MAX_STREAM_BYTES:
        raise AnimationError("event stream size outside bounds")
    if animation_version in (1, 2, 3):
        codec = CODEC_V1
    elif animation_version == 4:
        codec = CODEC_CURRENT
    else:
        raise AnimationError("unsupported recorded animation version")
    fields = unit_control_fields(unit_control_version)
    if fields is None:
        raise AnimationError("unsupported unit control data version")
    position = 0
    unit_control = []
    for code, size in fields:
        if position + size > len(data):
            raise AnimationError("unit control truncated")
        unit_control.append(_read(code, data, position))
        position += size
    state = []
    if codec == CODEC_CURRENT:
        if position + 12 > len(data):
            raise AnimationError("animation state truncated")
        state = [_read("s16", data, position + 2 * i) for i in range(6)]
        position += 12
    events = []
    total_ticks = 0
    while True:
        if len(events) >= MAX_EVENTS:
            raise AnimationError("event count above bound")
        if codec == CODEC_CURRENT:
            if position >= len(data):
                raise AnimationError("stream ends before its end event")
            head = data[position]
            kind, event_type = head & 3, head >> 2
            if kind < 2:
                delta, size = kind, 1
            elif kind == 2:
                if position + 2 > len(data):
                    raise AnimationError("event delta truncated")
                delta, size = data[position + 1], 2
                if not 1 < delta <= 255:
                    raise AnimationError("byte delta outside its kind's range")
            else:
                if position + 3 > len(data):
                    raise AnimationError("event delta truncated")
                delta, size = data[position + 1] | data[position + 2] << 8, 3
                if delta <= 255:
                    raise AnimationError("word delta outside its kind's range")
            payload_codes = CURRENT_PAYLOADS.get(event_type)
            end = CURRENT_END
        else:
            if position + 4 > len(data):
                raise AnimationError("stream ends before its end event")
            event_type, delta = _read("s16", data, position), _read("u16", data, position + 2)
            kind, size = KIND_NONE, 4
            payload_codes = V1_PAYLOADS.get(event_type)
            end = V1_END
        if payload_codes is None:
            raise AnimationError("unknown event type")
        payload = []
        cursor = position + size
        for code in payload_codes:
            if cursor + FIELD_BYTES[code] > len(data):
                raise AnimationError("event payload truncated")
            payload.append(_read(code, data, cursor))
            cursor += FIELD_BYTES[code]
        events.append((event_type, kind, delta, tuple(payload)))
        total_ticks += delta
        position = cursor
        if event_type == end:
            break
    tail = data[position:]
    return {"codec": codec, "animation_version": animation_version, "unit_control_version": unit_control_version,
            "length_in_ticks": length_in_ticks, "unit_control": unit_control, "state": state,
            "events": events, "tail": tail, "total_ticks": total_ticks, "source_bytes": len(data)}


def encode_xbox(decoded):
    """Rebuild the Xbox little-endian stream from decoded values (round-trip check)."""
    out = bytearray()
    for (code, _), value in zip(unit_control_fields(decoded["unit_control_version"]), decoded["unit_control"]):
        out += _write_le(code, value)
    for value in decoded["state"]:
        out += _write_le("s16", value)
    payloads = CURRENT_PAYLOADS if decoded["codec"] == CODEC_CURRENT else V1_PAYLOADS
    for event_type, kind, delta, payload in decoded["events"]:
        if decoded["codec"] == CODEC_CURRENT:
            out.append((event_type << 2 | kind) & 0xFF)
            if kind == 2:
                out.append(delta)
            elif kind == 3:
                out += _write_le("u16", delta)
        else:
            out += _write_le("s16", event_type) + _write_le("u16", delta)
        for code, value in zip(payloads[event_type], payload):
            out += _write_le(code, value)
    return bytes(out + decoded["tail"])


def pack_canonical(decoded):
    """Canonical big-endian HRA1 record.

    32-byte header: magic, format version 1, animation version, unit-control
    version, length in ticks, unit-control field count, event count, source
    stream bytes, sum of event deltas, tail bytes, SHA-256-derived source tag
    (first 4 bytes). Then each unit-control field as a big-endian 32-bit word
    (signed values sign-extended, floats as their IEEE-754 bits), the six
    16-bit state values sign-extended to 32 bits, then one 16-byte record per
    event: type (u8), delta kind (u8; 0xFF for v1), delta (u16), and three
    32-bit payload words (signed fields sign-extended, unused words 0). The
    opaque tail after the end event follows verbatim.
    """
    def word(value):
        return struct.pack(">I", value & 0xFFFFFFFF)

    source_tag = int.from_bytes(hashlib.sha256(encode_xbox(decoded)).digest()[:4], "big")
    header = HRA_HEADER.pack(HRA_MAGIC, 1, decoded["animation_version"], decoded["unit_control_version"],
                             (decoded["length_in_ticks"] or 0) & 0xFFFF, len(decoded["unit_control"]),
                             len(decoded["events"]), decoded["source_bytes"], decoded["total_ticks"],
                             len(decoded["tail"]), source_tag)
    body = bytearray(header)
    for value in decoded["unit_control"]:
        body += word(value)
    for value in decoded["state"]:
        body += word(value)
    for event_type, kind, delta, payload in decoded["events"]:
        body += struct.pack(">BBH", event_type, kind, delta)
        for i in range(3):
            body += word(payload[i] if i < len(payload) else 0)
    return bytes(body + decoded["tail"])


def unpack_canonical(blob):
    """Parse an HRA1 record back to decoded form (used to prove it is self-describing)."""
    if len(blob) < HRA_HEADER.size:
        raise AnimationError("HRA1 header truncated")
    (magic, version, animation_version, ucv, ticks, field_count, event_count, source_bytes, total_ticks,
     tail_bytes, _tag) = HRA_HEADER.unpack_from(blob, 0)
    if magic != HRA_MAGIC or version != 1:
        raise AnimationError("HRA1 signature/version")
    fields = unit_control_fields(ucv)
    codec = CODEC_V1 if animation_version in (1, 2, 3) else CODEC_CURRENT
    if fields is None or len(fields) != field_count or animation_version not in (1, 2, 3, 4):
        raise AnimationError("HRA1 version fields inconsistent")
    state_count = 6 if codec == CODEC_CURRENT else 0
    expected = HRA_HEADER.size + 4 * (field_count + state_count) + 16 * event_count + tail_bytes
    if len(blob) != expected or event_count > MAX_EVENTS:
        raise AnimationError("HRA1 length inconsistent with its counts")

    def signed(value, bits):
        return value - (1 << bits) if value & (1 << (bits - 1)) else value

    position = HRA_HEADER.size
    unit_control = []
    for code, _ in fields:
        v = struct.unpack_from(">I", blob, position)[0]
        unit_control.append(signed(v, 32) if code in ("s16", "s32") else v)
        position += 4
    state = [signed(struct.unpack_from(">I", blob, position + 4 * i)[0], 32) for i in range(state_count)]
    position += 4 * state_count
    payloads = CURRENT_PAYLOADS if codec == CODEC_CURRENT else V1_PAYLOADS
    events = []
    for _ in range(event_count):
        event_type, kind, delta = struct.unpack_from(">BBH", blob, position)
        codes = payloads.get(event_type)
        if codes is None:
            raise AnimationError("HRA1 unknown event type")
        words = struct.unpack_from(">3I", blob, position + 4)
        payload = tuple(signed(w, 32) if c in ("s8", "s16") else w for c, w in zip(codes, words))
        events.append((event_type, kind, delta, payload))
        position += 16
    return {"codec": codec, "animation_version": animation_version, "unit_control_version": ucv,
            "length_in_ticks": ticks, "unit_control": unit_control, "state": state, "events": events,
            "tail": bytes(blob[position:]), "total_ticks": total_ticks, "source_bytes": source_bytes}


def convert(stream, animation_version, unit_control_version, length_in_ticks):
    """Decode, round-trip check and canonicalize one stream. Returns (HRA1 bytes, facts)."""
    decoded = decode(stream, animation_version, unit_control_version, length_in_ticks)
    if encode_xbox(decoded) != bytes(stream):
        raise AnimationError("decoded stream does not reproduce its source")
    canonical = pack_canonical(decoded)
    again = unpack_canonical(canonical)
    if encode_xbox(again) != bytes(stream):
        raise AnimationError("canonical record does not reproduce its source")
    counts = {}
    for event_type, *_ in decoded["events"]:
        counts[event_type] = counts.get(event_type, 0) + 1
    return canonical, {"events": len(decoded["events"]), "event_types": dict(sorted(counts.items())),
                       "total_ticks": decoded["total_ticks"], "tail_bytes": len(decoded["tail"]),
                       "round_trip": "exact", "codec": decoded["codec"]}
