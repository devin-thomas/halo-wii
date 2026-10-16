"""Aggregate probe of model vertex buffers as a staged map holds them (HWI-082).

Reads one staged map (HWIMAP02, either byte order: map_stage.py) and reports,
for every model tag's geometry parts, counts only (never bytes, names or
paths) of what the Wii renderer consumes from the tag slot:

- where the part's vertex_buffer.hardware_format leads (relocated or not),
  and what the descriptor's Data word holds (an Xbox tag address or a slot
  offset);
- the vertex payload's byte order (positions finite and bounded read either
  way);
- the node bytes: multiples of 3 (rasterizer_geometry.c's assert; the Xbox
  vertex shader addresses three constant registers a node) and their node
  (byte / 3) against the model's node count;
- the node weight's domain: NORMSHORT1 (s16 / 32767, the vertex declaration
  at vertex_shader_declarations[0x78 / 4]; compress_real_to_int16_clamp) or
  the conflicting (byte)w / 255 of rasterizer_geometry_uncompress_vertices;
- the index payload's byte order (every index below the part's vertices).

    python -B tools/wii/model_vertex_probe.py --map <private staged .wmap>
"""
import argparse
import json
import math
import struct
import sys

TAG_BASE = 0x803A6000
MODE = 0x6D6F6465
MODEL_NODES, MODEL_GEOMETRIES = 184, 208
GEOMETRY_PARTS, GEOMETRY_BYTES, PART_BYTES = 36, 48, 104
PART_TRIANGLE_BUFFER, PART_VERTEX_BUFFER = 68, 84


def probe(data):
    if data[:8] != b"HWIMAP02":
        raise ValueError("not an HWIMAP02 staging")
    order = ">" if struct.unpack_from(">I", data, 8)[0] == 0x01020304 else "<"

    def u32(buf, at):
        return struct.unpack_from(order + "I", buf, at)[0]

    def s32(buf, at):
        return struct.unpack_from(order + "i", buf, at)[0]

    def s16(buf, at):
        return struct.unpack_from(order + "h", buf, at)[0]

    tags = relocations = None
    for index in range(u32(data, 0x0C)):
        kind, _, size, _, file_offset, count, reloc_offset = struct.unpack_from(order + "7I", data, 0x40 + 32 * index)
        if kind == 1:
            tags = data[file_offset:file_offset + size]
            relocations = set(struct.unpack_from(order + "%dI" % count, data, reloc_offset))
    if tags is None:
        raise ValueError("no tag segment")

    def slot(value):
        """a pointer word as a slot offset: relocated (an offset) or an Xbox tag address"""
        return (value - TAG_BASE, "xbox_address") if value >= TAG_BASE else (value, "slot_offset")

    f = {"order": "big" if order == ">" else "little", "models": 0, "parts": 0, "vertices": 0,
         "hardware_format_relocated": 0, "hardware_format_not_relocated": 0, "descriptor_data": {},
         "descriptor_data_relocated": 0, "part_flags_local_nodes": 0, "part_flags_stripped": 0,
         "vertex_types": {}, "strip_types": {}, "positions_plausible_le": 0, "positions_plausible_be": 0,
         "node_bytes_not_multiple_of_3": 0, "node_past_model_nodes": 0, "nodes_equal": 0,
         "weight_min": None, "weight_max": None, "weight_32767": 0, "weight_0": 0, "weight_above_255": 0,
         "weight_negative": 0, "weight_32767_when_nodes_equal": 0, "weight_255_when_nodes_equal": 0,
         "weight_0_when_nodes_equal": 0, "max_model_nodes": 0, "parts_max_node_over_21": 0,
         "indices_valid_be": 0, "indices_valid_le": 0, "indices_parts": 0}
    instances, _ = slot(u32(tags, 0))
    for ordinal in range(s32(tags, 12)):
        instance = instances + 32 * ordinal
        if u32(tags, instance) != MODE:
            continue
        root, _ = slot(u32(tags, instance + 20))
        f["models"] += 1
        node_count = s32(tags, root + MODEL_NODES)
        f["max_model_nodes"] = max(f["max_model_nodes"], node_count)
        geometries, _ = slot(u32(tags, root + MODEL_GEOMETRIES + 4))
        for g in range(s32(tags, root + MODEL_GEOMETRIES)):
            geometry = geometries + GEOMETRY_BYTES * g
            parts, _ = slot(u32(tags, geometry + GEOMETRY_PARTS + 4))
            for p in range(s32(tags, geometry + GEOMETRY_PARTS)):
                part = parts + PART_BYTES * p
                f["parts"] += 1
                flags = u32(tags, part)
                f["part_flags_stripped"] += flags & 1
                f["part_flags_local_nodes"] += (flags >> 1) & 1
                vtype, vcount = s16(tags, part + PART_VERTEX_BUFFER), s32(tags, part + PART_VERTEX_BUFFER + 4)
                ttype, tcount = s16(tags, part + PART_TRIANGLE_BUFFER), s32(tags, part + PART_TRIANGLE_BUFFER + 4)
                f["vertex_types"][str(vtype)] = f["vertex_types"].get(str(vtype), 0) + 1
                f["strip_types"][str(ttype)] = f["strip_types"].get(str(ttype), 0) + 1
                field = part + PART_VERTEX_BUFFER + 16
                f["hardware_format_relocated" if field in relocations else "hardware_format_not_relocated"] += 1
                descriptor, _ = slot(u32(tags, field))
                payload, domain = slot(u32(tags, descriptor + 4))
                f["descriptor_data"][domain] = f["descriptor_data"].get(domain, 0) + 1
                f["descriptor_data_relocated"] += (descriptor + 4) in relocations
                if vtype != 5 or vcount <= 0 or payload + 32 * vcount > len(tags):
                    continue
                f["vertices"] += vcount
                part_max_node = 0
                for v in range(vcount):
                    at = payload + 32 * v
                    for name, fmt in (("le", "<3f"), ("be", ">3f")):
                        if all(math.isfinite(c) and abs(c) < 1.0e4 for c in struct.unpack_from(fmt, tags, at)):
                            f["positions_plausible_" + name] += 1
                    n0, n1 = tags[at + 28], tags[at + 29]
                    weight = struct.unpack_from("<h", tags, at + 30)[0]
                    for k, n in (("0", n0), ("1", n1)):
                        f.setdefault("node%s_mod3" % k, [0, 0, 0])[n % 3] += 1
                        f["node%s_raw_below_model_nodes" % k] = f.get("node%s_raw_below_model_nodes" % k, 0) + \
                            (n < node_count)
                        f["node%s_raw_max" % k] = max(f.get("node%s_raw_max" % k, 0), n)
                        f["node%s_div3_below_model_nodes" % k] = f.get("node%s_div3_below_model_nodes" % k, 0) + \
                            (n % 3 == 0 and n // 3 < node_count)
                        f["node%s_255" % k] = f.get("node%s_255" % k, 0) + (n == 255)
                    if weight == 32767:
                        f["full_weight_node1_255"] = f.get("full_weight_node1_255", 0) + (n1 == 255)
                        f["full_weight_node1_0"] = f.get("full_weight_node1_0", 0) + (n1 == 0)
                        f["full_weight_node1_below_model_nodes"] = \
                            f.get("full_weight_node1_below_model_nodes", 0) + (n1 < node_count)
                    else:
                        f["partial_weight_both_raw_below"] = f.get("partial_weight_both_raw_below", 0) + \
                            (n0 < node_count and n1 < node_count)
                        f["partial_weight_both_div3_below"] = f.get("partial_weight_both_div3_below", 0) + \
                            (n0 % 3 == 0 and n1 % 3 == 0 and n0 // 3 < node_count and n1 // 3 < node_count)
                        f["partial_weight_count"] = f.get("partial_weight_count", 0) + 1
                    f["node_bytes_not_multiple_of_3"] += bool(n0 % 3 or n1 % 3)
                    f["node_past_model_nodes"] += n0 // 3 >= node_count or n1 // 3 >= node_count
                    part_max_node = max(part_max_node, n0 // 3, n1 // 3)
                    f["weight_min"] = weight if f["weight_min"] is None else min(f["weight_min"], weight)
                    f["weight_max"] = weight if f["weight_max"] is None else max(f["weight_max"], weight)
                    f["weight_32767"] += weight == 32767
                    f["weight_0"] += weight == 0
                    f["weight_above_255"] += weight > 255
                    f["weight_negative"] += weight < 0
                    if n0 == n1:
                        f["nodes_equal"] += 1
                        f["weight_32767_when_nodes_equal"] += weight == 32767
                        f["weight_255_when_nodes_equal"] += weight == 255
                        f["weight_0_when_nodes_equal"] += weight == 0
                f["parts_max_node_over_21"] += part_max_node > 21
                index_count = tcount + 2 if ttype == 1 and tcount else 3 * tcount if ttype == 0 else 0
                tdescriptor, _ = slot(u32(tags, part + PART_TRIANGLE_BUFFER + 12))
                indices, _ = slot(u32(tags, tdescriptor + 4))
                if index_count and indices + 2 * index_count <= len(tags):
                    f["indices_parts"] += 1
                    for name, fmt in (("be", ">%dH"), ("le", "<%dH")):
                        f["indices_valid_" + name] += max(struct.unpack_from(fmt % index_count, tags, indices)) < vcount
    return f


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", required=True)
    args = parser.parse_args()
    with open(args.map, "rb") as handle:
        data = handle.read()
    try:
        facts = probe(data)
    except (ValueError, struct.error, IndexError) as failure:
        sys.exit("refused: %s" % failure)
    print(json.dumps(facts, indent=1, sort_keys=True))


if __name__ == "__main__":
    main()
