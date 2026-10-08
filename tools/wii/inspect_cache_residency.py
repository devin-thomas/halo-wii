"""Stream a supported Xbox v5 map into a bounded tag-index inspection.

No conversion or canonical checksum validation. Optional private tag bytes are
written only to a new destination; no game content belongs in the repository.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import zlib

BASE = 0x803A6000
TAG_LIMIT = 0x1600000
MAP_LIMIT = 0x11600000
CHUNK = 65536


def span(address, count, stride, size):
    if count < 0 or stride <= 0 or address < BASE:
        raise ValueError("invalid encoded address/count/stride")
    offset = address - BASE
    length = count * stride
    if offset > size or length > size - offset or address + length > 0x100000000:
        raise ValueError("encoded span outside supplied tag bytes")
    return offset, length


def inspect(path, tag_output=None):
    path = Path(path)
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
        stream.seek(0)
        header = stream.read(2048)
        if len(header) != 2048:
            raise ValueError("truncated map header")
        signature, version, declared, _, tag_offset, tag_size = struct.unpack_from("<6I", header)
        if signature != 0x68656164 or struct.unpack_from("<I", header, 2044)[0] != 0x666F6F74 or version != 5:
            raise ValueError("only Xbox v5 head/foot identity supported")
        if not 2048 <= declared <= MAP_LIMIT or not 36 <= tag_size <= TAG_LIMIT:
            raise ValueError("map/tag size outside bounded profile")
        if tag_offset < 2048 or tag_offset > declared - tag_size:
            raise ValueError("tag extent outside declared map")
        tags = bytearray()
        position = 2048
        decoder = zlib.decompressobj()
        stream_bytes = 0
        trailing_bytes = 0
        body_crc = 0
        while True:
            compressed = stream.read(CHUNK)
            if not compressed:
                break
            if decoder.eof:
                trailing_bytes += len(compressed)
                continue
            pending = compressed
            while pending:
                output = decoder.decompress(pending, CHUNK)
                # At EOF both tails may contain the same trailing bytes.
                stream_bytes += len(pending) - (len(decoder.unused_data) if decoder.eof else len(decoder.unconsumed_tail))
                pending = decoder.unconsumed_tail
                if position + len(output) > declared:
                    raise ValueError("inflated output exceeds declared map length")
                lo = max(position, tag_offset)
                hi = min(position + len(output), tag_offset + tag_size)
                if lo < hi:
                    tags.extend(output[lo - position:hi - position])
                body_crc = zlib.crc32(output, body_crc)
                position += len(output)
                if decoder.eof:
                    trailing_bytes += len(decoder.unused_data)
                    break
        if not decoder.eof or position != declared or len(tags) != tag_size:
            raise ValueError("compressed stream EOF/exact output/tag extent mismatch")
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("input metadata changed during inspection")
    values = struct.unpack_from("<9I", tags)
    instance_address, scenario, checksum, count, vertex_count, vertex_address, index_count, index_address, sig = values
    if sig != 0x74616773 or count > 65535 or vertex_count > 0x7FFFFFFF or index_count > 0x7FFFFFFF:
        raise ValueError("unsupported tag-index signature/count")
    table, table_size = span(instance_address, count, 32, len(tags))
    for n, address in ((vertex_count, vertex_address), (index_count, index_address)):
        if n:
            span(address, n, 12, len(tags))
    instances = []
    classes = {}
    outside_roots = 0
    for ordinal in range(count):
        row = struct.unpack_from("<8I", tags, table + ordinal * 32)
        group, parent0, parent1, datum, name_address, root_address, unused0, unused1 = row
        if (datum & 65535) != ordinal:
            raise ValueError("tag datum ordinal mismatch")
        name, _ = span(name_address, 1, 1, len(tags))
        if tags.find(0, name) < 0:
            raise ValueError("tag name lacks bounded terminator")
        if not BASE <= root_address < BASE + len(tags):
            outside_roots += 1
        classes[f"{group:08x}"] = classes.get(f"{group:08x}", 0) + 1
        instances.append(row)
    scenario_ordinal = scenario & 65535
    if scenario_ordinal >= count or instances[scenario_ordinal][3] != scenario or instances[scenario_ordinal][0] != 0x73636E72:
        raise ValueError("scenario datum/class mismatch")
    scenario_root, _ = span(instances[scenario_ordinal][5], 0x5A4 + 12, 1, len(tags))
    bsp_count, bsp_address, _ = struct.unpack_from("<3I", tags, scenario_root + 0x5A4)
    if bsp_count > 16:
        raise ValueError("scenario BSP count exceeds source schema limit")
    bsps = []
    if bsp_count:
        bsp_table, _ = span(bsp_address, bsp_count, 32, len(tags))
        for i in range(bsp_count):
            file_offset, file_size, address = struct.unpack_from("<3I", tags, bsp_table + i * 32)
            if file_offset > declared or file_size > declared - file_offset:
                raise ValueError("BSP file extent outside declared map")
            if file_size < 24 or address < BASE or address - BASE > TAG_LIMIT or file_size > TAG_LIMIT - (address - BASE):
                raise ValueError("BSP runtime extent outside Xbox tag reservation")
            bsps.append({"ordinal": i, "file_offset": file_offset, "bytes": file_size,
                         "encoded_address": address, "tag_window_end_offset": address - BASE + file_size})
    result = {"scope": "streaming_xbox_v5_index_and_bsp_metadata_not_conversion",
              "input_sha256": digest.hexdigest(), "stored_bytes": before.st_size,
              "declared_bytes": declared, "tag_bytes": tag_size, "tag_file_offset": tag_offset,
              "tag_sha256": hashlib.sha256(tags).hexdigest(), "tag_crc32": zlib.crc32(tags),
              "inflated_body_crc32_noncanonical": body_crc,
              "compressed_stream_bytes": stream_bytes, "trailing_stored_bytes": trailing_bytes,
              "inflate_chunk_bound": CHUNK, "retained_tag_bound": TAG_LIMIT,
              "tag_base": BASE, "header_bytes": 36, "instance_stride": 32,
              "instance_address": instance_address, "instance_count": count, "instance_table_bytes": table_size,
              "scenario_datum": scenario, "header_checksum_observed_not_validated": checksum,
              "vertex_count": vertex_count, "index_count": index_count,
              "classes": classes, "roots_outside_tag_bytes": outside_roots, "bsps": bsps,
              "maximum_bsp_bytes": max((r["bytes"] for r in bsps), default=0),
              "maximum_tag_window_end": max([tag_size] + [r["tag_window_end_offset"] for r in bsps]),
              "source_metadata_unchanged": True, "conversion_performed": False}
    if tag_output is not None:
        destination = Path(tag_output)
        partial = destination.with_name(destination.name + ".partial")
        if destination.exists() or partial.exists():
            raise ValueError("refusing to overwrite private tag output")
        with partial.open("xb") as output:
            output.write(tags)
            output.flush()
            os.fsync(output.fileno())
        # No competing writer may create destination during this private run.
        if destination.exists():
            raise ValueError("tag destination appeared during write; partial retained")
        partial.rename(destination)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag-output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("refusing to overwrite inspection report")
    try:
        record = inspect(args.map, args.tag_output)
        with args.output.open("x", encoding="utf-8") as report:
            json.dump(record, report, indent=2)
            report.write("\n")
    except (OSError, ValueError, zlib.error) as error:
        parser.exit(1, f"cache inspection failed: {error}\n")
    print(f"Cache index inspected: {record['instance_count']} tags / {record['tag_bytes']} bytes")


if __name__ == "__main__":
    main()
