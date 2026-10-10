"""Tests for the map stager (HWI-015B, HWI-016C): tools/wii/map_stage.py and tools/wii/map_layouts.py.

Everything here is synthetic or generated from the engine's source: no owned
map data is read.
"""
import json
from pathlib import Path
import struct
import sys
import tempfile
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

    def script_nodes(self, nodes):
        """a data array of script nodes, (type, flags, 4 value bytes) each; returns (slot, value offsets)"""
        header = self.layouts["types"]["struct data_array"]
        node = self.layouts["types"]["struct hs_syntax_node"]
        size = header["size"] + len(nodes) * node["size"]
        slot = bytearray(size)
        struct.pack_into("<h", slot, self.member("struct data_array", "maximum_count"), len(nodes))
        struct.pack_into("<h", slot, self.member("struct data_array", "size"), node["size"])
        values = []
        for index, (node_type, flags, value) in enumerate(nodes):
            at = header["size"] + index * node["size"]
            struct.pack_into("<h", slot, at + self.member("struct hs_syntax_node", "type"), node_type)
            struct.pack_into("<H", slot, at + self.member("struct hs_syntax_node", "flags"), flags)
            slot[at + 16:at + 20] = value
            values.append(at + 16)
        return slot, values

    def test_script_constants_by_their_type_cell(self):
        # hs.h enum hs_type: boolean 5, real 6, short integer 7, long 8, trigger volume 11, ai 17,
        # game difficulty (an enum) 32, object 37, object name 43; flags 9: a primitive constant
        nodes = [(5, 9, b"\x01\xff\xff\xff"), (6, 9, b"\x00\x00\x80\x3f"), (7, 9, b"\x05\x00\xff\xff"),
                 (8, 9, b"\x78\x56\x34\x12"), (11, 9, b"\x03\x00\x00\x00"), (17, 9, b"\x01\x00\x02\x00"),
                 (32, 9, b"\x02\x00\xff\xff"), (43, 9, b"\x07\x00\xff\xff"), (37, 9, b"\xff\xff\xff\xff")]
        slot, values = self.script_nodes(nodes)
        plan = map_stage.Plan(len(slot))
        map_stage.rule_syntax_data(plan, slot, self.layouts, 0, len(slot), 0)
        runs = {offset: width for offset, width, _ in plan.runs}
        boolean, real, short, long_, volume, ai, enum, name, obj = values
        self.assertNotIn(boolean, runs)                 # a boolean's byte, the rest as it is
        self.assertEqual(runs[real], 4)
        for value in (short, volume, enum, name):        # a short cell: its first two bytes
            self.assertEqual(runs[value], 2)
            self.assertNotIn(value + 2, runs)
        for value in (long_, ai, obj):
            self.assertEqual(runs[value], 4)
        self.assertEqual(plan.owner.count(1), len(slot))
        big = bytearray(slot)
        plan.apply(big)
        self.assertEqual(bytes(big[short:short + 4]), b"\x00\x05\xff\xff")
        self.assertEqual(bytes(big[volume:volume + 4]), b"\x00\x03\x00\x00")
        self.assertEqual(bytes(big[boolean:boolean + 4]), b"\x01\xff\xff\xff")
        self.assertEqual(bytes(big[long_:long_ + 4]), b"\x12\x34\x56\x78")

    def test_a_global_reference_is_a_long_whatever_its_type(self):
        # flags 13: primitive and a global's name, whose index the runtime reads as a long cast
        slot, values = self.script_nodes([(7, 13, b"\x04\x00\x00\x00"), (5, 13, b"\x02\x00\x00\x00")])
        plan = map_stage.Plan(len(slot))
        map_stage.rule_syntax_data(plan, slot, self.layouts, 0, len(slot), 0)
        runs = {offset: width for offset, width, _ in plan.runs}
        self.assertEqual((runs[values[0]], runs[values[1]]), (4, 4))

    def test_short_cell_types_are_the_runtimes(self):
        """the stager's short cells are the types hs_global_reconcile_read reads as shorts"""
        hs_h = (ROOT / "source/hs/hs.h").read_text(encoding="utf-8")
        body = hs_h[hs_h.index("enum hs_type"):hs_h.index("NUMBER_OF_HS_TYPES")]
        names = [line.strip().rstrip(",") for line in body.splitlines() if line.strip().startswith("_hs")]
        runtime = (ROOT / "source/hs/hs_runtime.c").read_text(encoding="utf-8")
        read = runtime[runtime.rindex("static void hs_global_reconcile_read(\n"):
                       runtime.rindex("static void hs_global_reconcile_write(\n")]
        shorts = set()
        for case in read.split("case ")[1:]:
            name = case.split(":")[0].strip()
            if name in names and "*(short *)&global->value" in case.split("break;")[0]:
                shorts.add(names.index(name))
        self.assertEqual(names.index("_hs_type_boolean"), map_stage.HS_TYPE_BOOLEAN)
        # (the six object names share one case in the reader's fall-through; their range is the enum's)
        names_range = set(range(names.index("_hs_type_object_name"), names.index("_hs_type_scenery_name") + 1))
        self.assertEqual(shorts | names_range, set(map_stage.HS_SHORT_CELL_TYPES))

    def recorded(self, version, unit_control_version, stream):
        slot = bytearray(64 + len(stream))
        slot[self.member("struct recorded_animation_definition", "version")] = version
        slot[self.member("struct recorded_animation_definition", "unit_control_data_version")] = unit_control_version
        slot[64:] = stream
        plan = map_stage.Plan(len(slot))
        map_stage.rule_recorded_animation(plan, slot, self.layouts, 64, len(stream), 0)
        return plan

    # unit control version 2 (recorded_animation.py UNIT_CONTROL_TABLES): two bytes, three shorts, eleven
    # floats, then a long
    CONTROL_V2 = b"\x01\x02" + bytes(6) + bytes(44) + bytes(4)

    def test_recorded_animation_current_codec(self):
        state = bytes(12)
        events = (bytes([4 << 2 | 1]) + b"\x34\x12" +                     # control flags, delta 1: a short
                  bytes([6 << 2 | 2, 7]) + bytes(8) +                      # throttle, a byte delta: two floats
                  bytes([15 << 2 | 3, 0x00, 0x01]) + b"\x01\x00\x02\x00" +  # a word delta: two shorts
                  bytes([1 << 2]))                                         # the end
        stream = self.CONTROL_V2 + state + events + b"\xee"
        plan = self.recorded(4, 2, stream)
        runs = {offset - 64: width for offset, width, _ in plan.runs}
        self.assertEqual([runs.get(at) for at in (0, 1, 2, 4, 6, 8, 48, 52)], [None, None, 2, 2, 2, 4, 4, 4])
        self.assertEqual([runs.get(at) for at in range(56, 68, 2)], [2] * 6)
        base = 68
        self.assertNotIn(base, runs)                     # a header byte
        self.assertEqual(runs[base + 1], 2)
        self.assertNotIn(base + 4, runs)                 # a byte delta
        self.assertEqual((runs[base + 5], runs[base + 9]), (4, 4))
        self.assertNotIn(base + 14, runs)                # a word delta, decoded byte by byte
        self.assertNotIn(base + 15, runs)
        self.assertEqual((runs[base + 16], runs[base + 18]), (2, 2))
        self.assertEqual(plan.span(64, len(stream)).count(1), len(stream))

    def test_recorded_animation_v1_headers_are_native(self):
        events = struct.pack("<hH", 5, 3) + b"\x09\x00" + struct.pack("<hH", 1, 0)
        plan = self.recorded(1, 1, b"\x01\x02" + bytes(6) + bytes(44) + events)
        runs = {offset - 64 - 52: width for offset, width, _ in plan.runs if offset >= 64 + 52}
        self.assertEqual([runs[at] for at in (0, 2, 4, 6, 8)], [2, 2, 2, 2, 2])

    def test_recorded_animation_refusals(self):
        with self.assertRaises(map_stage.StageError):
            self.recorded(4, 1, bytes(52) + bytes(12) + bytes([63 << 2]))   # an event the engine does not have
        with self.assertRaises(map_stage.StageError):
            self.recorded(4, 1, bytes(52) + bytes(12) + bytes([4 << 2]))    # no end event
        with self.assertRaises(map_stage.StageError):
            self.recorded(9, 1, bytes(64))                                  # a version it does not play


class UnplannedTest(unittest.TestCase):
    def test_runs_by_kind(self):
        # a range of 256 bytes at slot offset 1024; planned: [1024, 1040), [1100, 1104), [1200, 1250)
        data = bytearray(b"\x11" * 256)
        data[1110 - 1024:1150 - 1024] = bytes(40)
        plan = map_stage.Plan(256, 1024)
        plan.bytes_(1024, 16, "x")
        plan.bytes_(1100, 4, "x")
        plan.add(1200, 2, 25, "x")
        markers = [("tag_names", {1045}), ("buffer_payloads", {1270})]
        result = map_stage.classify_unplanned(plan, markers, data, 1024)
        self.assertEqual(result["tag_names"], 1100 - 1040)       # the run holding a name
        self.assertEqual(result["buffer_payloads"], 1280 - 1250)  # holding a payload's start
        self.assertEqual(result["other"], 1200 - 1104)           # [1104, 1200): not all zero
        self.assertEqual(result["other_runs"], 1)
        data[1104 - 1024:1200 - 1024] = bytes(96)
        self.assertEqual(map_stage.classify_unplanned(plan, markers, data, 1024)["zero"], 96)

    def test_padding_is_alignment(self):
        plan = map_stage.Plan(64)
        plan.bytes_(0, 29, "x")
        plan.bytes_(32, 30, "x")                          # 3 bytes to 32, 2 to the range's end
        result = map_stage.classify_unplanned(plan, [], b"\x01" * 64, 0)
        self.assertEqual((result["alignment"], result["other"]), (5, 0))

    def test_split_records(self):
        bsp = {"slot_offset": 0x1000, "bytes": 0x100}
        records = [(1, 0x10, 0, 0, 0), (3, 0x1004, 0x20, 0, 0), (2, 0x20, 4, 0, 0)]
        tags, own = map_stage.split_records(records, 0x800, bsp)
        self.assertEqual((tags, own), ([records[0], records[2]], [records[1]]))
        with self.assertRaises(map_stage.StageError):
            map_stage.split_records([(1, 0x900, 0, 0, 0)], 0x800, bsp)


class FileFormatTest(unittest.TestCase):
    def test_engine_crc(self):
        # crc_new (0xFFFFFFFF), then the reflected CRC-32 with no final inversion
        self.assertEqual(map_stage.engine_crc(b"123456789"), 0xCBF43926 ^ 0xFFFFFFFF)
        self.assertEqual(map_stage.engine_crc(b"6789", map_stage.engine_crc(b"12345")),
                         map_stage.engine_crc(b"123456789"))

    def segments(self):
        tags = bytes(range(256)) * 2
        bsp = bytes(range(64))
        return [{"kind": map_stage.SEGMENT_TAGS, "file_offset": 0x800, "bytes": len(tags), "slot_offset": 0,
                 "little": tags, "big": tags[::-1], "relocations": [0, 8]},
                {"kind": map_stage.SEGMENT_BSP, "file_offset": 0x900, "bytes": len(bsp), "slot_offset": 0x8000,
                 "little": bsp, "big": bsp[::-1], "relocations": [0x8004]},
                {"kind": map_stage.SEGMENT_BSP, "file_offset": 0xA00, "bytes": len(bsp), "slot_offset": 0x7F00,
                 "little": bsp, "big": bsp, "relocations": []}]

    def header(self):
        header = bytearray(0x800)
        struct.pack_into("<6I", header, 0, 0x68656164, 5, 0x3000, 0, 0x800, 0x100)
        struct.pack_into("<I", header, 0x7FC, 0x666F6F74)
        return bytes(header)

    def test_wmap_round_trip(self):
        for big in (True, False):
            data = map_stage.write_wmap(header_bytes=self.header(), segments=self.segments(), file_length=0x3000,
                                        big_endian=big)
            order = ">" if big else "<"
            self.assertEqual(data[:8], b"HWIMAP02")
            magic, count, header_size, header_at, length, file_bytes, header_crc, table_crc, version = \
                struct.unpack_from(order + "9I", data, 8)
            self.assertEqual((magic, count, header_size, length, file_bytes, version),
                             (0x01020304, 3, 0x800, 0x3000, len(data), 2))
            self.assertEqual(struct.unpack_from(order + "I", data, header_at + 4)[0], 5)
            self.assertEqual(map_stage.engine_crc(data[header_at:header_at + 0x800]), header_crc)
            table = bytearray(data[:0x40 + 32 * count])
            struct.pack_into(order + "I", table, 0x24, 0)
            self.assertEqual(map_stage.engine_crc(bytes(table)), table_crc)
            self.assertEqual(map_stage.table_digest(data), table_crc)
            for index, source in enumerate(self.segments()):
                kind, map_offset, size, slot_offset, data_at, relocations, relocation_at, crc = \
                    struct.unpack_from(order + "8I", data, 0x40 + 32 * index)
                image = source["big" if big else "little"]
                self.assertEqual((kind, map_offset, size, slot_offset, relocations),
                                 (source["kind"], source["file_offset"], len(image), source["slot_offset"],
                                  len(source["relocations"])))
                self.assertEqual(data[data_at:data_at + size], image)
                words = list(struct.unpack_from(f"{order}{relocations}I", data, relocation_at))
                self.assertEqual(words, source["relocations"])
                self.assertEqual(crc, map_stage.engine_crc(data[relocation_at:relocation_at + 4 * relocations],
                                                           map_stage.engine_crc(image)))

    def test_manifest_lines_replace_and_keep(self):
        first = map_stage.write_wmap(self.header(), self.segments(), 0x3000, True)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.txt"
            map_stage.manifest_update(path, "beta", first)
            map_stage.manifest_update(path, "alpha", first)
            segments = self.segments()
            segments[1]["big"] = bytes(64)
            second = map_stage.write_wmap(self.header(), segments, 0x3000, True)
            map_stage.manifest_update(path, "beta", second)
            raw = path.read_bytes()
            self.assertNotIn(b"\r", raw)
            lines = raw.decode("ascii").splitlines()
            self.assertEqual(lines[0], "HWIMAP-MANIFEST 2")
            self.assertEqual([line.split()[0] for line in lines[1:]], ["alpha", "beta"])
            name, version, size, digest, sha = lines[2].split()
            self.assertEqual((version, int(size), int(digest, 16)), ("2", len(second), map_stage.table_digest(second)))
            self.assertNotEqual(map_stage.table_digest(first), map_stage.table_digest(second))
            with self.assertRaises(map_stage.StageError):
                map_stage.manifest_update(path, "Bad Name", first)

    def test_a_manifest_of_another_version_is_replaced(self):
        data = map_stage.write_wmap(self.header(), self.segments(), 0x3000, False)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.le.txt"
            path.write_text("HWIMAP-MANIFEST 1\nold 1 5 00000000 " + "0" * 64 + "\n", encoding="ascii")
            map_stage.manifest_update(path, "map", data)
            self.assertEqual([line.split()[0] for line in path.read_text(encoding="ascii").splitlines()],
                             ["HWIMAP-MANIFEST", "map"])


if __name__ == "__main__":
    unittest.main()
