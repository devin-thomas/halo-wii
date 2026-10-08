"""Inspect bounded Xbox BSP residency metadata without exporting asset content.

Only the selected scenario association, 24-byte BSP header, 648-byte root extent
and 12-byte descriptor tables are checked. Descriptor Data has a one-byte span;
its payload size, geometry and nested root fields remain unvalidated. Inputs and
the exclusive numeric output stay outside the checkout. Stat guards are not locks.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import zlib

from inspect_widget_graph import InspectionError, external_path, write_exclusive


BASE = 0x803A6000
MAX_BYTES = 0x01600000
UINT32_MAX = 0xFFFFFFFF
INT32_MAX = 0x7FFFFFFF
SCENARIO_GROUP = 0x73636E72
BSP_GROUP = 0x73627370
SCENARIO_BYTES = 0x5B0
BSP_ROOT_BYTES = 648
CHECKOUT = Path(__file__).resolve().parents[2]


def _integer(value, lower, upper, reason):
    if type(value) is not int or not lower <= value <= upper:
        raise InspectionError(reason)


def resolve_windows(windows, address, count, stride):
    """Resolve a span wholly within either of two truthful encoded byte windows.

    The gap and rounded reservation tail are not readable windows. Empty spans
    preserve but do not follow their unsigned address word. Arithmetic models
    uint32 address space explicitly even though Python integers do not overflow.
    """
    if len(windows) != 2:
        raise InspectionError("two valid windows required")
    for base, size in windows:
        _integer(base, 0, UINT32_MAX, "window base argument")
        _integer(size, 0, UINT32_MAX, "window size argument")
        if base + size > UINT32_MAX + 1:
            raise InspectionError("encoded window overflow")
    (a, alen), (b, blen) = windows
    if alen and blen and max(a, b) < min(a + alen, b + blen):
        raise InspectionError("valid window overlap")
    _integer(address, 0, UINT32_MAX, "address word argument")
    _integer(count, 0, INT32_MAX, "signed span count")
    _integer(stride, 0, UINT32_MAX, "span stride argument")
    if count == 0:
        return {"window": None, "offset": 0, "bytes": 0}
    if stride == 0:
        raise InspectionError("positive span stride")
    length = count * stride
    if length > UINT32_MAX or address + length > UINT32_MAX + 1:
        raise InspectionError("encoded span overflow")
    for index, (base, size) in enumerate(windows):
        offset = address - base
        if 0 <= offset <= size and length <= size - offset:
            return {"window": index, "offset": offset, "bytes": length}
    raise InspectionError("span outside valid windows or in unread gap")


def _region(windows, region, address, count, stride):
    span = resolve_windows(windows, address, count, stride)
    if span["bytes"] and span["window"] != region:
        raise InspectionError("span belongs to wrong region")
    return span


def _claim(claims, span):
    if not span["bytes"]:
        return
    start, end = span["offset"], span["offset"] + span["bytes"]
    if any(max(start, a) < min(end, b) for a, b in claims):
        raise InspectionError("known header root table overlap")
    claims.append((start, end))


def _digest(blob):
    return {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
            "crc32": zlib.crc32(blob)}


def inspect_bytes(tags, bsp, declared_map_length, bsp_ordinal, tag_base=BASE):
    """Inspect immutable snapshots, never interpreting numeric words as pointers."""
    if not isinstance(tags, bytes) or not isinstance(bsp, bytes):
        raise InspectionError("inputs must be immutable bytes")
    if not 36 <= len(tags) <= MAX_BYTES or not 24 <= len(bsp) <= MAX_BYTES:
        raise InspectionError("input size outside bounded regions")
    _integer(declared_map_length, 1, INT32_MAX, "declared map length argument")
    _integer(bsp_ordinal, 0, 15, "BSP ordinal argument")
    _integer(tag_base, 0, UINT32_MAX, "tag base argument")
    if tag_base + MAX_BYTES > UINT32_MAX + 1:
        raise InspectionError("tag reservation encoded overflow")
    words = list(struct.unpack_from("<9I", tags))
    count, vertices, indices = (struct.unpack_from("<i", tags, offset)[0]
                                for offset in (12, 16, 24))
    if words[8] != 0x74616773:
        raise InspectionError("tag header signature")
    if not 1 <= count <= 65535 or vertices < 0 or indices < 0:
        raise InspectionError("tag header signed counts")
    # Initially only tags are readable; the second window is empty until a
    # scenario reference establishes the independently bounded BSP placement.
    tag_windows = ((tag_base, len(tags)), (tag_base + len(tags), 0))
    claims = []
    _claim(claims, {"offset": 0, "bytes": 36})
    table = _region(tag_windows, 0, words[0], count, 32)
    _claim(claims, table)
    for address, number in ((words[5], vertices), (words[7], indices)):
        _claim(claims, _region(tag_windows, 0, address, number, 12))

    def instance(datum, group):
        ordinal = datum & 0xFFFF
        if datum == UINT32_MAX or ordinal >= count:
            raise InspectionError("datum ordinal")
        record = list(struct.unpack_from("<8I", tags, table["offset"] + ordinal * 32))
        if record[3] != datum:
            raise InspectionError("full instance datum identity")
        if record[0] != group:
            raise InspectionError("instance group identity")
        return record

    scenario_instance = instance(words[1], SCENARIO_GROUP)
    scenario = _region(tag_windows, 0, scenario_instance[5], 1, SCENARIO_BYTES)
    _claim(claims, scenario)
    ref_field = scenario["offset"] + 0x5A4
    ref_count, ref_address, ref_definition = struct.unpack_from("<iII", tags, ref_field)
    if not 0 <= ref_count <= 16:
        raise InspectionError("scenario BSP signed count")
    refs = _region(tag_windows, 0, ref_address, ref_count, 32)
    _claim(claims, refs)
    if bsp_ordinal >= ref_count:
        raise InspectionError("selected BSP ordinal outside block")
    reference_offset = refs["offset"] + bsp_ordinal * 32
    reference = list(struct.unpack_from("<8I", tags, reference_offset))
    file_offset, file_size = struct.unpack_from("<ii", tags, reference_offset)
    bsp_base = reference[2]
    if file_offset < 0 or not 24 <= file_size <= MAX_BYTES:
        raise InspectionError("BSP signed file extent")
    if file_size != len(bsp):
        raise InspectionError("BSP sidecar length identity")
    if file_offset > declared_map_length - file_size:
        raise InspectionError("BSP logical extent outside declared map")
    rounded = (file_size + 511) & ~511
    if (bsp_base < tag_base + len(tags) or
            bsp_base + rounded > tag_base + MAX_BYTES):
        raise InspectionError("BSP rounded reservation outside tag window or overlaps retained tags")
    if reference[4] != BSP_GROUP:
        raise InspectionError("BSP reference group identity")
    bsp_instance = instance(reference[7], BSP_GROUP)
    if bsp_instance[5] != 0:
        raise InspectionError("BSP instance must be unloaded with zero root")
    windows = ((tag_base, len(tags)), (bsp_base, len(bsp)))
    header = list(struct.unpack_from("<6I", bsp))
    if header[5] != BSP_GROUP:
        raise InspectionError("BSP header signature")
    counts = (struct.unpack_from("<i", bsp, 4)[0], struct.unpack_from("<i", bsp, 12)[0])
    if min(counts) < 0:
        raise InspectionError("BSP header signed descriptor count")
    root = _region(windows, 1, header[0], 1, BSP_ROOT_BYTES)
    bsp_claims = []
    _claim(bsp_claims, {"offset": 0, "bytes": 24})
    _claim(bsp_claims, root)
    descriptor_tables = []
    for number, address in zip(counts, (header[2], header[4])):
        span = _region(windows, 1, address, number, 12)
        _claim(bsp_claims, span)
        descriptor_tables.append({"count": number, "address_word": address, "span": span})
    for table_info in descriptor_tables:
        span = table_info["span"]
        low = high = None
        for offset in range(span["offset"], span["offset"] + span["bytes"], 12):
            data_address = struct.unpack_from("<I", bsp, offset + 4)[0]
            _region(windows, 1, data_address, 1, 1)
            low = data_address if low is None else min(low, data_address)
            high = data_address if high is None else max(high, data_address)
        table_info.update(data_spans_checked=table_info["count"],
                          minimum_data_address=low, maximum_data_address=high,
                          raw_table=_digest(memoryview(bsp)[span["offset"]:span["offset"] + span["bytes"]]))
    return {"format_version": 1, "scope": "selected BSP association and two-window residency metadata only",
            "tag_input": _digest(tags), "bsp_input": _digest(bsp), "tag_base": tag_base,
            "tag_input_sha256": hashlib.sha256(tags).hexdigest(),
            "bsp_input_sha256": hashlib.sha256(bsp).hexdigest(),
            "declared_map_length": declared_map_length, "tag_header_words": words,
            "instance_table": table, "scenario_instance_words": scenario_instance,
            "scenario_root": scenario, "BSP_block": {"count": ref_count, "address_word": ref_address,
                "definition_word": ref_definition, "span": refs},
            "selected_reference_words": reference, "BSP_instance_words": bsp_instance,
            "BSP_header_words": header, "BSP_root": root, "descriptor_tables": descriptor_tables,
            "valid_windows": [{"base": base, "bytes": size} for base, size in windows],
            "goldens": {"scenario_datum": words[1], "datum": reference[7], "bsp_ordinal": bsp_ordinal,
                "encoded_base": bsp_base, "root_address": header[0], "root_offset": root["offset"],
                "declared_map_bytes": declared_map_length, "file_bytes": file_size,
                "rounded_reservation_bytes": rounded, "vertex_count": counts[0], "index_count": counts[1],
                "serialized_bytes": len(bsp), "serialized_crc32": zlib.crc32(bsp),
                "serialized_sha256": hashlib.sha256(bsp).hexdigest()},
            "limits": {"tag_reservation_bytes": MAX_BYTES, "BSP_count": 16,
                "scenario_root_bytes": SCENARIO_BYTES, "BSP_root_bytes": BSP_ROOT_BYTES,
                "BSP_header_bytes": 24, "descriptor_stride": 12, "descriptor_Data_extent": 1},
            "limitations": ["selected_reference_only_other_BSP_references_not_validated",
                "numeric_addresses_preserved_no_relocation_or_native_pointer_use",
                "rounded_reservation_tail_and_gap_not_readable",
                "names_scripts_geometry_nested_fields_and_Data_payload_sizes_not_validated",
                "whole_BSP_hashes_are_inspection_identity_not_canonical_Xbox_checksums",
                "no_engine_registration_runtime_gameplay_or_hardware_qualification"]}


def _identity(stat):
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def inspect_files(tag_path, bsp_path, declared_map_length, bsp_ordinal, tag_base=BASE):
    paths = [external_path(tag_path), external_path(bsp_path)]
    with ExitStack() as stack:
        streams = [stack.enter_context(path.open("rb")) for path in paths]
        before = [os.fstat(stream.fileno()) for stream in streams]
        if not 36 <= before[0].st_size <= MAX_BYTES or not 24 <= before[1].st_size <= MAX_BYTES:
            raise InspectionError("input size outside bounded regions")
        blobs = [stream.read(MAX_BYTES + 1) for stream in streams]
        result = inspect_bytes(*blobs, declared_map_length, bsp_ordinal, tag_base)
        for path, stream, initial, blob in zip(paths, streams, before, blobs):
            after = os.fstat(stream.fileno())
            current = path.stat()
            # Descriptor ctime snapshots are comparable; Windows path stat and
            # descriptor fstat may use different ctime semantics.
            if (_identity(initial) != _identity(after) or initial.st_ctime_ns != after.st_ctime_ns or
                    _identity(after) != _identity(current) or len(blob) != initial.st_size):
                raise InspectionError("input changed while reading or inspecting")
    result["source_hashes"] = {name: hashlib.sha256((CHECKOUT / name).read_bytes()).hexdigest()
        for name in ("tools/wii/inspect_bsp_residency.py", "tools/wii/inspect_widget_graph.py",
                     "source/cache/cache_files.c", "source/scenario/scenario.h",
                     "source/scenario/scenario_definitions.h", "source/structures/structure_bsp_definitions.h",
                     "port/linux/game/tag_schema_scenario.c", "port/linux/game/cache_file_formats.c",
                     "port/linux/game/tag_validate.c")}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag_blob", type=Path)
    parser.add_argument("bsp_blob", type=Path)
    parser.add_argument("--declared-map-length", type=int, required=True)
    parser.add_argument("--bsp-ordinal", type=int, required=True)
    parser.add_argument("--tag-base", type=lambda value: int(value, 0), default=BASE)
    parser.add_argument("--output", type=Path, required=True, help="new external numeric JSON file")
    args = parser.parse_args(argv)
    try:
        output = external_path(args.output)
        if output.exists():
            raise InspectionError("output already exists")
        result = inspect_files(args.tag_blob, args.bsp_blob, args.declared_map_length,
                               args.bsp_ordinal, args.tag_base)
        write_exclusive(output, result)
    except InspectionError as error:
        print("BSP inspection failed: " + str(error), file=sys.stderr)
        return 1
    except OSError as error:
        print("BSP inspection file operation failed (errno %s)." % error.errno, file=sys.stderr)
        return 1
    print("BSP numeric inspection: %d bytes, %d descriptor Data spans checked." %
          (result["goldens"]["file_bytes"], sum(t["count"] for t in result["descriptor_tables"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
