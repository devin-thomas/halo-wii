#!/usr/bin/env python3
"""Read and check the canonical game-state image (HWI-076, ADR-021).

An independent reader of the format that source/saved games/game_state_image.c
writes (its comment is the specification): the 512-byte header and its CRC,
the chunks and theirs, the segments, the length and the SHA-256 of the
payload. It does not know the engine's layout, so it checks the container
and gives back the game state's words (little-endian, pointers made places);
the engine checks the rest before it takes an image.

    python tools/wii/game_state_image_format.py <image> [--words <out.bin>] [--json]
    python tools/wii/game_state_image_format.py <image> --address-like <low> <high>

The CRCs are the engine's (crc.c): CRC-32 with polynomial 0xEDB88320,
starting at 0xFFFFFFFF and not inverted at the end, which is zlib's CRC-32
inverted.
"""

import argparse
import hashlib
import json
import struct
import sys
import zlib

HEADER_BYTES = 512
CHUNK_HEADER_BYTES = 16
CHUNK_BYTES = 0x10000
VERSION = 2
LITERAL_MAXIMUM_WORDS = 1024
ZERO_RUN_MINIMUM_WORDS = 3
ZERO_RUN_BIT = 0x80000000
POINTER_TAG = 0x40000000
FLAG_COMPLETE = 1
FLAG_BASE_BOUND = 2

FIELDS = (  # (name, offset, struct format)
    ("version", 4, "<I"), ("header_bytes", 8, "<I"), ("flags", 12, "<I"), ("byte_order", 16, "<I"),
    ("encoding", 20, "<I"), ("chunk_bytes", 24, "<I"), ("chunk_count", 28, "<I"), ("payload_bytes", 32, "<I"),
    ("state_bytes", 36, "<I"), ("cpu_used", 40, "<I"), ("gpu_used", 44, "<I"), ("state_size", 48, "<I"),
    ("allocation_count", 52, "<I"), ("layout_digest", 56, "<I"), ("allocation_checksum", 60, "<I"),
    ("cache_file_checksum", 64, "<I"), ("player_count", 68, "<h"), ("difficulty", 70, "<h"),
    ("save_base", 392, "<I"), ("foreign_pointers", 396, "<I"), ("pool_blocks", 400, "<I"),
    ("script_threads", 404, "<I"), ("pointers", 408, "<I"), ("header_checksum", 508, "<I"),
)


class ImageError(ValueError):
    """an image this reader refuses, with the engine's name for why"""

    def __init__(self, refusal, detail=""):
        super().__init__(f"{refusal}: {detail}" if detail else refusal)
        self.refusal = refusal


def engine_crc(data: bytes) -> int:
    return zlib.crc32(data) ^ 0xFFFFFFFF


def parse_header(header: bytes) -> dict:
    if len(header) < HEADER_BYTES:
        raise ImageError("read", "shorter than its header")
    if header[:4] != b"HWGS":
        raise ImageError("magic")
    fields = {name: struct.unpack_from(fmt, header, at)[0] for name, at, fmt in FIELDS}
    if fields["version"] != VERSION or fields["header_bytes"] != HEADER_BYTES:
        raise ImageError("version", str(fields["version"]))
    if fields["header_checksum"] != engine_crc(header[:508]):
        raise ImageError("header_checksum")
    if not fields["flags"] & FLAG_COMPLETE:
        raise ImageError("incomplete")
    fields["map_name"] = header[72:328].split(b"\0", 1)[0].decode("ascii", "replace")
    fields["build_number"] = header[328:360].split(b"\0", 1)[0].decode("ascii", "replace")
    fields["digest"] = header[360:392].hex()
    fields["base_bound"] = bool(fields["flags"] & FLAG_BASE_BOUND)
    return fields


def payload_of(image: bytes, header: dict) -> bytes:
    """the chunks' payloads, each checked, one after another"""
    payload_bytes, chunk_bytes, count = header["payload_bytes"], header["chunk_bytes"], header["chunk_count"]
    if chunk_bytes != CHUNK_BYTES or payload_bytes < 4 or payload_bytes % 4 or \
            count != (payload_bytes + chunk_bytes - 1) // chunk_bytes:
        raise ImageError("length")
    out, at = bytearray(), HEADER_BYTES
    for index in range(count):
        size = min(chunk_bytes, payload_bytes - len(out))
        chunk = image[at:at + CHUNK_HEADER_BYTES + size]
        if len(chunk) != CHUNK_HEADER_BYTES + size:
            raise ImageError("read", f"chunk {index}")
        magic, number, length, crc = struct.unpack_from("<4sIII", chunk)
        if magic != b"HWGC" or number != index or length != size:
            raise ImageError("chunk", str(index))
        if crc != engine_crc(chunk[CHUNK_HEADER_BYTES:]):
            raise ImageError("chunk_checksum", str(index))
        out += chunk[CHUNK_HEADER_BYTES:]
        at += len(chunk)
    if hashlib.sha256(out).hexdigest() != header["digest"]:
        raise ImageError("digest")
    return bytes(out)


def words_of(payload: bytes, state_words: int) -> list:
    """the game state's words from the segments"""
    words, at = [], 0
    while at < len(payload):
        control = struct.unpack_from("<I", payload, at)[0]
        at += 4
        count = control & ~ZERO_RUN_BIT & 0xFFFFFFFF
        if count < 1 or len(words) + count > state_words:
            raise ImageError("segment", str(at - 4))
        if control & ZERO_RUN_BIT:
            words.extend([0] * count)
        else:
            if count > LITERAL_MAXIMUM_WORDS or at + 4 * count > len(payload):
                raise ImageError("segment", str(at - 4))
            words.extend(struct.unpack_from(f"<{count}I", payload, at))
            at += 4 * count
    if len(words) != state_words:
        raise ImageError("length", "words")
    return words


def encode_words(words) -> bytes:
    """the segments the engine writes for these words (its rule, for tests)"""
    out, literal, zeros = bytearray(), [], 0

    def flush_literal():
        if literal:
            out.extend(struct.pack("<I", len(literal)) + struct.pack(f"<{len(literal)}I", *literal))
            literal.clear()

    def flush_zeros():
        nonlocal zeros
        if zeros >= ZERO_RUN_MINIMUM_WORDS:
            flush_literal()
            out.extend(struct.pack("<I", ZERO_RUN_BIT | zeros))
        else:
            for _ in range(zeros):
                literal.append(0)
                if len(literal) == LITERAL_MAXIMUM_WORDS:
                    flush_literal()
        zeros = 0

    for word in words:
        if not word:
            zeros += 1
            continue
        if zeros:
            flush_zeros()
        literal.append(word)
        if len(literal) == LITERAL_MAXIMUM_WORDS:
            flush_literal()
    if zeros:
        flush_zeros()
    flush_literal()
    return bytes(out)


def build_image(words, **fields) -> bytes:
    """an image of these words with this header (for tests): what the engine writes"""
    payload = encode_words(words)
    chunks = [payload[at:at + CHUNK_BYTES] for at in range(0, len(payload), CHUNK_BYTES)]
    header = bytearray(HEADER_BYTES)
    header[:4] = b"HWGS"
    values = {"version": VERSION, "header_bytes": HEADER_BYTES, "flags": FLAG_COMPLETE, "byte_order": 1,
              "encoding": 1, "chunk_bytes": CHUNK_BYTES, "chunk_count": len(chunks), "payload_bytes": len(payload),
              "state_bytes": 4 * len(words), "cpu_used": 4 * len(words), "gpu_used": 0, "state_size": 4 * len(words)}
    values.update({key: value for key, value in fields.items() if key not in ("map_name", "build_number")})
    for name, at, fmt in FIELDS:
        if name in values:
            struct.pack_into(fmt, header, at, values[name])
    header[72:72 + len(fields.get("map_name", ""))] = fields.get("map_name", "").encode("ascii")
    header[328:328 + len(fields.get("build_number", ""))] = fields.get("build_number", "").encode("ascii")
    header[360:392] = hashlib.sha256(payload).digest()
    struct.pack_into("<I", header, 508, engine_crc(bytes(header[:508])))
    body = b"".join(struct.pack("<4sIII", b"HWGC", index, len(chunk), engine_crc(chunk)) + chunk
                    for index, chunk in enumerate(chunks))
    return bytes(header) + body


def read_image(image: bytes):
    """(header fields, the game state's words); ImageError if the container is damaged"""
    header = parse_header(image[:HEADER_BYTES])
    if header["state_bytes"] % 4 or header["state_bytes"] != header["cpu_used"] + header["gpu_used"]:
        raise ImageError("layout")
    payload = payload_of(image, header)
    return header, words_of(payload, header["state_bytes"] // 4)


def word_offset(header: dict, index: int) -> int:
    """the game-state offset of the index-th word (the CPU part, then the GPU part)"""
    cpu_words = header["cpu_used"] // 4
    return 4 * index if index < cpu_words else header["state_size"] - header["gpu_used"] + 4 * (index - cpu_words)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image")
    parser.add_argument("--words", help="write the game state's words (little-endian) here")
    parser.add_argument("--address-like", nargs=2, metavar=("LOW", "HIGH"),
                        help="list the words in [low, high): addresses left in the image")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    data = open(args.image, "rb").read()
    try:
        header, words = read_image(data)
    except ImageError as error:
        print(json.dumps({"valid": False, "refusal": error.refusal, "detail": str(error)}))
        return 1
    summary = {"valid": True, "file_bytes": len(data), "words": len(words),
               "zero_words": sum(1 for word in words if not word),
               "pointer_tagged_words": sum(1 for word in words if word & 0xC0000000 == POINTER_TAG),
               **{key: header[key] for key in ("map_name", "build_number", "byte_order", "chunk_count",
                                               "payload_bytes", "state_bytes", "pointers", "pool_blocks",
                                               "script_threads", "foreign_pointers", "base_bound", "digest")}}
    if args.address_like:
        low, high = (int(value, 0) for value in args.address_like)
        summary["address_like"] = [{"offset": word_offset(header, index), "word": f"{word:08x}"}
                                   for index, word in enumerate(words) if low <= word < high]
    if args.words:
        with open(args.words, "wb") as out:
            out.write(struct.pack(f"<{len(words)}I", *words))
    print(json.dumps(summary, indent=None if not args.json else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
