"""Authored-fixture tests of the HWI-008C content categories.

Covers model geometry (HWM1), structure-BSP lightmap geometry (HWL1),
collision BSPs (HWC1), model animations (HMA1), fonts (HWF1), unicode text
(HUS1) and the shader in-place census: conversion, explicit decoding back to
the authored values, determinism, rejection of each authored defect without
touching the published generation, container validation, and the tag
offsets against the upstream validator schema tables.
"""
import hashlib
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import be_records as br  # noqa: E402
import content_animation as ca  # noqa: E402
import content_convert as cc  # noqa: E402
import content_fixtures as fx  # noqa: E402
import content_geometry as cg  # noqa: E402
import content_publish as cp  # noqa: E402
import content_text as ct  # noqa: E402

NEW = ["models", "lightmaps", "collision", "model_animations", "fonts", "strings", "shaders"]


def tree(directory):
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(Path(directory).rglob("*")) if p.is_file()}


class CategoryConversionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def staging(self, defects=(), name="staging"):
        return fx.write_staging(self.root / name, {"alpha": fx.build_map("alpha", categories=set(defects)),
                                                   "beta": fx.build_map("beta", category=1)})

    def convert(self, staging, out="out", categories=None, **kwargs):
        return cc.run(staging, None, categories or NEW, "gx-baseline-v1", self.root / out, **kwargs)

    def published(self, out="out"):
        current = cp.read_current(self.root / out, cc.verify_output)
        return current["manifest"], self.root / out / "generations" / current["generation"]

    def output(self, manifest, generation, kind):
        entry = next(o for o in manifest["outputs"] if o["kind"] == kind)
        return entry, (generation / entry["path"]).read_bytes()

    def test_every_category_converts_and_decodes_to_the_authored_values(self):
        record = self.convert(self.staging())
        manifest, generation = self.published()
        kinds = {o["kind"]: o for o in manifest["outputs"]}
        self.assertEqual(set(kinds), {"model", "lightmap_geometry", "collision", "model_animation", "font",
                                      "strings"})
        self.assertEqual(sum(1 for o in manifest["outputs"] if o["kind"] == "collision"), 2)  # model and BSP
        self.assertEqual(sum(1 for o in manifest["outputs"] if o["kind"] == "strings"), 2)    # list and HUD
        self.assertEqual(manifest["in_place"]["shaders"], {"alpha": {"senv": {"tags": 1, "root_bytes": 836}}, "beta": {}})
        self.assertEqual(record["totals"]["model"]["files"], 1)
        # model: parts, little-endian source vertices rebuilt from the big-endian ones
        entry, blob = self.output(manifest, generation, "model")
        rows, vertices, indices = cg.unpack_model(blob)
        self.assertEqual((len(rows), vertices, indices), (2, 9, 13))
        self.assertEqual(rows[0][13:19], (5, 1, 5, 0, 7, 0))
        self.assertEqual(rows[1][13:19], (5, 0, 4, 5, 6, 7))
        sections = br.unpack_container(blob, b"HWM1", cg.HWM_SPEC)
        first = br.to_little_endian(sections[2][1], cg.MODEL_VERTEX_FMT, vertices)
        self.assertEqual(first[:160], fx.pattern(160, 40))
        self.assertEqual(entry["source"]["parts"], 2)
        # lightmap geometry
        _, blob = self.output(manifest, generation, "lightmap_geometry")
        sections = cg.unpack_lightmaps(blob)
        self.assertEqual([sections[i][0] for i in range(1, 7)], [1, 1, 2, 7, 4, 3])
        self.assertEqual(br.to_little_endian(sections[4][1], cg.ENV_VERTEX_FMT, 7)[:128], fx.pattern(160, 90)[:128])
        self.assertEqual(br.decode_records(sections[6][1], cg.SURFACE_FMT, 3, ">"), (0, 1, 2, 2, 1, 3, 0, 2, 1))
        # collision: the same authored BSP in the model and the structure
        blobs = [(generation / o["path"]).read_bytes() for o in manifest["outputs"] if o["kind"] == "collision"]
        model_bsp, structure_bsp = (cg.unpack_collision(b) for b in blobs)
        self.assertEqual([bytes(model_bsp[k][1]) for k in range(2, 10)],
                         [bytes(structure_bsp[k][1]) for k in range(2, 10)])
        sections = model_bsp
        self.assertEqual(br.decode_records(sections[8][1], "iiiiii", 3, ">")[:6], (0, 1, 1, 2, 0, -1))
        self.assertEqual(struct.unpack(">4f", sections[3][1][16:32]), (1.0, 0.0, 0.0, -1.0))
        # model animations: uncompressed frames, defaults and the compressed block
        _, blob = self.output(manifest, generation, "model_animation")
        rows = ca.unpack_graph(blob)
        self.assertEqual([(r[1], r[2], r[3], r[5]) for r in rows], [(0, 4, 32, 3), (0, 6, 28, 2), (1, 1, 0, 0)])
        sections = br.unpack_container(blob, b"HMA1", ca.HMA_SPEC)
        frame = struct.unpack_from(">4h3f4h", sections[4][1], 0)
        self.assertEqual(frame, (0, 0, 300, -32000, 0.0, 0.0, -1.5, 0, -1, 300, -32000))
        block = bytes(sections[5][1])
        self.assertEqual(len(block), len(fx.compressed_animation_block(2, [3, 6], 2)))
        self.assertEqual(ca.convert_compressed(fx.compressed_animation_block(2, [3, 6], 2), 2, (2, 1, 0)), block)
        self.assertEqual(struct.unpack_from(">11i", block, 0)[0], 0x2C + 8)
        # font and strings
        _, blob = self.output(manifest, generation, "font")
        head, sections = ct.unpack_font(blob)
        self.assertEqual(head, (1, 12, 3, 2, 1, 2, 3, 64))
        self.assertEqual(br.decode_records(sections[4][1], ct.CHARACTER_FMT, 3, ">")[9:18],
                         (0x20, 3, -2, 0, 2, -5, -1, 0, 0))
        self.assertEqual(bytes(sections[5][1]), fx.pattern(64, 77))
        texts = []
        for o in manifest["outputs"]:
            if o["kind"] == "strings":
                for units in ct.unpack_strings((generation / o["path"]).read_bytes()):
                    texts.append(struct.pack(">%dH" % len(units), *units).decode("utf-16-be"))
        self.assertEqual(sorted(texts), sorted(["Halo éΩ\0", "two\0", "hud\0text\0"]))

    def test_conversion_is_deterministic_and_old_categories_are_unchanged(self):
        staging = self.staging()
        first = self.convert(staging)
        second = self.convert(staging, out="again")
        self.assertEqual(first["manifest_sha256"], second["manifest_sha256"])
        self.assertEqual(tree(self.root / "out" / "generations"), tree(self.root / "again" / "generations"))
        # adding the new categories leaves texture, animation and sound bytes as they were
        old = self.convert(staging, out="old", categories=["textures", "animations", "sounds"])
        both = self.convert(staging, out="both", categories=list(cc.CATEGORIES))
        old_tree = tree(self.root / "old" / "generations" / old["generation"])
        both_tree = tree(self.root / "both" / "generations" / both["generation"])
        shared = {k: v for k, v in old_tree.items() if k != "manifest.json"}
        self.assertTrue(shared)
        self.assertEqual({k: both_tree[k] for k in shared}, shared)

    def test_each_defect_rejects_and_keeps_the_published_generation(self):
        self.convert(self.staging())
        before = tree(self.root / "out")
        current = (self.root / "out" / "CURRENT").read_text()
        defects = {"model_index": "index past its vertices", "model_vertex_type": "vertex type",
                   "collision_edge": "collision edge", "animation_frame_size": "frame size",
                   "animation_compressed_gap": "no array describes", "font_glyph": "glyph pixels",
                   "string_odd": "odd byte count", "shader_root": "shader root", "bsp_data_size": "vertex data size"}
        for number, (defect, reason) in enumerate(sorted(defects.items())):
            with self.subTest(defect=defect):
                staging = self.staging([defect], name="bad-%d" % number)
                with self.assertRaises(cc.ConversionError) as caught:
                    self.convert(staging)
                self.assertIn(reason, str(caught.exception))
                self.assertNotIn("alpha\\", str(caught.exception))
                self.assertEqual((self.root / "out" / "CURRENT").read_text(), current)
                self.assertEqual(tree(self.root / "out"), before)

    def test_interruption_with_new_categories_preserves_the_last_generation(self):
        staging = self.staging()
        first = self.convert(staging, categories=["models"])
        before = tree(self.root / "out")
        for stage in ("before-manifest", "before-validate", "before-promote", "before-current"):
            with self.subTest(stage=stage), self.assertRaises(InterruptedError):
                self.convert(staging, fault=cp.FaultInjector(stage=stage))
        with self.assertRaises(OSError):
            self.convert(staging, fault=cp.FaultInjector(after_bytes=100))
        self.assertEqual(cp.read_current(self.root / "out")["generation"], first["generation"])
        self.assertEqual({k: v for k, v in tree(self.root / "out").items() if k in before}, before)
        self.assertFalse([p for p in (self.root / "out").iterdir() if p.name.startswith(".staging")])

    def test_tampered_container_fails_verification(self):
        self.convert(self.staging())
        manifest, generation = self.published()
        for kind in ("model", "lightmap_geometry", "collision", "model_animation", "font", "strings"):
            entry, blob = self.output(manifest, generation, kind)
            with self.subTest(kind=kind):
                cc.verify_output(entry, blob)
                for bad in (blob[:-32], blob[:40], blob + bytes(32), b"XXXX" + blob[4:]):
                    with self.assertRaises(cp.PublishError):
                        cc.verify_output(entry, bad)


class ContainerTests(unittest.TestCase):
    SPEC = {1: 4, 2: 2}

    def blob(self):
        return br.pack_container(b"TST1", [(1, 4, 2, struct.pack(">2I", 1, 2)), (2, 2, 3, struct.pack(">3H", 4, 5, 6))])

    def test_round_trip_and_alignment(self):
        blob = self.blob()
        self.assertEqual(len(blob) % 32, 0)
        sections = br.unpack_container(blob, b"TST1", self.SPEC)
        self.assertEqual(br.decode_records(sections[2][1], "H", 3, ">"), (4, 5, 6))

    def test_malformed_containers_reject(self):
        blob = self.blob()
        cases = {"magic": b"TSX1" + blob[4:], "truncated": blob[:-32], "header": blob[:20],
                 "reserved": blob[:20] + b"\1" + blob[21:], "padding": blob[:-1] + b"\1",
                 "trailing": blob + bytes(32)}
        swapped = bytearray(blob)
        struct.pack_into(">H", swapped, 32, 2)
        cases["section order"] = bytes(swapped)
        recount = bytearray(blob)
        struct.pack_into(">I", recount, 36, 3)
        cases["count"] = bytes(recount)
        for name, data in cases.items():
            with self.subTest(name=name), self.assertRaises(br.RecordError):
                br.unpack_container(data, b"TST1", self.SPEC)
        with self.assertRaises(br.RecordError):
            br.unpack_container(blob, b"TST1", {1: 4, 2: 2, 3: 8})

    def test_records_are_explicit_and_rebuild_their_source(self):
        source = struct.pack("<hI2B", -2, 0xDEADBEEF, 1, 255)
        big = br.to_big_endian(source, "hIBB", 1)
        self.assertEqual(big, struct.pack(">hI2B", -2, 0xDEADBEEF, 1, 255))
        self.assertEqual(br.to_little_endian(big, "hIBB", 1), source)
        with self.assertRaises(br.RecordError):
            br.to_big_endian(source[:-1], "hIBB", 1)


class SchemaOffsetTests(unittest.TestCase):
    """The tag offsets used here are the upstream validator schema's."""

    @classmethod
    def setUpClass(cls):
        text = (Path(__file__).resolve().parent / "cache_schema_tables.c").read_text(encoding="utf-8")
        cls.definitions = {}
        fields = re.findall(r"\{(-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+)\}",
                            text[text.index("cache_schema_fields[]"):text.index("cache_schema_definitions[]")])
        definitions = re.findall(r"\{(-?\d+), (-?\d+), (-?\d+)\}, /\* (\w+) \*/", text)
        names = [d[3] for d in definitions]
        for size, first, count, name in definitions:
            rows = [tuple(map(int, f)) for f in fields[int(first):int(first) + int(count)]]
            cls.definitions.setdefault(name, []).append((int(size), [
                (r[0], r[4], names[r[8]] if r[0] in (0, 9) and r[8] >= 0 else None) for r in rows]))

    def offset(self, definition, target=None, kind=0, index=0):
        size, fields = self.definitions[definition][index]
        matches = [offset for t, offset, linked in fields if t == kind and (target is None or linked == target)]
        return size, matches

    def test_model_and_animation_offsets(self):
        self.assertEqual(self.offset("model", "model_geometry"), (cg.MODEL_ROOT_BYTES, [cg.MODEL_GEOMETRIES]))
        self.assertEqual(self.offset("model_geometry", "model_geometry_part"), (cg.GEOMETRY_BYTES,
                                                                                [cg.GEOMETRY_PARTS]))
        self.assertEqual(self.offset("model_geometry_part", "model_vertex_compressed")[0], cg.PART_BYTES)
        self.assertEqual(self.offset("animation_graph", "animation"), (ca.GRAPH_ROOT_BYTES, [ca.GRAPH_ANIMATIONS]))
        self.assertEqual(self.offset("animation", kind=1), (ca.ANIMATION_BYTES, [72, 140, 160]))

    def test_bsp_and_collision_offsets(self):
        self.assertEqual(self.offset("structure_bsp", "structure_lightmap"), (cg.BSP_ROOT_BYTES, [cg.BSP_LIGHTMAPS]))
        self.assertEqual(self.offset("structure_bsp", "collision_bsp")[1], [cg.BSP_COLLISION])
        self.assertEqual(self.offset("structure_bsp", "structure_surface")[1], [cg.BSP_SURFACES])
        self.assertEqual(self.offset("structure_lightmap", "structure_material"),
                         (cg.LIGHTMAP_BYTES, [cg.LIGHTMAP_MATERIALS]))
        self.assertEqual(self.offset("structure_material", kind=1), (cg.MATERIAL_BYTES, [216, 236]))
        self.assertEqual(self.offset("collision_model", "collision_node"),
                         (cg.COLLISION_MODEL_ROOT_BYTES, [cg.COLLISION_MODEL_NODES]))
        self.assertEqual(self.offset("collision_node", "collision_bsp"), (cg.COLLISION_NODE_BYTES,
                                                                          [cg.COLLISION_NODE_BSPS]))
        size, fields = self.definitions["collision_bsp"][0]
        self.assertEqual(size, cg.COLLISION_BSP_BYTES)
        self.assertEqual(sorted(offset for t, offset, _ in fields if t == 0),
                         sorted(field for field, _, _ in cg.COLLISION_ARRAYS))
        sizes = {name: self.definitions[name][0][0] for name in (
            "bsp3d_node", "real_plane3d", "collision_leaf", "bsp2d_reference", "bsp2d_node", "collision_surface",
            "collision_edge", "collision_vertex")}
        self.assertEqual(list(sizes.values()), [br.record_size(fmt) for _, fmt, _ in cg.COLLISION_ARRAYS])

    def test_text_and_shader_offsets(self):
        self.assertEqual(self.offset("font", "font_character")[1], [ct.FONT_CHARACTERS])
        self.assertEqual(self.offset("font", "font_character_table"), (ct.FONT_ROOT_BYTES, [ct.FONT_TABLES]))
        self.assertEqual(self.offset("font", kind=1)[1], [ct.FONT_PIXELS])
        self.assertEqual(self.definitions["font_character"][0][0], br.record_size(ct.CHARACTER_FMT))
        self.assertEqual(self.offset("unicode_string_list", "unicode_string_list_entry"),
                         (ct.STRING_LIST_ROOT_BYTES, [0]))
        self.assertEqual(self.offset("hud_message_text", kind=1), (ct.HUD_TEXT_ROOT_BYTES, [0]))
        groups = {"senv": "shader_environment", "soso": "shader_model", "schi": "shader_transparent_chicago",
                  "scex": "shader_transparent_chicago_extended", "sgla": "shader_transparent_glass",
                  "smet": "shader_transparent_meter", "sotr": "shader_transparent_generic",
                  "spla": "shader_transparent_plasma", "swat": "shader_transparent_water"}
        for group, definition in groups.items():
            size, fields = self.definitions[definition][0]
            self.assertEqual(size, ct.SHADER_ROOTS[group], group)
            # parameters only: no tag-data or file-data field anywhere below a shader root
            self.assertFalse(self._has_data(definition), group)

    def _has_data(self, definition, seen=None):
        seen = seen or set()
        if definition in seen:
            return False
        seen.add(definition)
        for kind, _, linked in self.definitions[definition][0][1]:
            if kind in (1, 2) or (linked and self._has_data(linked, seen)):
                return True
        return False


if __name__ == "__main__":
    unittest.main()
