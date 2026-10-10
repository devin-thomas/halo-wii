"""Select one compressed-environment material section for the first geometry diagnostic.

Read-only: reuses the bounded material inspection, then validates every triangle
index and every vertex position the renderer will consume. Writes a private
numeric manifest (counts, extents, hashes); never exports indices, positions,
names, raw bytes or private paths.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

import inspect_bsp_residency as residency
import inspect_material_graph as graph
from inspect_widget_graph import InspectionError, external_path, write_exclusive

BASE = graph.BASE
ENVIRONMENT_SHADER = 0x73656E76
COMPRESSED_ENVIRONMENT = 1
RULE = "largest_environment_shader_compressed_material_by_surface_count_in_lightmap_0"
CHECKOUT = Path(__file__).resolve().parents[2]


def _materials(meta):
    """Yield (global index, lightmap ordinal, local ordinal, BSP offset)."""
    for lightmap in meta["lightmaps"]:
        for local in range(lightmap["material_count"]):
            yield (lightmap["first_material"] + local, lightmap["ordinal"], local,
                   lightmap["material_span"]["offset"] + local * 256)


def _material_fields(bsp, offset):
    group = struct.unpack_from("<I", bsp, offset)[0]
    first, count = struct.unpack_from("<ii", bsp, offset + 20)
    vertex_type, _, vertices = struct.unpack_from("<hHi", bsp, offset + 176)
    size, _, _, address, _ = struct.unpack_from("<iIiII", bsp, offset + 236)
    return group, first, count, vertex_type, vertices, size, address


def choose(meta, bsp):
    best = None
    for index, lightmap, _, offset in _materials(meta):
        group, _, surfaces, vertex_type, _, _, _ = _material_fields(bsp, offset)
        if lightmap == 0 and group == ENVIRONMENT_SHADER and vertex_type == COMPRESSED_ENVIRONMENT:
            if best is None or surfaces > best[1]:
                best = index, surfaces
    if best is None:
        raise InspectionError("no material satisfies the selection rule")
    return best[0]


def select_bytes(tags, bsp, declared_map_length, bsp_ordinal, material, tag_base=BASE):
    meta = graph.inspect_bytes(tags, bsp, declared_map_length, bsp_ordinal, tag_base)
    parent = residency.inspect_bytes(tags, bsp, declared_map_length, bsp_ordinal, tag_base)
    windows = tuple((w["base"], w["bytes"]) for w in parent["valid_windows"])
    rule_choice = choose(meta, bsp)
    if material == "auto":
        material = rule_choice
    found = [m for m in _materials(meta) if m[0] == material]
    if not found:
        raise InspectionError("selected material index outside the published graph")
    _, lightmap, local, offset = found[0]
    group, first, surfaces, vertex_type, vertices, size, address = _material_fields(bsp, offset)
    if vertex_type != COMPRESSED_ENVIRONMENT:
        raise InspectionError("selected material is not compressed environment geometry")
    if surfaces <= 0 or vertices <= 0:
        raise InspectionError("selected material has no triangles or vertices")
    if vertices * 32 > size:
        raise InspectionError("compressed environment records exceed tag data")
    data = residency._region(windows, 1, address, size, 1)
    root = parent["BSP_root"]["offset"]
    surface_count, surface_address = struct.unpack_from("<iI", bsp, root + 248)
    table = residency._region(windows, 1, surface_address, surface_count, 6)
    if first < 0 or first > surface_count - surfaces:
        raise InspectionError("selected surface range outside the root surface table")

    index_hash = hashlib.sha256()
    referenced = set()
    degenerate = 0
    for ordinal in range(surfaces):
        corners = struct.unpack_from("<3H", bsp, table["offset"] + (first + ordinal) * 6)
        if max(corners) >= vertices:
            raise InspectionError("triangle index outside the material's environment vertices")
        degenerate += len(set(corners)) < 3
        referenced.update(corners)
        index_hash.update(struct.pack("<3H", *corners))

    position_hash = hashlib.sha256()
    lower, upper = [float("inf")] * 3, [float("-inf")] * 3
    subnormal = negative_zero = 0
    for vertex in range(vertices):
        bits = struct.unpack_from("<3I", bsp, data["offset"] + vertex * 32)
        if any(b & 0x7F800000 == 0x7F800000 for b in bits):
            raise InspectionError("non-finite position bits")
        subnormal += sum(b & 0x7F800000 == 0 and b & 0x007FFFFF != 0 for b in bits)
        negative_zero += sum(b == 0x80000000 for b in bits)
        position_hash.update(struct.pack("<3I", *bits))
        if vertex in referenced:
            for axis, value in enumerate(struct.unpack("<3f", struct.pack("<3I", *bits))):
                lower[axis] = min(lower[axis], value)
                upper[axis] = max(upper[axis], value)

    section = hashlib.sha256(struct.pack("<5I", material, first, surfaces, vertices, size))
    section.update(bytes.fromhex(index_hash.hexdigest()))
    section.update(bytes.fromhex(position_hash.hexdigest()))
    return {
        "format_version": 1,
        "scope": "selected_geometry_section_numeric_manifest_private",
        "inputs": {"tag_input_sha256": meta["tag_input_sha256"], "bsp_input_sha256": meta["bsp_input_sha256"],
                   "tag_input_bytes": len(tags), "bsp_input_bytes": len(bsp),
                   "declared_map_bytes": declared_map_length, "bsp_ordinal": bsp_ordinal, "tag_base": tag_base},
        "rule": RULE, "rule_choice": rule_choice, "selected_matches_rule": material == rule_choice,
        "selected": {"material_index": material, "lightmap": lightmap, "local_material": local,
                     "shader_group_word": group, "first_surface": first, "surface_count": surfaces,
                     "environment_vertices": vertices, "compressed_data_bytes": size},
        "validation": {"triangle_indices_in_bounds": True, "positions_finite": True,
                       "degenerate_triangles": degenerate, "referenced_vertices": len(referenced),
                       "unreferenced_vertices": vertices - len(referenced),
                       "subnormal_position_words": subnormal, "negative_zero_position_words": negative_zero},
        "extents": {"min": lower, "max": upper, "scope": "referenced vertices, binary32 decoded on host"},
        "consumed_bytes": {"source_index_bytes": surfaces * 6, "source_position_records": vertices * 32,
                           "native_positions_f32": vertices * 12, "native_indices_u16": surfaces * 6},
        "hashes": {"indices_sha256": index_hash.hexdigest(), "positions_sha256": position_hash.hexdigest(),
                   "section_sha256": section.hexdigest()},
        "consumer": "cache_material_get_surface_positions(material_index, local 0..surface_count-1)",
        "material_inspection_numeric_sha256": meta["numeric_projection_sha256"],
    }


def select_files(tag_path, bsp_path, declared_map_length, bsp_ordinal, material, tag_base=BASE):
    paths = [external_path(tag_path), external_path(bsp_path)]
    with ExitStack() as stack:
        streams = [stack.enter_context(path.open("rb")) for path in paths]
        before = [os.fstat(stream.fileno()) for stream in streams]
        blobs = [stream.read(graph.MAX_BYTES + 1) for stream in streams]
        if any(len(blob) > graph.MAX_BYTES for blob in blobs):
            raise InspectionError("input size outside bounded regions")
        result = select_bytes(*blobs, declared_map_length, bsp_ordinal, material, tag_base)
        for stream, initial, blob in zip(streams, before, blobs):
            after = os.fstat(stream.fileno())
            if residency._identity(initial) != residency._identity(after) or len(blob) != initial.st_size:
                raise InspectionError("input changed while reading or inspecting")
    result["source_hashes"] = {name: hashlib.sha256((CHECKOUT / name).read_bytes()).hexdigest()
                               for name in ("tools/wii/select_geometry_section.py", "tools/wii/inspect_material_graph.py",
                                            "tools/wii/inspect_bsp_residency.py")}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag_blob", type=Path)
    parser.add_argument("bsp_blob", type=Path)
    parser.add_argument("--declared-map-length", type=int, required=True)
    parser.add_argument("--bsp-ordinal", type=int, required=True)
    parser.add_argument("--material", default="auto", help="global material index, or auto for the selection rule")
    parser.add_argument("--tag-base", type=lambda value: int(value, 0), default=BASE)
    parser.add_argument("--output", type=Path, required=True, help="new external manifest JSON file")
    args = parser.parse_args(argv)
    material = args.material if args.material == "auto" else int(args.material)
    try:
        output = external_path(args.output)
        if output.exists():
            raise InspectionError("output already exists")
        result = select_files(args.tag_blob, args.bsp_blob, args.declared_map_length, args.bsp_ordinal,
                              material, args.tag_base)
        write_exclusive(output, result)
    except InspectionError as error:
        print("Geometry section selection failed: " + str(error), file=sys.stderr)
        return 1
    except OSError as error:
        print("Geometry section file operation failed (errno %s)." % error.errno, file=sys.stderr)
        return 1
    selected = result["selected"]
    print("Selected material %d: %d triangles, %d vertices, section %s." % (
        selected["material_index"], selected["surface_count"], selected["environment_vertices"],
        result["hashes"]["section_sha256"][:16]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
