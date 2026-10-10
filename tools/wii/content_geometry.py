"""Model render geometry, structure-BSP lightmap geometry and collision BSPs (HWI-008C).

Three sectioned big-endian containers (be_records.py), each converted with
explicit little-endian field decoding and checked by rebuilding the exact
source bytes:

HWM1 model geometry (one per model tag). Sections:
  1 PARTS     56 bytes: geometry u16, part u16, flags u32, shader s16,
              previous part s8, next part s8, centroid primary node s16,
              centroid secondary node s16, primary weight f32, secondary
              weight f32, centroid 3 x f32, vertex type s16 (5 = model
              compressed), strip type s16 (1 = precompiled strip, 0 =
              triangle list), vertex count u32, first vertex u32, index
              count u32, first index u32
  2 VERTICES  32 bytes, the Xbox compressed model vertex: position 3 x f32,
              normal, binormal and tangent as packed u32 (11:11:10, decoded
              by rasterizer_geometry.c uncompress_int32_to_real_vector3d),
              texcoord 2 x s16, node indices 2 x u8 (x3, as stored), node
              weight s16
  3 INDICES   u16 vertex indices, part by part

HWL1 structure-BSP lightmap geometry (one per scenario structure BSP). Sections:
  1 BSP              16 bytes: lightmap bitmap tag datum u32, lightmaps u32,
                     materials u32, surfaces u32
  2 LIGHTMAPS        12 bytes: bitmap index s16, pad u16, first material
                     u32, material count u32
  3 MATERIALS        188 bytes: shader group u32, shader tag datum u32,
                     permutation s16, flags u16, first surface s32, surface
                     count s32, centroid 3 x f32, render lighting (ambient
                     3 x f32, distant light count s16, pad u16, two distant
                     lights of colour 3 x f32 and direction 3 x f32, point
                     light count s16, pad u16, two point light indices s32,
                     reflection tint 4 x f32, shadow vector 3 x f32, shadow
                     colour 3 x f32), plane 4 x f32, breakable surface s16,
                     unused u16, vertex type s16 (1), lightmap vertex type
                     s16 (3), vertex count u32, first vertex u32, lightmap
                     vertex count u32, first lightmap vertex u32
  4 VERTICES         32 bytes, the compressed environment vertex: position
                     3 x f32, normal, binormal, tangent packed u32, texcoord
                     2 x f32
  5 LIGHTMAP_VERTICES 8 bytes: incident radiosity packed u32, lightmap u
                     and v s16
  6 SURFACES         6 bytes: three u16 vertex indices (local to the
                     surface's material)

HWC1 collision BSPs (one per collision model tag, one per structure BSP).
Sections:
  1 BSPS              68 bytes: node s16 (-1 for a structure BSP), bsp s16,
                      then (first u32, count u32) for each of the eight
                      arrays below, in this order
  2 BSP3D_NODES       12 bytes: plane s32, back child s32, front child s32
  3 PLANES            16 bytes: i, j, k, d f32
  4 LEAVES             8 bytes: flags u16, bsp2d reference count s16,
                      first bsp2d reference s32
  5 BSP2D_REFERENCES   8 bytes: plane s32, bsp2d node s32
  6 BSP2D_NODES       20 bytes: plane i, j, d f32, left child s32, right
                      child s32
  7 SURFACES          12 bytes: plane s32, first edge s32, flags u8,
                      breakable surface s8, material s16
  8 EDGES             24 bytes: start and end vertex s32, forward and
                      reverse edge s32, left and right surface s32
  9 VERTICES          16 bytes: point 3 x f32, first edge s32

Layouts follow source/models, source/structures and
source/physics/collision_bsp_definitions.c; tag offsets are the upstream
validator schema's (tools/wii/cache_schema_tables.c, checked by the tests).
"""
import hashlib
import struct

import be_records as br
import halo_cache as hc

# ---- formats -------------------------------------------------------------------
PART_FMT = "HHIhbbhhIIIIIhhIIII"
MODEL_VERTEX_FMT = "IIIIIIhhBBh"
INDEX_FMT = "H"
HWM_SPEC = {1: br.record_size(PART_FMT), 2: br.record_size(MODEL_VERTEX_FMT), 3: 2}

LIGHTING_FMT = "IIIhH" + "I" * 12 + "hHii" + "I" * 10
BSP_FMT = "IIII"
LIGHTMAP_FMT = "hHII"
MATERIAL_FMT = "IIhHii" + "III" + LIGHTING_FMT + "IIII" + "hH" + "hh" + "IIII"
ENV_VERTEX_FMT = "IIIIIIII"
LIGHTMAP_VERTEX_FMT = "Ihh"
SURFACE_FMT = "HHH"
HWL_SPEC = {1: br.record_size(BSP_FMT), 2: br.record_size(LIGHTMAP_FMT), 3: br.record_size(MATERIAL_FMT),
            4: 32, 5: 8, 6: 6}

COLLISION_ARRAYS = (  # (field offset in collision_bsp, record format, maximum count) in section order
    (0, "iii", 131072), (12, "IIII", 65536), (24, "Hhi", 65536), (36, "ii", 131072),
    (48, "IIIii", 65535), (60, "iiBbh", 131072), (72, "iiiiii", 262144), (84, "IIIi", 131072))
BSPS_FMT = "hh" + "II" * 8
HWC_SPEC = {1: br.record_size(BSPS_FMT),
            **{i + 2: br.record_size(fmt) for i, (_, fmt, _) in enumerate(COLLISION_ARRAYS)}}

# ---- tag layout (upstream validator schema offsets) ------------------------------
MODEL_ROOT_BYTES, MODEL_GEOMETRIES, GEOMETRY_BYTES, GEOMETRY_PARTS = 232, 208, 48, 36
PART_BYTES, PART_OWN_BLOCKS = 104, (32, 44, 56)
PART_TRIANGLE_BUFFER, PART_VERTEX_BUFFER = 68, 84
MAX_GEOMETRIES, MAX_PARTS, MAX_PART_VERTICES = 256, 32, 65535
MODEL_COMPRESSED_VERTEX_TYPE, STRIP, TRIANGLES = 5, 1, 0
DESCRIPTOR_BYTES = 12
SCENARIO_ROOT_BYTES, SCENARIO_BSPS, BSP_REFERENCE_BYTES, MAX_BSPS = 0x5B0, 0x5A4, 32, 16
BSP_HEADER_BYTES, BSP_ROOT_BYTES = 24, 648
BSP_COLLISION, BSP_SURFACES, BSP_LIGHTMAPS = 176, 248, 260
LIGHTMAP_BYTES, LIGHTMAP_MATERIALS, MATERIAL_BYTES = 32, 20, 256
MAX_LIGHTMAPS, MAX_MATERIALS, MAX_SURFACES = 128, 2048, 131072
ENVIRONMENT_COMPRESSED, LIGHTMAP_COMPRESSED = 1, 3
COLLISION_MODEL_ROOT_BYTES, COLLISION_MODEL_NODES, COLLISION_NODE_BYTES, COLLISION_NODE_BSPS = 664, 652, 64, 52
COLLISION_BSP_BYTES, MAX_COLLISION_NODES, MAX_NODE_BSPS = 96, 64, 32
GROUP = {name: struct.unpack(">I", name.encode("latin-1"))[0] for name in ("mode", "coll", "sbsp", "bitm")}


class GeometryError(ValueError):
    """A bounded validation failure; the message names no tag or input bytes."""


def _sha(*parts):
    digest = hashlib.sha256()
    for part in parts:
        digest.update(bytes(part))
    return digest.hexdigest()


# ---- address spaces ----------------------------------------------------------------
class BspSpace:
    """A loaded structure BSP: its bytes at an Xbox base address, checked like tag space."""

    def __init__(self, data, base):
        self.data, self.base = memoryview(data), base

    def offset(self, address, length):
        if length < 0 or address < self.base or address - self.base > len(self.data) or \
                length > len(self.data) - (address - self.base):
            raise GeometryError("BSP span outside the loaded BSP")
        return address - self.base

    def block(self, field, stride, maximum):
        count, address = struct.unpack_from("<iI", self.data, field)
        if count < 0 or count > maximum:
            raise GeometryError("BSP block count outside bounds")
        return [] if count == 0 else [self.offset(address, count * stride) + i * stride for i in range(count)]

    def bytes_at(self, offset, length):
        return self.data[offset:offset + length]


class TagSpace:
    """Tag space of a CacheMap with the same interface as BspSpace."""

    def __init__(self, cache):
        self.cache, self.data = cache, cache.tags

    def offset(self, address, length):
        try:
            return self.cache.offset(address, length)
        except hc.CacheError:
            raise GeometryError("tag span outside tag data") from None

    def block(self, field, stride, maximum):
        try:
            return self.cache.block(field, stride, maximum)
        except hc.CacheError:
            raise GeometryError("tag block count or span outside bounds") from None

    def bytes_at(self, offset, length):
        return self.data[offset:offset + length]


def _unpack(space, fmt, offset):
    return struct.unpack_from("<" + fmt, space.data, offset)


# ---- models ------------------------------------------------------------------------
def _descriptor_data(cache, table_address, table_count, descriptor):
    if descriptor == 0 or table_count <= 0:
        raise GeometryError("model part has no vertex or index buffer")
    index, rest = divmod(descriptor - table_address, DESCRIPTOR_BYTES)
    if descriptor < table_address or rest or index >= table_count:
        raise GeometryError("model part buffer is not one of the map's descriptors")
    return cache.u32(cache.offset(table_address, table_count * DESCRIPTOR_BYTES) + index * DESCRIPTOR_BYTES + 4)


def convert_model(cache, instance):
    """HWM1 bytes and facts for one model tag."""
    space = TagSpace(cache)
    root = cache.root_offset(instance, MODEL_ROOT_BYTES)
    if root is None:
        raise GeometryError("model root outside tag data")
    _, _, _, _, vcount_table, vtable, icount_table, itable, _ = struct.unpack_from("<9I", cache.tags, 0)
    parts, vertices, indices, source = [], [], [], []
    first_vertex = first_index = 0
    for g, geometry in enumerate(space.block(root + MODEL_GEOMETRIES, GEOMETRY_BYTES, MAX_GEOMETRIES)):
        for p, part in enumerate(space.block(geometry + GEOMETRY_PARTS, PART_BYTES, MAX_PARTS)):
            if any(cache.s32(part + field) for field in PART_OWN_BLOCKS):
                raise GeometryError("cache model part keeps vertex or triangle blocks of its own")
            flags, shader, previous, following, cpn, csn, w1, w2, cx, cy, cz = _unpack(space, "IhbbhhIIIII", part)
            strip_type, _, triangle_count, _, index_descriptor = _unpack(space, "hHiII", part + PART_TRIANGLE_BUFFER)
            vertex_type, _, vertex_count, _, _, vertex_descriptor = _unpack(space, "hHiiII", part + PART_VERTEX_BUFFER)
            if vertex_type != MODEL_COMPRESSED_VERTEX_TYPE:
                raise GeometryError("model part vertex type not handled by this profile")
            if strip_type not in (STRIP, TRIANGLES):
                raise GeometryError("model part index buffer type unknown")
            if not 0 <= vertex_count <= MAX_PART_VERTICES or not 0 <= triangle_count <= MAX_PART_VERTICES:
                raise GeometryError("model part vertex or triangle count outside bounds")
            index_count = (triangle_count + 2 if triangle_count else 0) if strip_type == STRIP else 3 * triangle_count
            vdata = _descriptor_data(cache, vtable, vcount_table, vertex_descriptor)
            idata = _descriptor_data(cache, itable, icount_table, index_descriptor)
            vsize = vertex_count * br.record_size(MODEL_VERTEX_FMT)
            vbytes = space.bytes_at(space.offset(vdata, vsize), vsize)
            ibytes = space.bytes_at(space.offset(idata, 2 * index_count), 2 * index_count)
            values = br.decode_records(ibytes, INDEX_FMT, index_count)
            if values and max(values) >= vertex_count:
                raise GeometryError("model part index past its vertices")
            parts.append((g, p, flags, shader, previous, following, cpn, csn, w1, w2, cx, cy, cz, vertex_type,
                          strip_type, vertex_count, first_vertex, index_count, first_index))
            vertices.append(br.to_big_endian(vbytes, MODEL_VERTEX_FMT, vertex_count))
            indices.append(br.to_big_endian(ibytes, INDEX_FMT, index_count))
            source += [space.bytes_at(part, PART_BYTES), vbytes, ibytes]
            first_vertex += vertex_count
            first_index += index_count
    part_bytes = br.encode_records([v for row in parts for v in row], PART_FMT, len(parts))
    blob = br.pack_container(b"HWM1", [(1, HWM_SPEC[1], len(parts), part_bytes),
                                       (2, HWM_SPEC[2], first_vertex, b"".join(vertices)),
                                       (3, HWM_SPEC[3], first_index, b"".join(indices))])
    facts = {"geometries": len({row[0] for row in parts}), "parts": len(parts), "vertices": first_vertex,
             "indices": first_index, "source_sha256": _sha(*source)}
    return blob, facts


def unpack_model(blob):
    """Explicit decoder: (parts as tuples, vertex count, index count)."""
    sections = br.unpack_container(blob, b"HWM1", HWM_SPEC)
    parts = br.decode_records(sections[1][1], PART_FMT, sections[1][0], ">")
    width = len(struct.unpack("<" + PART_FMT, bytes(HWM_SPEC[1])))
    rows = [parts[i:i + width] for i in range(0, len(parts), width)]
    vertex_total, index_total = sections[2][0], sections[3][0]
    v_cursor = i_cursor = 0
    for row in rows:
        vertex_count, first_vertex, index_count, first_index = row[15:19]
        if first_vertex != v_cursor or first_index != i_cursor:
            raise GeometryError("model part ranges out of order")
        v_cursor += vertex_count
        i_cursor += index_count
    if (v_cursor, i_cursor) != (vertex_total, index_total):
        raise GeometryError("model part ranges do not cover the arrays")
    indices = br.decode_records(sections[3][1], INDEX_FMT, index_total, ">")
    for row in rows:
        vertex_count, _, index_count, first_index = row[15:19]
        if index_count and max(indices[first_index:first_index + index_count]) >= vertex_count:
            raise GeometryError("model index past its part's vertices")
    return rows, vertex_total, index_total


# ---- structure BSPs ------------------------------------------------------------------
def scenario_bsps(cache):
    """(bsp tag ordinal, BspSpace, root offset) for each scenario structure BSP."""
    scenario = cache.instances[cache.scenario_ordinal]
    root = cache.root_offset(scenario, SCENARIO_ROOT_BYTES)
    if root is None:
        raise GeometryError("scenario root outside tag data")
    result = []
    for reference in TagSpace(cache).block(root + SCENARIO_BSPS, BSP_REFERENCE_BYTES, MAX_BSPS):
        file_offset, size, base = struct.unpack_from("<iiI", cache.tags, reference)
        group, datum = cache.u32(reference + 16), cache.u32(reference + 28)
        ordinal = datum & 0xFFFF
        if group != GROUP["sbsp"] or ordinal >= len(cache.instances) or cache.instances[ordinal]["datum"] != datum \
                or cache.instances[ordinal]["group"] != GROUP["sbsp"]:
            raise GeometryError("scenario BSP reference does not name a structure BSP")
        if size < BSP_HEADER_BYTES:
            raise GeometryError("structure BSP smaller than its header")
        try:
            data = cache.file_span(file_offset, size)
        except hc.CacheError:
            raise GeometryError("structure BSP outside the map") from None
        header = struct.unpack_from("<6I", data, 0)
        if header[5] != GROUP["sbsp"]:
            raise GeometryError("structure BSP header signature")
        space = BspSpace(data, base)
        result.append((ordinal, space, space.offset(header[0], BSP_ROOT_BYTES)))
    return result


def convert_lightmaps(cache, space, root):
    """HWL1 bytes and facts for one structure BSP."""
    lightmap_group, lightmap_datum = struct.unpack_from("<I8xI", space.data, root)
    surfaces = space.block(root + BSP_SURFACES, 6, MAX_SURFACES)
    surface_count = len(surfaces)
    surface_bytes = space.bytes_at(surfaces[0], 6 * surface_count) if surfaces else b""
    surface_values = br.decode_records(surface_bytes, SURFACE_FMT, surface_count)
    lightmaps, materials, vertices, lightmap_vertices, source = [], [], [], [], [space.bytes_at(root, BSP_ROOT_BYTES)]
    first_material = first_vertex = first_lightmap_vertex = 0
    for lightmap in space.block(root + BSP_LIGHTMAPS, LIGHTMAP_BYTES, MAX_LIGHTMAPS):
        bitmap, pad = _unpack(space, "hH", lightmap)
        elements = space.block(lightmap + LIGHTMAP_MATERIALS, MATERIAL_BYTES, MAX_MATERIALS)
        lightmaps.append((bitmap, pad, first_material, len(elements)))
        for material in elements:
            group, datum = _unpack(space, "I8xI", material)
            head = _unpack(space, "hHii", material + 16)
            centroid = _unpack(space, "III", material + 28)
            lighting = _unpack(space, LIGHTING_FMT, material + 40)
            plane_etc = _unpack(space, "IIIIhH", material + 156)
            vtype, _, vcount = _unpack(space, "hHi", material + 176)
            ltype, _, lcount = _unpack(space, "hHi", material + 196)
            uncompressed = _unpack(space, "iIiI", material + 216)
            compressed = _unpack(space, "iIiI", material + 236)
            if vtype != ENVIRONMENT_COMPRESSED or (lcount and ltype != LIGHTMAP_COMPRESSED):
                raise GeometryError("BSP material vertex type not handled by this profile")
            if vcount < 0 or lcount < 0 or vcount > 0xFFFF or lcount > vcount:
                raise GeometryError("BSP material vertex counts outside bounds")
            if uncompressed[0] != 0:
                raise GeometryError("BSP material keeps uncompressed vertices")
            if compressed[0] != 32 * vcount + 8 * lcount:
                raise GeometryError("BSP material vertex data size differs from its counts")
            first_surface, count = head[2], head[3]
            if first_surface < 0 or count < 0 or first_surface + count > surface_count:
                raise GeometryError("BSP material surfaces outside the surface block")
            local = surface_values[3 * first_surface:3 * (first_surface + count)]
            if local and max(local) >= vcount:
                raise GeometryError("BSP surface index past its material's vertices")
            data = space.bytes_at(space.offset(compressed[3], compressed[0]), compressed[0]) if compressed[0] else b""
            vertices.append(br.to_big_endian(data[:32 * vcount], ENV_VERTEX_FMT, vcount))
            lightmap_vertices.append(br.to_big_endian(data[32 * vcount:], LIGHTMAP_VERTEX_FMT, lcount))
            materials.append((group, datum) + head + centroid + lighting + plane_etc +
                             (vtype, ltype if lcount else 0, vcount, first_vertex, lcount, first_lightmap_vertex))
            source += [space.bytes_at(material, MATERIAL_BYTES), data]
            first_vertex += vcount
            first_lightmap_vertex += lcount
        first_material += len(elements)
    source.append(surface_bytes)
    record = (lightmap_datum if lightmap_group == GROUP["bitm"] else 0xFFFFFFFF, len(lightmaps), len(materials),
              surface_count)
    blob = br.pack_container(b"HWL1", [
        (1, HWL_SPEC[1], 1, br.encode_records(record, BSP_FMT, 1)),
        (2, HWL_SPEC[2], len(lightmaps), br.encode_records([v for r in lightmaps for v in r], LIGHTMAP_FMT,
                                                           len(lightmaps))),
        (3, HWL_SPEC[3], len(materials), br.encode_records([v for r in materials for v in r], MATERIAL_FMT,
                                                           len(materials))),
        (4, HWL_SPEC[4], first_vertex, b"".join(vertices)),
        (5, HWL_SPEC[5], first_lightmap_vertex, b"".join(lightmap_vertices)),
        (6, HWL_SPEC[6], surface_count, br.to_big_endian(surface_bytes, SURFACE_FMT, surface_count))])
    facts = {"lightmaps": len(lightmaps), "materials": len(materials), "vertices": first_vertex,
             "lightmap_vertices": first_lightmap_vertex, "surfaces": surface_count, "source_sha256": _sha(*source)}
    return blob, facts


def unpack_lightmaps(blob):
    sections = br.unpack_container(blob, b"HWL1", HWL_SPEC)
    bsp = br.decode_records(sections[1][1], BSP_FMT, sections[1][0], ">")
    if sections[1][0] != 1 or bsp[1:] != (sections[2][0], sections[3][0], sections[6][0]):
        raise GeometryError("BSP record counts differ from the sections")
    width = len(struct.unpack("<" + MATERIAL_FMT, bytes(HWL_SPEC[3])))
    values = br.decode_records(sections[3][1], MATERIAL_FMT, sections[3][0], ">")
    surfaces = br.decode_records(sections[6][1], SURFACE_FMT, sections[6][0], ">")
    v_cursor = l_cursor = 0
    for i in range(sections[3][0]):
        row = values[i * width:(i + 1) * width]
        first_surface, count = row[4], row[5]
        vcount, first_vertex, lcount, first_lightmap = row[-4:]
        if first_vertex != v_cursor or first_lightmap != l_cursor or first_surface + count > sections[6][0]:
            raise GeometryError("BSP material ranges inconsistent")
        local = surfaces[3 * first_surface:3 * (first_surface + count)]
        if local and max(local) >= vcount:
            raise GeometryError("BSP surface index past its material's vertices")
        v_cursor += vcount
        l_cursor += lcount
    if (v_cursor, l_cursor) != (sections[4][0], sections[5][0]):
        raise GeometryError("BSP material ranges do not cover the vertex arrays")
    return sections


# ---- collision BSPs ---------------------------------------------------------------------
def _collision_bsp(space, bsp):
    arrays, source = [], [space.bytes_at(bsp, COLLISION_BSP_BYTES)]
    for field, fmt, maximum in COLLISION_ARRAYS:
        elements = space.block(bsp + field, br.record_size(fmt), maximum)
        size = br.record_size(fmt) * len(elements)
        data = space.bytes_at(elements[0], size) if elements else b""
        arrays.append((len(elements), data))
        source.append(data)
    counts = [count for count, _ in arrays]
    edges = br.decode_records(arrays[6][1], COLLISION_ARRAYS[6][1], counts[6])
    for e in range(counts[6]):
        start, end, _, _, left, right = edges[6 * e:6 * e + 6]
        if not (0 <= start < counts[7] and 0 <= end < counts[7] and -1 <= left < counts[5] and
                -1 <= right < counts[5]):
            raise GeometryError("collision edge names a vertex or surface outside its BSP")
    surfaces = br.decode_records(arrays[5][1], COLLISION_ARRAYS[5][1], counts[5])
    if any(not 0 <= surfaces[5 * s + 1] < counts[6] for s in range(counts[5])):
        raise GeometryError("collision surface's first edge outside its BSP")
    leaves = br.decode_records(arrays[2][1], COLLISION_ARRAYS[2][1], counts[2])
    # (an empty leaf may name no first reference: count 0, first -1)
    if any(leaves[3 * l + 1] < 0 or (leaves[3 * l + 1] and (leaves[3 * l + 2] < 0 or
                                                             leaves[3 * l + 1] + leaves[3 * l + 2] > counts[3]))
           for l in range(counts[2])):
        raise GeometryError("collision leaf references outside its BSP")
    return arrays, source


def convert_collision(bsps):
    """HWC1 bytes and facts for [(node, bsp index, space, bsp offset)]."""
    records, payloads, source = [], [[] for _ in COLLISION_ARRAYS], []
    firsts = [0] * len(COLLISION_ARRAYS)
    for node, index, space, bsp in bsps:
        arrays, raw = _collision_bsp(space, bsp)
        row = [node, index]
        for k, ((count, data), (_, fmt, _)) in enumerate(zip(arrays, COLLISION_ARRAYS)):
            row += [firsts[k], count]
            payloads[k].append(br.to_big_endian(data, fmt, count))
            firsts[k] += count
        records.append(tuple(row))
        source += raw
    sections = [(1, HWC_SPEC[1], len(records), br.encode_records([v for r in records for v in r], BSPS_FMT,
                                                                  len(records)))]
    for k in range(len(COLLISION_ARRAYS)):
        sections.append((k + 2, HWC_SPEC[k + 2], firsts[k], b"".join(payloads[k])))
    facts = {"bsps": len(records), "elements": sum(firsts), "source_sha256": _sha(*source)}
    facts.update({name: firsts[k] for k, name in enumerate(("bsp3d_nodes", "planes", "leaves", "bsp2d_references",
                                                            "bsp2d_nodes", "surfaces", "edges", "vertices"))})
    return br.pack_container(b"HWC1", sections), facts


def collision_model_bsps(cache, instance):
    space = TagSpace(cache)
    root = cache.root_offset(instance, COLLISION_MODEL_ROOT_BYTES)
    if root is None:
        raise GeometryError("collision model root outside tag data")
    found = []
    for n, node in enumerate(space.block(root + COLLISION_MODEL_NODES, COLLISION_NODE_BYTES, MAX_COLLISION_NODES)):
        for b, bsp in enumerate(space.block(node + COLLISION_NODE_BSPS, COLLISION_BSP_BYTES, MAX_NODE_BSPS)):
            found.append((n, b, space, bsp))
    return found


def structure_collision_bsps(space, root):
    return [(-1, b, space, bsp) for b, bsp in enumerate(space.block(root + BSP_COLLISION, COLLISION_BSP_BYTES, 1))]


def unpack_collision(blob):
    sections = br.unpack_container(blob, b"HWC1", HWC_SPEC)
    count = sections[1][0]
    rows = br.decode_records(sections[1][1], BSPS_FMT, count, ">")
    width = 2 + 2 * len(COLLISION_ARRAYS)
    cursors = [0] * len(COLLISION_ARRAYS)
    for i in range(count):
        row = rows[i * width:(i + 1) * width]
        for k in range(len(COLLISION_ARRAYS)):
            first, n = row[2 + 2 * k], row[3 + 2 * k]
            if first != cursors[k]:
                raise GeometryError("collision BSP ranges out of order")
            cursors[k] += n
    if cursors != [sections[k + 2][0] for k in range(len(COLLISION_ARRAYS))]:
        raise GeometryError("collision BSP ranges do not cover the arrays")
    return sections
