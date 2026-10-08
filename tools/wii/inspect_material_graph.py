"""Inspect partial Xbox root/lightmap/material metadata, never exporting assets.

Every selected material is checked. Compact summaries and hashes preserve numeric
metadata identity without exporting names, geometry, raw bytes or private paths.
Encoded words remain unchanged; serialization goldens describe opaque raw spans.
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

import inspect_bsp_residency as residency
from inspect_widget_graph import InspectionError, Reader, external_path, write_exclusive

BASE = residency.BASE
MAX_BYTES = residency.MAX_BYTES
MAX_OUTPUT_BYTES = 1024 * 1024
TAG_DATA_LIMITS = (64000 * (56 + 20), 64000 * (32 + 8))
SHADER_GROUP = 0x73686472
NONE = 0xFFFFFFFF
CHECKOUT = Path(__file__).resolve().parents[2]


class TagReader(Reader):
    """Reuse bounded instance/name checks after the BSP index proof succeeds."""
    def __init__(self, tags, metadata):
        self.blob = tags
        self.work = self.scan_bytes = 0
        self.names = {}
        self.header = {"tag_count": metadata["tag_header_words"][3]}
        table = metadata["instance_table"]
        self.table = table["offset"], table["bytes"]
        self.windows = tuple((w["base"], w["bytes"]) for w in metadata["valid_windows"])

    def resolve(self, address, count, stride):
        span = residency._region(self.windows, 0, address, count, stride)
        return span["offset"], span["bytes"]


def _bounded_output(result):
    if len((json.dumps(result, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")) > MAX_OUTPUT_BYTES:
        raise InspectionError("numeric output size limit")
    return result


def inspect_bytes(tags, bsp, declared_map_length, bsp_ordinal, tag_base=BASE):
    parent = residency.inspect_bytes(tags, bsp, declared_map_length, bsp_ordinal, tag_base)
    windows = tuple((w["base"], w["bytes"]) for w in parent["valid_windows"])
    reader = TagReader(tags, parent)
    root = parent["BSP_root"]
    root_offset = root["offset"]
    claims = []
    for span in [{"offset": 0, "bytes": 24}, root] + [t["span"] for t in parent["descriptor_tables"]]:
        residency._claim(claims, span)

    def block(offset, maximum, stride, claimed=True):
        count, address, definition = struct.unpack_from("<iII", bsp, offset)
        if not 0 <= count <= maximum:
            raise InspectionError("root or material block signed count/source limit")
        span = residency._region(windows, 1, address, count, stride)
        if claimed:
            residency._claim(claims, span)
        return count, address, definition, span

    surfaces, _, _, _ = block(root_offset + 248, 131072, 6, False)
    lm_count, lm_address, lm_definition, lm_span = block(root_offset + 260, 128, 32)
    material_tables = []
    lightmaps = []
    total_materials = 0
    # Claim all tables before inspecting any contents, so overlap never depends
    # on material traversal order or on a later shader/resource failure.
    for ordinal in range(lm_count):
        offset = lm_span["offset"] + ordinal * 32
        count, address, definition, span = block(offset + 20, 2048, 256)
        total_materials += count
        bitmap, pad = struct.unpack_from("<hH", bsp, offset)
        lightmaps.append({"ordinal": ordinal, "bitmap_index": bitmap, "pad_word": pad,
                          "first_material": total_materials - count, "material_count": count,
                          "material_address_word": address, "definition_word": definition,
                          "source_offset": offset, "material_span": span})
        material_tables.append(span)

    numeric = hashlib.sha256()
    numeric.update(struct.pack("<6I", root_offset, 648, lm_span["offset"], lm_span["bytes"],
                               total_materials, lm_count))
    raw_sha = hashlib.sha256()
    crc = serialized = 0
    for span in [root, lm_span] + material_tables:
        raw = memoryview(bsp)[span["offset"]:span["offset"] + span["bytes"]]
        raw_sha.update(raw)
        crc = zlib.crc32(raw, crc)
        serialized += len(raw)
    if serialized > residency.UINT32_MAX:
        raise InspectionError("serialized size overflow")

    totals = {"environment_vertices": 0, "lightmap_vertices": 0, "compressed_data_bytes": 0,
              "uncompressed_data_bytes": 0, "shader_NONE": 0, "shader_identity_checked": 0,
              "hardware_null": 0, "hardware_identity_checked": 0, "zero_count_payloads": 0,
              "environment_payload_bytes": 0, "lightmap_payload_bytes": 0,
              "centroid_nonzero_bit_words": 0, "surface_count": 0}
    types = [0, 0, 0, 0]
    centroid_hash = hashlib.sha256()
    for lm, table in zip(lightmaps, material_tables):
        numeric.update(struct.pack("<hH4I", lm["bitmap_index"], lm["pad_word"], lm["first_material"],
                                   lm["material_count"], lm["source_offset"], 32))
        for ordinal in range(lm["material_count"]):
            offset = table["offset"] + ordinal * 256
            shader = struct.unpack_from("<4I", bsp, offset)
            if shader[3] == NONE:
                totals["shader_NONE"] += 1
            else:
                target = reader.instance(shader[3], shader[0])
                if SHADER_GROUP not in [target["group"]] + target["parent_groups"]:
                    raise InspectionError("reference target is not shader primary/ancestor")
                reader.name(shader[1])
                reader.name(target["name_address"])
                totals["shader_identity_checked"] += 1
            permutation, flags, first_surface, surface_count = struct.unpack_from("<hHii", bsp, offset + 16)
            if first_surface < 0 or surface_count < 0 or first_surface > surfaces - surface_count:
                raise InspectionError("material surface range")
            totals["surface_count"] += surface_count
            centroid = struct.unpack_from("<3I", bsp, offset + 28)
            centroid_hash.update(struct.pack("<3I", *centroid))
            totals["centroid_nonzero_bit_words"] += sum(v != 0 for v in centroid)
            buffers = []
            for kind, relative in enumerate((176, 196)):
                fields = struct.unpack_from("<hHiiII", bsp, offset + relative)
                vertex_type, pad, count, vertex_offset, base_address, hardware = fields
                if vertex_type not in ((0, 1) if kind == 0 else (2, 3)) or not 0 <= count <= 64000:
                    raise InspectionError("vertex type or signed count/source limit")
                types[vertex_type] += 1
                totals[("environment_vertices", "lightmap_vertices")[kind]] += count
                if hardware == 0:
                    totals["hardware_null"] += 1
                else:
                    descriptor = parent["descriptor_tables"][kind]["span"]
                    relative_descriptor = hardware - windows[1][0] - descriptor["offset"]
                    if relative_descriptor < 0 or relative_descriptor % 12 or relative_descriptor > descriptor["bytes"] - 12:
                        raise InspectionError("hardware descriptor table identity/alignment")
                    data_address = struct.unpack_from("<I", bsp, descriptor["offset"] + relative_descriptor + 4)[0]
                    stride = (56, 32, 20, 8)[vertex_type]
                    residency._region(windows, 1, data_address, count, stride)
                    totals["hardware_identity_checked"] += 1
                    totals[("environment_payload_bytes", "lightmap_payload_bytes")[kind]] += count * stride
                if count == 0:
                    totals["zero_count_payloads"] += 1
                buffers.append(fields)
            data_fields = []
            for kind, relative in enumerate((216, 236)):
                fields = struct.unpack_from("<iIiII", bsp, offset + relative)
                size, pad, file_offset, address, definition = fields
                if not 0 <= size <= TAG_DATA_LIMITS[kind]:
                    raise InspectionError("tag data signed size/source limit")
                residency._region(windows, 1, address, size, 1)
                totals[("uncompressed_data_bytes", "compressed_data_bytes")[kind]] += size
                data_fields.append(fields)
            if buffers[0][2] * 32 + buffers[1][2] * 8 > data_fields[1][0]:
                raise InspectionError("Xbox compressed count minimum exceeds data size")
            if any(value > residency.UINT32_MAX for value in totals.values()):
                raise InspectionError("aggregate numeric total overflow")
            numeric.update(struct.pack("<4IhHii3I", *shader, permutation, flags,
                                       first_surface, surface_count, *centroid))
            for fields in buffers:
                numeric.update(struct.pack("<hHiiII", *fields))
            for fields in data_fields:
                numeric.update(struct.pack("<iIiII", *fields))
            numeric.update(struct.pack("<2I", offset, 256))
    goldens = {"scenario_datum": parent["goldens"]["scenario_datum"],
               "bsp_datum": parent["goldens"]["datum"], "lightmap_count": lm_count,
               "material_count": total_materials, "environment_vertices": totals["environment_vertices"],
               "lightmap_vertices": totals["lightmap_vertices"], "serialized_bytes": serialized,
               "serialized_crc32": crc, "serialized_sha256": raw_sha.hexdigest()}
    return _bounded_output({"format_version": 1, "scope": "partial root lightmap material numeric metadata only",
        "tag_input_sha256": parent["tag_input_sha256"], "bsp_input_sha256": parent["bsp_input_sha256"],
        "tag_input_bytes": len(tags), "bsp_input_bytes": len(bsp), "declared_map_bytes": declared_map_length,
        "bsp_ordinal": bsp_ordinal, "tag_base": tag_base, "goldens": goldens,
        "root": {"span": root, "surface_count": surfaces, "lightmap_count": lm_count,
                 "lightmap_address_word": lm_address, "lightmap_definition_word": lm_definition},
        "lightmaps": lightmaps, "totals": totals, "vertex_type_counts": types,
        "numeric_projection_sha256": numeric.hexdigest(), "centroid_bits_sha256": centroid_hash.hexdigest(),
        "name_scan": {"distinct_bytes": reader.scan_bytes, "distinct_names": len(reader.names)},
        "limits": {"input_bytes_each": MAX_BYTES, "output_bytes": MAX_OUTPUT_BYTES,
                   "lightmaps": 128, "materials_per_lightmap": 2048, "vertices_per_material": 64000,
                   "uncompressed_tag_data_bytes": TAG_DATA_LIMITS[0],
                   "compressed_tag_data_bytes": TAG_DATA_LIMITS[1],
                   "aggregate_numeric_total": residency.UINT32_MAX},
        "limitations": ["partial_named_fields_no_full_native_ABI_layout_proof",
            "shader_names_bounded_not_exported_or_required_equal_name_length_word_unused",
            "NULL_hardware_has_no_resource_payload_extent_proof_zero_count_payload_not_followed",
            "Xbox_compressed_minimum32N_plus8M_distinct_from_type_indexed_resource56_32_20_8",
            "surface_ranges_signed_in_root_extent_only_no_ordering_or_disjointness_requirement",
            "float_bits_preserved_no_arithmetic_geometry_triangle_payload_contents_not_validated",
            "opaque_raw_concat_not_target_serialization_relocation_standalone_cache_or_save",
            "Python_stat_guards_not_locks_C_generation_lifetime_engine_GX_gameplay_hardware_unqualified"]})


def inspect_files(tag_path, bsp_path, declared_map_length, bsp_ordinal, tag_base=BASE):
    paths = [external_path(tag_path), external_path(bsp_path)]
    with ExitStack() as stack:
        streams = [stack.enter_context(path.open("rb")) for path in paths]
        before = [os.fstat(stream.fileno()) for stream in streams]
        path_before = [path.stat() for path in paths]
        if not 36 <= before[0].st_size <= MAX_BYTES or not 24 <= before[1].st_size <= MAX_BYTES:
            raise InspectionError("input size outside bounded regions")
        blobs = [stream.read(MAX_BYTES + 1) for stream in streams]
        result = inspect_bytes(*blobs, declared_map_length, bsp_ordinal, tag_base)
        for path, stream, initial, initial_path, blob in zip(paths, streams, before, path_before, blobs):
            after, current = os.fstat(stream.fileno()), path.stat()
            if (residency._identity(initial) != residency._identity(after) or
                    initial.st_ctime_ns != after.st_ctime_ns or
                    residency._identity(initial_path) != residency._identity(current) or
                    initial_path.st_ctime_ns != current.st_ctime_ns or
                    residency._identity(after) != residency._identity(current) or len(blob) != initial.st_size):
                raise InspectionError("input changed while reading or inspecting")
    result["source_hashes"] = {name: hashlib.sha256((CHECKOUT / name).read_bytes()).hexdigest()
        for name in ("tools/wii/inspect_material_graph.py", "tools/wii/inspect_bsp_residency.py",
                     "tools/wii/inspect_widget_graph.py", "source/structures/structure_bsp_definitions.h",
                     "source/structures/structures.h", "source/rasterizer/rasterizer_geometry.h",
                     "source/tag_files/tag_groups.h", "port/linux/game/cache_file_formats.c",
                     "port/linux/game/tag_schema_collision.c", "port/linux/game/tag_validate.c")}
    return _bounded_output(result)


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
        result = inspect_files(args.tag_blob, args.bsp_blob, args.declared_map_length, args.bsp_ordinal, args.tag_base)
        write_exclusive(output, result)
    except InspectionError as error:
        print("Material inspection failed: " + str(error), file=sys.stderr)
        return 1
    except OSError as error:
        print("Material inspection file operation failed (errno %s)." % error.errno, file=sys.stderr)
        return 1
    print("Material numeric inspection: %d lightmaps, %d materials, %d serialized bytes." %
          tuple(result["goldens"][k] for k in ("lightmap_count", "material_count", "serialized_bytes")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
