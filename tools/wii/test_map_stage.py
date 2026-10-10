"""Tests for the map stager (HWI-015B): tools/wii/map_stage.py and tools/wii/map_layouts.py.

Everything here is synthetic or generated from the engine's source: no owned
map data is read.
"""
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import map_layouts  # noqa: E402
import map_stage  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


class GeneratedLayoutsTest(unittest.TestCase):
    """map_layouts.json against the schema tables it was generated beside"""

    @classmethod
    def setUpClass(cls):
        cls.schema = map_stage.Schema(map_stage.TABLES.read_text(encoding="utf-8"))
        cls.layouts = map_stage.load_layouts(cls.schema)

    def test_every_definition_has_a_layout_of_its_size(self):
        self.assertEqual(len(self.layouts["definitions"]), len(self.schema.definitions))
        for item, (size, _, _, name) in zip(self.layouts["definitions"], self.schema.definitions):
            self.assertEqual((item["name"], item["size"]), (name, size))
            self.assertLessEqual(item["size"], item["type_size"])
            for offset, width, count in item["swaps"]:
                self.assertIn(width, (2, 4, 8))
                self.assertEqual(offset % width, 0, item["name"])
                self.assertLessEqual(offset + width * count, item["size"])

    def test_no_definition_needs_an_explicit_decoder(self):
        for item in self.layouts["definitions"]:
            self.assertEqual(item["conflicts"], [], item["name"])
            self.assertEqual(item["bitfields"], [], item["name"])

    def test_script_node_value_is_the_one_known_union(self):
        node = self.layouts["types"]["struct hs_syntax_node"]
        self.assertEqual(node["size"], 20)
        self.assertEqual([c["offset"] for c in node["conflicts"]], [16])

    def test_every_pointer_field_of_the_schema_is_a_planned_word(self):
        # a block's address and a reference's name are 32-bit scalars of their definition
        widths = {}
        for definition in range(len(self.schema.definitions)):
            codes = map_stage.definition_widths(self.schema, self.layouts, definition, widths)
            _, first, count, _ = self.schema.definitions[definition]
            for field_id in range(first, first + count):
                kind, _, size, repeat, offset = self.schema.fields[field_id][:5]
                for index in range(repeat):
                    at = offset + index * size
                    if kind == map_stage.F_BLOCK:
                        self.assertEqual((codes[at], codes[at + 4], codes[at + 8]), (4, 4, 4))
                    elif kind == 3:  # reference: group, name, length, index
                        self.assertEqual([codes[at + k] for k in (0, 4, 8, 12)], [4, 4, 4, 4])

    def test_generated_file_has_no_host_path(self):
        text = (ROOT / "tools/wii/map_layouts.json").read_text(encoding="utf-8")
        self.assertNotIn("/mnt/", text)
        self.assertNotIn(":\\", text)


class FlattenerTest(unittest.TestCase):
    DUMP = {
        "struct map_layout_0": [
            "         0 |   struct outer value",
            "         0 |     short a",
            "         2 |     char[2] b",
            "         4 |     union point2d c",
            "         4 |       short[2] n",
            "         4 |       struct point2d::(anonymous at x.h:1:2) ",
            "         4 |         short x",
            "         6 |         short y",
            "         8 |     struct inner[2] d",
            "        16 |     float * e",
            "           | [sizeof=20, align=4]",
        ],
        "struct inner": ["         0 |   long v", "           | [sizeof=4, align=4]"],
        "struct map_layout_1": [
            "         0 |   union bad value",
            "         0 |     long whole",
            "         0 |     short half",
            "           | [sizeof=4, align=4]",
        ],
    }

    def test_nested_unions_arrays_and_pointers(self):
        flattener = map_layouts.Flattener(self.DUMP)
        result = flattener.record("struct map_layout_0", 0, "")
        self.assertEqual(result["conflicts"], [])
        self.assertEqual(map_layouts.runs(result["scalars"], 20), [[0, 2, 1], [4, 2, 2], [8, 4, 3]])

    def test_union_of_different_widths_is_a_conflict(self):
        flattener = map_layouts.Flattener(self.DUMP)
        result = flattener.record("struct map_layout_1", 0, "")
        self.assertEqual(len(result["conflicts"]), 1)
        self.assertEqual(result["scalars"], [])


class PlanTest(unittest.TestCase):
    def test_bytes_are_planned_once(self):
        plan = map_stage.Plan(64)
        plan.add(0, 4, 2, "x")
        with self.assertRaises(map_stage.StageError):
            plan.add(4, 2, 1, "y")
        with self.assertRaises(map_stage.StageError):
            plan.add(62, 4, 1, "z")

    def test_apply_swaps_each_width(self):
        data = bytearray(range(16))
        plan = map_stage.Plan(16)
        plan.add(0, 4, 1, "a")
        plan.add(4, 2, 2, "b")
        plan.bytes_(8, 4, "c")
        plan.add(12, 4, 1, "d")
        plan.apply(data)
        self.assertEqual(bytes(data), bytes([3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 15, 14, 13, 12]))
        self.assertEqual(plan.words, {0, 12})


class DataRuleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = map_stage.Schema(map_stage.TABLES.read_text(encoding="utf-8"))
        cls.layouts = map_stage.load_layouts(cls.schema)

    def member(self, type_name, path):
        return map_stage.member(self.layouts, type_name, path)

    def animation(self, slot, element, node_count, rotation, translation, scale, frames, frame_size, flags=0,
                  compressed_offset=0):
        def put(path, fmt, value):
            struct.pack_into("<" + fmt, slot, element + self.member("struct animation", path), value)
        put("node_count", "h", node_count)
        put("frame_count", "h", frames)
        put("frame_size", "h", frame_size)
        put("flags", "H", flags)
        put("compressed_data_offset", "i", compressed_offset)
        put("nodes_with_rotation_flags[0]", "I", rotation)
        put("nodes_with_translation_flags[0]", "I", translation)
        put("nodes_with_scale_flags[0]", "I", scale)

    def test_uncompressed_frames_and_defaults(self):
        slot = bytearray(4096)
        # two nodes: node 0 rotates and translates, node 1 only scales
        self.animation(slot, 0, 2, 0b01, 0b01, 0b10, 3, 8 + 12 + 4)
        plan = map_stage.Plan(4096)
        map_stage.rule_animation_data(plan, slot, self.layouts, 1024, 3 * 24, 0)
        self.assertEqual(plan.runs[:3], [(1024, 2, 4), (1032, 4, 3), (1044, 4, 1)])
        plan = map_stage.Plan(4096)
        # defaults: node 0 its scale, node 1 its rotation and translation
        map_stage.rule_animation_default(plan, slot, self.layouts, 2048, 4 + 8 + 12, 0)
        self.assertEqual(plan.runs, [(2048, 4, 1), (2052, 2, 4), (2060, 4, 3)])

    def test_compressed_parts_by_their_offsets(self):
        slot = bytearray(8192)
        self.animation(slot, 0, 1, 1, 0, 0, 2, 8, flags=1, compressed_offset=16)
        base = 1024 + 16
        parts = [48 + 8 * index for index in range(11)]
        for (name, _), part in zip(map_stage.COMPRESSED_REGIONS, parts):
            struct.pack_into("<i", slot, base + self.member("struct compressed_animation_header", name), part)
        plan = map_stage.Plan(8192)
        map_stage.rule_animation_data(plan, slot, self.layouts, 1024, 16 + 48 + 88, 0)
        widths = {offset - base: width for offset, width, _ in plan.runs if offset >= base}
        self.assertEqual(widths[0], 4)        # the header's offsets
        self.assertEqual(widths[44], 4)       # rotation node headers
        self.assertEqual(widths[48], 2)       # rotation keyframe frame indices
        self.assertEqual(widths[48 + 8 * 3], 4)  # translation node headers
        self.assertEqual(plan.owner[1024:1024 + 152].count(1), 152)

    def test_script_nodes_by_type(self):
        header = self.layouts["types"]["struct data_array"]
        node = self.layouts["types"]["struct hs_syntax_node"]
        size = header["size"] + 3 * node["size"]
        slot = bytearray(size)
        struct.pack_into("<h", slot, self.member("struct data_array", "maximum_count"), 3)
        struct.pack_into("<h", slot, self.member("struct data_array", "size"), node["size"])
        for index, (node_type, flags) in enumerate(((1, 1), (3, 1), (2, 1))):
            at = header["size"] + index * node["size"]
            struct.pack_into("<h", slot, at + self.member("struct hs_syntax_node", "type"), node_type)
            struct.pack_into("<H", slot, at + self.member("struct hs_syntax_node", "flags"), flags)
        plan = map_stage.Plan(size)
        map_stage.rule_syntax_data(plan, slot, self.layouts, 0, size, 0)
        values = [header["size"] + index * node["size"] + 16 for index in range(3)]
        runs = {offset: width for offset, width, _ in plan.runs}
        self.assertNotIn(values[0], runs)     # a boolean's byte
        self.assertEqual(runs[values[1]], 2)  # a short's short
        self.assertEqual(runs[values[2]], 4)  # a real's float
        self.assertEqual(plan.owner.count(1), size)


class FileFormatTest(unittest.TestCase):
    def test_wmap_round_trip(self):
        header = bytearray(0x800)
        struct.pack_into("<6I", header, 0, 0x68656164, 5, 0x3000, 0, 0x800, 0x100)
        struct.pack_into("<I", header, 0x7FC, 0x666F6F74)
        image = bytearray(range(256)) * 0x100
        bsp = {"file_offset": 0x900, "bytes": 0x40, "slot_offset": 0x8000}
        for big in (True, False):
            data = map_stage.write_wmap(header_bytes=bytes(header), image=bytes(image), tag_bytes=0x100,
                                        tag_offset=0x800, bsps=[bsp], relocations=[0, 8, 0x8004],
                                        file_length=0x3000, big_endian=big)
            order = ">" if big else "<"
            self.assertEqual(data[:8], map_stage.MAGIC)
            self.assertEqual(struct.unpack_from(order + "I", data, 8)[0], 0x01020304)
            count, header_size, header_at, length = struct.unpack_from(order + "4I", data, 12)
            self.assertEqual((count, header_size, length), (2, 0x800, 0x3000))
            self.assertEqual(struct.unpack_from(order + "I", data, header_at + 4)[0], 5)
            segments = [struct.unpack_from(order + "8I", data, 0x20 + 32 * index) for index in range(count)]
            kind, map_offset, size, slot_offset, data_at, relocations, relocation_at, _ = segments[1]
            self.assertEqual((kind, map_offset, size, slot_offset), (2, 0x900, 0x40, 0x8000))
            self.assertEqual(data[data_at:data_at + size], bytes(image[0x8000:0x8040]))
            self.assertEqual(struct.unpack_from(order + "I", data, relocation_at)[0], 0x8004)
            self.assertEqual(segments[0][5], 2)


if __name__ == "__main__":
    unittest.main()
