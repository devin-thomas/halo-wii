"""Render staging (HWI-016B): authored fixtures only, no owned assets."""
import hashlib
import json
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

import be_records as br
import content_geometry as cg
import render_stage as rs

NONE = rs.NONE
MATERIAL_FIELDS = len(struct.unpack("<" + cg.MATERIAL_FMT, bytes(cg.HWL_SPEC[3])))


def hwl_blob(lightmap_datum, pages, materials):
    """pages: [(bitmap index, first material, count)]; materials: [(shader datum, permutation, surfaces,
    lightmap vertices)], each with three vertices and its own surfaces"""
    rows, surfaces, first_surface, first_vertex, first_lightmap = [], [], 0, 0, 0
    for shader, permutation, count, lightmap_vertices in materials:
        row = [0] * MATERIAL_FIELDS
        row[0], row[1], row[2], row[4], row[5] = 0x73686472, shader, permutation, first_surface, count
        row[-6:] = [cg.ENVIRONMENT_COMPRESSED, cg.LIGHTMAP_COMPRESSED if lightmap_vertices else 0, 3, first_vertex,
                    lightmap_vertices, first_lightmap]
        rows += row
        surfaces += [0, 1, 2] * count
        first_surface += count
        first_vertex += 3
        first_lightmap += lightmap_vertices
    lightmaps = [value for page in pages for value in (page[0], 0, page[1], page[2])]
    return br.pack_container(b"HWL1", [
        (1, cg.HWL_SPEC[1], 1, br.encode_records((lightmap_datum, len(pages), len(materials), first_surface),
                                                 cg.BSP_FMT, 1)),
        (2, cg.HWL_SPEC[2], len(pages), br.encode_records(lightmaps, cg.LIGHTMAP_FMT, len(pages))),
        (3, cg.HWL_SPEC[3], len(materials), br.encode_records(rows, cg.MATERIAL_FMT, len(materials))),
        (4, 32, first_vertex, bytes(32 * first_vertex)),
        (5, 8, first_lightmap, bytes(8 * first_lightmap)),
        (6, 6, first_surface, br.encode_records(surfaces, cg.SURFACE_FMT, first_surface))])


# two lightmap pages and a page-less group; environment shaders 0x10 and 0x11
# share base map tag 30 (two bitmaps); 0x12 is transparent; 0x13 has no base map
PAGES = [(0, 0, 2), (1, 2, 2), (-1, 4, 1)]
MATERIALS = [(0xE0000010, 0, 40, 3), (0xE0000011, 3, 10, 3), (0xE0000012, 0, 5, 3), (0xE0000013, 0, 7, 3),
             (0xE0000012, 0, 2, 0)]
SHADERS = {0xE0000010: (3, 0xA000001E), 0xE0000011: (3, 0xA000001E), 0xE0000012: (5, NONE),
           0xE0000013: (3, NONE)}


class SelectionTests(unittest.TestCase):
    def test_hwl_records_round_trip(self):
        datum, pages, materials = rs.hwl_records(hwl_blob(0xB0000042, PAGES, MATERIALS))
        self.assertEqual(datum, 0xB0000042)
        self.assertEqual(pages, PAGES)
        self.assertEqual(materials, MATERIALS)

    def test_selects_geometry_lightmap_pages_and_base_maps_by_coverage(self):
        files = rs.select("map", 1641, 0xB0000042, PAGES, MATERIALS, SHADERS, {30: 2})
        self.assertEqual(files, [
            ("render/map/lightmaps/01641.hwl", "geometry", 0),
            ("render/map/textures/00066-000.hwt", "lightmap", 50),
            ("render/map/textures/00066-001.hwt", "lightmap", 12),
            # permutation 0 % 2 and 3 % 2
            ("render/map/textures/00030-000.hwt", "base", 40),
            ("render/map/textures/00030-001.hwt", "base", 10)])

    def test_no_lightmap_bitmap_stages_no_pages(self):
        files = rs.select("map", 7, NONE, PAGES, MATERIALS, SHADERS, {30: 2})
        self.assertEqual([kind for _, kind, _ in files], ["geometry", "base", "base"])

    def test_base_map_without_bitmaps_fails(self):
        with self.assertRaises(rs.StageError):
            rs.select("map", 7, NONE, PAGES, MATERIALS, SHADERS, {30: 0})

    def test_paths_and_stage_arguments(self):
        self.assertEqual(rs.source_path("render/map/textures/00030-001.hwt"), "textures/map/00030-001.hwt")
        self.assertEqual(rs.source_path("render/map/lightmaps/01641.hwl"), "lightmaps/map/01641.hwl")
        record = {"files": [{"path": "render/map/lightmaps/01641.hwl"}]}
        self.assertEqual(rs.stage_arguments(Path("/private/out"), record),
                         ["/private/out/render/map/lightmaps/01641.hwl=" + rs.SD_ROOT + "/map/lightmaps/01641.hwl"])

    def test_engine_layout_offsets(self):
        # shader_environment diffuse.base_map (render_engine.c asserts 0x88) and its index
        self.assertEqual(rs.SHADER_BASE_MAP_INDEX, 0x94)
        self.assertEqual(rs.SHADER_TYPE_OFFSET, 0x24)
        self.assertEqual(rs.BITMAP_GROUP_BITMAPS, 0x60)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.directory)

    def test_manifest_index_requires_the_pipeline_format(self):
        (self.directory / "manifest.json").write_text(json.dumps({"format": "other", "outputs": []}),
                                                      encoding="utf-8")
        with self.assertRaises(rs.StageError):
            rs.manifest_index(self.directory)
        output = {"path": "textures/map/00030-000.hwt", "bytes": 3, "sha256": hashlib.sha256(b"abc").hexdigest()}
        (self.directory / "manifest.json").write_text(
            json.dumps({"format": "halo-wii-content-manifest", "outputs": [output]}), encoding="utf-8")
        self.assertEqual(rs.manifest_index(self.directory), {output["path"]: output})


if __name__ == "__main__":
    unittest.main()
