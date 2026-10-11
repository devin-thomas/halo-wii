"""Campaign render residency (HWI-016E): the Wii rasterizer draws BSP geometry from the tag slot.

GX reads each BSP material's vertices where the staged map (map_stage.py)
holds them, and the HWL1 file (content_geometry.py) is only compared with
them at run time. These tests tie the two converters and the renderer's
layout constants together on authored fixtures (no owned assets).
"""
import random
import re
import struct
import unittest
from pathlib import Path

import be_records as br
import content_geometry as cg
import map_stage

ROOT = Path(__file__).resolve().parents[2]
RENDER_GX = ROOT / "port/wii/render/render_gx.c"
RENDER_ENGINE = ROOT / "port/wii/render/render_engine.c"


def defines(path):
    """the file's integer #defines"""
    text = path.read_text(encoding="utf-8")
    return {name: int(value, 0) for name, value in re.findall(r"#define (\w+) (0x[0-9a-fA-F]+|\d+)u?\b", text)}


class StagedVerticesEqualHwl1(unittest.TestCase):
    """The tag slot's bytes (map_stage.py's plan for compressed_vertex_data) are HWL1's vertex and
    lightmap vertex sections for the same source bytes."""

    @classmethod
    def setUpClass(cls):
        cls.schema = map_stage.Schema(map_stage.TABLES.read_text(encoding="utf-8"))
        cls.layouts = map_stage.load_layouts(cls.schema)

    def member(self, path):
        return map_stage.member(self.layouts, "struct structure_material", path)

    def staged_and_converted(self, vertices, lightmap, tail=0):
        rng = random.Random(vertices * 1000 + lightmap)
        size = 32 * vertices + 8 * lightmap + tail
        element, data_at = 0, 1024
        slot = bytearray(data_at + size)
        struct.pack_into("<i", slot, element + self.member("vertices.count"), vertices)
        struct.pack_into("<i", slot, element + self.member("lightmap_vertices.count"), lightmap)
        source = bytes(rng.randrange(256) for _ in range(size))
        slot[data_at:] = source
        plan = map_stage.Plan(len(slot))
        map_stage.rule_material_compressed(plan, slot, self.layouts, data_at, size, element)
        staged = bytearray(slot)
        plan.apply(staged)
        converted = (br.to_big_endian(source[:32 * vertices], cg.ENV_VERTEX_FMT, vertices) +
                     br.to_big_endian(source[32 * vertices:32 * vertices + 8 * lightmap], cg.LIGHTMAP_VERTEX_FMT,
                                      lightmap))
        return bytes(staged[data_at:data_at + 32 * vertices + 8 * lightmap]), converted

    def test_vertices_and_lightmap_vertices_are_the_same_bytes(self):
        for vertices, lightmap in ((1, 1), (3, 2), (17, 17), (5, 0)):
            staged, converted = self.staged_and_converted(vertices, lightmap)
            self.assertEqual(staged, converted, (vertices, lightmap))

    def test_every_byte_of_the_data_is_planned(self):
        rng_size = 32 * 4 + 8 * 4
        slot = bytearray(1024 + rng_size)
        struct.pack_into("<i", slot, self.member("vertices.count"), 4)
        struct.pack_into("<i", slot, self.member("lightmap_vertices.count"), 4)
        plan = map_stage.Plan(len(slot))
        map_stage.rule_material_compressed(plan, slot, self.layouts, 1024, rng_size, 0)
        self.assertEqual(plan.owner[1024:].count(1), rng_size)


class RendererLayout(unittest.TestCase):
    """render_gx.c's strides and offsets are the converters' record layouts."""

    def test_vertex_strides_and_offsets(self):
        d = defines(RENDER_GX)
        env = cg.ENV_VERTEX_FMT
        self.assertEqual(d["VERTEX_BYTES"], br.record_size(env))
        # position: words 0-2; three packed normals: words 3-5; texcoord: words 6-7 (HWL1 header)
        self.assertEqual(d["VERTEX_TEXCOORD_OFFSET"], br.record_size(env[:6]))
        self.assertEqual(d["LIGHTMAP_VERTEX_BYTES"], br.record_size(cg.LIGHTMAP_VERTEX_FMT))
        # the packed incident radiosity, then the texcoord's two s16
        self.assertEqual(cg.LIGHTMAP_VERTEX_FMT[1:], "hh")
        self.assertEqual(d["LIGHTMAP_TEXCOORD_OFFSET"], br.record_size(cg.LIGHTMAP_VERTEX_FMT[:1]))
        self.assertEqual(br.record_size(cg.SURFACE_FMT), 6)
        self.assertEqual((d["ENVIRONMENT_COMPRESSED"], d["LIGHTMAP_COMPRESSED"]),
                         (cg.ENVIRONMENT_COMPRESSED, cg.LIGHTMAP_COMPRESSED))

    def test_geometry_is_not_decoded_or_copied(self):
        text = RENDER_GX.read_text(encoding="utf-8")
        self.assertNotIn("hwl_load(", text)
        arrays = re.findall(r"GX_SetArray\((GX_VA_\w+), ([^;]+);", text)
        self.assertTrue(arrays)
        for attribute, rest in arrays:
            self.assertRegex(rest, r"m->(vertices|lightmap_vertices)", attribute)

    def test_no_normal_is_read(self):
        """the environment path draws no lighting: no packed normal, binormal, tangent or incident
        radiosity is read (they stay in the tag slot as staged, for HWI-032)"""
        code = re.sub(r"/\*.*?\*/", "", RENDER_GX.read_text(encoding="utf-8"), flags=re.S)
        for word in ("normal", "binormal", "tangent", "incident", "content_unpack_vector"):
            self.assertNotIn(word, code)

    def test_engine_passes_the_tag_slot_vertices(self):
        text = RENDER_ENGINE.read_text(encoding="utf-8")
        self.assertIn("material->compressed_vertex_data.address", text)
        self.assertIn("structure_bsp->surfaces.address", text)

    def test_hwl1_is_compared_in_chunks(self):
        d = defines(RENDER_GX)
        self.assertLessEqual(d["CHECK_CHUNK_BYTES"], 65536)
        text = RENDER_GX.read_text(encoding="utf-8")
        self.assertIn("geometry_check", text)
        self.assertNotIn("content_load_file(", text)


if __name__ == "__main__":
    unittest.main()
