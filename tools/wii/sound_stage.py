#!/usr/bin/env python3
"""Stage a map's sound samples for the Wii (HWI-074): <map>.wsnd, the sound pack.

The Xbox engine reads a sound permutation's samples from the map file, at
their offset, into its sound cache (source/cache/xbox_sound_cache.c); the Wii's
staged map (tools/wii/map_stage.py, <map>.wmap) holds no samples. This packs
the map's permutations, each the converter's HWS1 container from a published
generation (tools/wii/content_convert.py: Xbox ADPCM kept verbatim, PCM
big-endian), keyed by the samples' offset in the map, for the Wii's cache
layer to serve (port/wii/sound/sound_pack.c, whose comment has the layout).

Everything is checked before anything is written: each container's SHA-256
against the generation's manifest, its header against the map's permutation,
and its payload against the map's own bytes at the offset (PCM after
restoring its byte order). A permutation listed twice must be the same bytes.
The pack is written beside a record (<map>.wsnd.json) and published
atomically (a temporary file, then a rename): an interrupted staging leaves
the previous pack as it was.

The decoded PCM a Wii launch logs (SOUND_DECODE lines, sound_pack.c) is
compared with this host's reference decoder (media_formats.decode_xbox_adpcm,
HWI-022's) by tools/wii/compare_sound_runs.py, through decoded_crc().

    python tools/wii/sound_stage.py --map <owned .map> --generation <generation dir> --output <private dir>

The map and the generation are owned data: the output is private and never
committed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))

import content_convert as cc  # noqa: E402
import halo_cache as hc  # noqa: E402
import media_formats as mf  # noqa: E402

PACK_MAGIC = b"HWSPACK1"
PACK_VERSION = 1
HEADER_BYTES = 64
ENTRY_BYTES = 24
ALIGNMENT = 32
HWS_HEADER_BYTES = 32
CODECS = {"xbox_adpcm": 1, "pcm16be": 2}


class StageError(ValueError):
    pass


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_hws(data):
    """(codec number, rate, channels, frames, payload) of an HWS1 container"""
    if len(data) < HWS_HEADER_BYTES or data[:4] != b"HWS1":
        raise StageError("not an HWS1 container")
    _magic, version, codec, rate, channels, reserved, frames, payload_bytes = struct.unpack_from(">4sHHIHHII", data)
    if version != 1 or reserved or any(data[24:HWS_HEADER_BYTES]) or payload_bytes != len(data) - HWS_HEADER_BYTES:
        raise StageError("HWS1 header inconsistent")
    if codec not in CODECS.values():
        raise StageError("HWS1 codec outside the profile")
    return codec, rate, channels, frames, bytes(data[HWS_HEADER_BYTES:])


def map_bytes_of(codec, channels, payload):
    """the samples as the map holds them (PCM back to little-endian)"""
    if codec == CODECS["pcm16be"]:
        return mf.pcm16le_to_be(payload, channels)  # the swap is its own inverse
    return payload


def plan(cache, map_name, generation):
    """[(offset, size, map crc, container bytes, codec)] in offset order"""
    manifest = json.loads((generation / "manifest.json").read_text(encoding="utf-8"))
    hashes = {entry["path"]: entry["sha256"] for entry in manifest["outputs"] if entry.get("kind") == "sound"}
    by_offset = {}
    for item in cc.plan_sounds(cache, map_name):
        path = generation / item["path"]
        if item["path"] not in hashes:
            raise StageError(f"{item['path']} is not in the generation's manifest")
        container = path.read_bytes()
        if hashlib.sha256(container).hexdigest() != hashes[item["path"]]:
            raise StageError(f"{item['path']} differs from the generation's manifest")
        codec, rate, channels, frames, payload = parse_hws(container)
        expected_codec = CODECS["pcm16be"] if item["codec"] == "pcm16le" else CODECS[item["codec"]]
        if (codec, rate, channels, frames) != (expected_codec, item["rate"], item["channels"], item["frames"]):
            raise StageError(f"{item['path']} does not describe the map's permutation")
        original = bytes(cache.file_span(item["file_offset"], item["size"]))
        if map_bytes_of(codec, channels, payload) != original:
            raise StageError(f"{item['path']} is not the map's samples at {item['file_offset']:#x}")
        key = item["file_offset"]
        record = (key, item["size"], crc32(original), container, codec)
        if key in by_offset:
            if by_offset[key][1:3] != record[1:3]:
                raise StageError(f"two permutations at {key:#x} with different samples")
            continue
        by_offset[key] = record
    if not by_offset:
        raise StageError(f"{map_name} has no sound permutations")
    return [by_offset[key] for key in sorted(by_offset)]


def pack_bytes(map_name, generation_id, entries):
    """the pack's bytes (sound_pack.c's layout)"""
    if len(map_name.encode("ascii")) > 15 or len(generation_id) != 16:
        raise StageError("map name or generation id does not fit the header")
    count = len(entries)
    containers_at = HEADER_BYTES + count * 4 + count * ENTRY_BYTES
    position = (containers_at + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT
    offsets, table, body = bytearray(), bytearray(), bytearray()
    for offset, size, crc, container, codec in entries:
        offsets += struct.pack(">I", offset)
        table += struct.pack(">IIIIIHH", offset, size, position, len(container), crc, codec, 0)
        body += bytes(position - containers_at - len(body)) + container
        position += (len(container) + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT
    total = containers_at + len(body)
    header = struct.pack(">8sIIIII16s16s", PACK_MAGIC, PACK_VERSION, count, crc32(offsets), crc32(table), total,
                         map_name.encode("ascii"), generation_id.encode("ascii"))
    header += struct.pack(">I", crc32(header))
    assert len(header) == HEADER_BYTES
    return header + offsets + table + body


def read_pack(path):
    """(header facts, [entry dicts]) of a pack, checked as sound_pack.c checks it"""
    data = Path(path).read_bytes()
    if len(data) < HEADER_BYTES or data[:8] != PACK_MAGIC:
        raise StageError("not a sound pack")
    magic, version, count, offsets_crc, table_crc, total, name, generation = struct.unpack_from(">8sIIIII16s16s", data)
    if version != PACK_VERSION or struct.unpack_from(">I", data, 60)[0] != crc32(data[:60]) or total != len(data):
        raise StageError("sound pack header damaged")
    offsets = data[HEADER_BYTES:HEADER_BYTES + count * 4]
    table = data[HEADER_BYTES + count * 4:HEADER_BYTES + count * (4 + ENTRY_BYTES)]
    if crc32(offsets) != offsets_crc or crc32(table) != table_crc:
        raise StageError("sound pack tables damaged")
    entries = []
    for index in range(count):
        offset, size, at, length, crc, codec, _zero = struct.unpack_from(">IIIIIHH", table, index * ENTRY_BYTES)
        entries.append({"offset": offset, "size": size, "container": data[at:at + length], "crc": crc,
                        "codec": codec})
    return {"map": name.rstrip(b"\0").decode("ascii"), "generation": generation.decode("ascii"),
            "entries": count, "bytes": total}, entries


def decoded_crc(container):
    """(frames, CRC-32 of the decoded PCM as 16-bit little-endian), by the
    host reference decoder; PCM's own samples (little-endian) for PCM"""
    codec, _rate, channels, frames, payload = parse_hws(container)
    if codec == CODECS["xbox_adpcm"]:
        samples = mf.decode_xbox_adpcm(payload, channels)
        return frames, crc32(struct.pack("<%dh" % len(samples), *samples))
    little = map_bytes_of(codec, channels, payload)
    return frames, crc32(little[:len(little) - len(little) % (2 * channels)])


def publish(output, name, data, record):
    output.mkdir(parents=True, exist_ok=True)
    target = output / f"{name}.wsnd"
    temporary = output / f".{name}.wsnd.tmp"
    temporary.write_bytes(data)
    with temporary.open("rb+") as stream:
        os.fsync(stream.fileno())
    os.replace(temporary, target)
    (output / f"{name}.wsnd.json").write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8", newline="\n")
    return target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--map", type=Path, required=True, help="an owned Xbox .map (private)")
    parser.add_argument("--generation", type=Path, required=True,
                        help="a published converter generation (content_convert.py; private)")
    parser.add_argument("--output", type=Path, required=True, help="a private directory")
    args = parser.parse_args(argv)
    started = time.perf_counter()
    name = args.map.stem
    generation_id = args.generation.name
    try:
        cache = hc.open_map(args.map)
        entries = plan(cache, name, args.generation)
        data = pack_bytes(name, generation_id, entries)
    except (StageError, hc.CacheError, mf.MediaError, cc.ConversionError, OSError) as error:
        print(f"sound_stage: {name}: {error}", file=sys.stderr)
        return 1
    codecs = {}
    for entry in entries:
        codecs[entry[4]] = codecs.get(entry[4], 0) + 1
    record = {"format": "HWSPACK1", "map": name, "source_sha256": cache.facts.get("source_sha256"),
              "generation": generation_id, "entries": len(entries), "bytes": len(data),
              "sample_bytes": sum(entry[1] for entry in entries),
              "codecs": {str(key): value for key, value in sorted(codecs.items())},
              "sha256": hashlib.sha256(data).hexdigest(), "seconds": round(time.perf_counter() - started, 1)}
    publish(args.output, name, data, record)
    print(json.dumps(record))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
