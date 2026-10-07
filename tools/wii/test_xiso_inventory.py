"""Synthetic read-only inventory tests; no commercial image or engine proof."""
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import xiso_inventory as xiso


SECTOR = 2048
DESCRIPTOR = 0x10000
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ROOT = 40


def record(name, sector=0, size=0, *, directory=False, left=0, right=0):
    if isinstance(name, str):
        name = name.encode("ascii")
    value = struct.pack("<HHIIBB", left, right, sector, size, 0x10 if directory else 0, len(name)) + name
    return value + bytes((-len(value)) % 4)


def table(rows):
    """Independent linear right-subtree tree, pointers in four-byte units."""
    values = [bytearray(record(*args, **kwargs)) for args, kwargs in rows]
    offset = 0
    for index, value in enumerate(values[:-1]):
        offset += len(value)
        struct.pack_into("<H", value, 2, offset // 4)
    return b"".join(values)


def descriptor(root_size, root_sector=ROOT):
    value = bytearray(SECTOR)
    value[:20] = MAGIC
    value[0x7EC:] = MAGIC
    struct.pack_into("<II", value, 20, root_sector, root_size)
    return value


def image(root_table=b"", chunks=(), size=72 * SECTOR):
    value = bytearray(size)
    value[DESCRIPTOR:DESCRIPTOR + SECTOR] = descriptor(len(root_table))
    value[ROOT * SECTOR:ROOT * SECTOR + len(root_table)] = root_table
    for offset, content in chunks:
        if offset + len(content) > size:
            raise ValueError("Synthetic image chunk exceeds storage")
        value[offset:offset + len(content)] = content
    return value


def map_header(*, version=5, length=4096, offset=2048, tag_size=2048,
               name=b"bloodgulch", build=b"01.10.12.2276"):
    value = bytearray(SECTOR)
    value[:4], value[0x7FC:] = b"daeh", b"toof"
    struct.pack_into("<ii", value, 4, version, length)
    struct.pack_into("<ii", value, 0x10, offset, tag_size)
    value[0x20:0x20 + len(name)] = name
    value[0x40:0x40 + len(build)] = build
    return value


class SparseImage(io.RawIOBase):
    """Truthful virtual zero-filled image without a hundreds-of-MB allocation."""
    def __init__(self, size, chunks):
        self.size, self.chunks, self.position = size, chunks, 0

    def seek(self, offset, whence=io.SEEK_SET):
        target = offset if whence == io.SEEK_SET else self.position + offset if whence == io.SEEK_CUR else self.size + offset
        if target < 0:
            raise ValueError("negative seek")
        self.position = target
        return target

    def tell(self):
        return self.position

    def read(self, size=-1):
        count = max(0, self.size - self.position)
        if size >= 0:
            count = min(count, size)
        start, end = self.position, self.position + count
        value = bytearray(count)
        for offset, content in self.chunks:
            lower, upper = max(start, offset), min(end, offset + len(content))
            if lower < upper:
                value[lower - start:upper - start] = content[lower - offset:upper - offset]
        self.position = end
        return bytes(value)


class InventoryTests(unittest.TestCase):
    def inspect(self, data):
        return xiso.inspect(io.BytesIO(data), len(data))

    def rejects(self, data):
        with self.assertRaises(xiso.InventoryError):
            self.inspect(data)

    def one_file(self, name="ordinary.bin", size=4, sector=50, content=b"DATA"):
        return image(record(name, sector, size), [(sector * SECTOR, content)] if content else [])

    def map_row(self, header, *, stored_size=4096):
        content = bytes(header) + bytes(max(0, stored_size - len(header)))
        data = self.one_file("level.map", stored_size, content=content)
        result = self.inspect(data)
        self.assertEqual(result["counts"]["map_files"], 1)
        self.assertEqual(len(result["maps"]), 1)
        return result["maps"][0]

    def test_valid_nested_listing_sorted_independent_offsets_counts(self):
        nested = table([(("level.map", 52, 4096), {}), (("zero.bin", 0, 0), {})])
        root = table([(("Z.bin", 50, 4), {}), (("maps", 45, len(nested)), {"directory": True}),
                      (("intro.bik", 55, 3), {})])
        data = image(root, [(45 * SECTOR, nested), (50 * SECTOR, b"DATA"),
                            (52 * SECTOR, map_header()), (55 * SECTOR, b"BIK")])
        before = bytes(data)
        result = self.inspect(data)
        self.assertEqual(bytes(data), before)
        self.assertEqual({key: result["volume"][key] for key in ("partition_offset", "descriptor_offset", "root_sector", "root_size")},
                         {"partition_offset": 0, "descriptor_offset": DESCRIPTOR, "root_sector": ROOT, "root_size": len(root)})
        self.assertEqual(result["volume"]["descriptor_sha256"], hashlib.sha256(descriptor(len(root))).hexdigest())
        paths = [row["path"] for row in result["entries"]]
        self.assertEqual(paths, ["intro.bik", "maps", "maps/level.map", "maps/zero.bin", "Z.bin"])
        rows = {row["path"]: row for row in result["entries"]}
        self.assertEqual(rows["maps"]["kind"], "directory")
        self.assertEqual(rows["maps/level.map"]["offset"], 52 * SECTOR)
        self.assertEqual(rows["maps/level.map"]["sector"], 52)
        self.assertEqual(rows["maps/level.map"]["size"], 4096)
        self.assertEqual(result["counts"], {"files": 4, "directories": 1, "map_files": 1,
                                            "movies": 1, "file_bytes": 4103})
        self.assertTrue(result["maps"][0]["header_identity_supported"])
        self.assertEqual(result["maps"][0]["issues"], [])
        self.assertNotIn("image_sha256", result)

    def test_empty_root_zero_size(self):
        data = image()
        struct.pack_into("<II", data, DESCRIPTOR + 20, 0, 0)
        result = self.inspect(data)
        self.assertEqual(result["entries"], [])
        self.assertEqual(result["counts"]["files"], 0)

    def test_valid_left_and_right_subtrees_and_read_only_stream(self):
        left, right = record("Alpha", 51, 3), record("zebra", 52, 4)
        root = record("middle", 50, 2)
        root = record("middle", 50, 2, left=len(root) // 4, right=(len(root) + len(left)) // 4)
        data = image(root + left + right)

        class ReadOnlyStream(io.BytesIO):
            def write(self, value):
                raise AssertionError("Inventory must never write image bytes")

        stream = ReadOnlyStream(data)
        result = xiso.inspect(stream, len(data))
        self.assertEqual([row["path"] for row in result["entries"]], ["Alpha", "middle", "zebra"])
        self.assertEqual(stream.getvalue(), bytes(data))

    def test_all_ff_empty_table(self):
        self.assertEqual(self.inspect(image(b"\xff" * 64))["entries"], [])

    def test_empty_child_directory_and_zero_files_share_zero_sector(self):
        root = table([(("empty", 0, 0), {"directory": True}), (("a", 0, 0), {}), (("b", 0, 0), {})])
        result = self.inspect(image(root))
        self.assertEqual(result["counts"]["directories"], 1)
        self.assertEqual(result["counts"]["files"], 2)
        self.assertEqual(result["counts"]["file_bytes"], 0)

    def test_supported_partition_offsets_without_large_allocation(self):
        for partition in (0, 0xFD90000, 0x2080000, 0x18300000):
            with self.subTest(partition=hex(partition)):
                root = record("one.bin", 50, 4)
                stream = SparseImage(partition + 72 * SECTOR,
                                     [(partition + DESCRIPTOR, descriptor(len(root))),
                                      (partition + ROOT * SECTOR, root), (partition + 50 * SECTOR, b"DATA")])
                result = xiso.inspect(stream, stream.size)
                self.assertEqual(result["volume"]["partition_offset"], partition)
                self.assertEqual(result["entries"][0]["offset"], partition + 50 * SECTOR)

    def test_missing_and_short_descriptor(self):
        for data in (b"", bytes(DESCRIPTOR + 100), bytes(72 * SECTOR)):
            with self.subTest(length=len(data)):
                self.rejects(data)

    def test_descriptor_magic_requires_both_signatures(self):
        for where in (0, 0x7EC):
            with self.subTest(where=where):
                data = image()
                data[DESCRIPTOR + where] ^= 1
                self.rejects(data)

    def test_ambiguous_valid_descriptors_rejected(self):
        partition = 0x2080000
        stream = SparseImage(partition + 72 * SECTOR,
                             [(DESCRIPTOR, descriptor(0, 0)), (partition + DESCRIPTOR, descriptor(0, 0))])
        with self.assertRaises(xiso.InventoryError):
            xiso.inspect(stream, stream.size)

    def test_root_span_outside_image(self):
        for sector, size in ((72, 1), (0xFFFFFFFF, 4), (ROOT, 0xFFFFFFFF)):
            with self.subTest(sector=sector, size=size):
                data = image()
                struct.pack_into("<II", data, DESCRIPTOR + 20, sector, size)
                self.rejects(data)

    def test_directory_table_over_four_megabytes_rejected(self):
        limit = 4 * 1024 * 1024
        stream = SparseImage(ROOT * SECTOR + limit + 1, [(DESCRIPTOR, descriptor(limit + 1))])
        with self.assertRaises(xiso.InventoryError):
            xiso.inspect(stream, stream.size)

    def test_truncated_entry_record_and_name(self):
        for root in (b"\x00" * 13, struct.pack("<HHIIBB", 0, 0, 50, 1, 0, 12) + b"short", record("a")[:-1]):
            with self.subTest(root=root):
                self.rejects(image(root))

    def test_invalid_component_names(self):
        for name in (b"", b".", b"..", b"a/b", b"a\\b", b"/absolute", b"a:b", b"nul\x00x", b"line\nx", b"\xff"):
            with self.subTest(name=name):
                self.rejects(image(record(name, 50, 1)))

    def test_duplicate_casefold_paths_rejected(self):
        self.rejects(image(table([(("Case.bin", 50, 1), {}), (("case.BIN", 51, 1), {})])))

    def test_duplicate_directory_paths_rejected(self):
        self.rejects(image(table([(("maps", 45, 0), {"directory": True}),
                                 (("MAPS", 46, 0), {"directory": True})])))

    def test_subtree_pointer_outside_table(self):
        for link in (0xFFFE, 0xFFFF, 100):
            with self.subTest(link=link):
                self.rejects(image(record("file", 50, 1, right=link)))

    def test_subtree_cycle_and_repeated_node(self):
        first = bytearray(record("a", 50, 1))
        second = record("b", 51, 1, right=len(first) // 4)
        struct.pack_into("<H", first, 2, len(first) // 4)
        self.rejects(image(bytes(first) + second))
        struct.pack_into("<H", first, 0, len(first) // 4)
        second = record("b", 51, 1)
        self.rejects(image(bytes(first) + second))

    def test_overlapping_directory_records_rejected(self):
        self.rejects(image(record("long_record_name", 50, 1, right=4)))

    def test_mixed_ffff_marker_not_empty_padding(self):
        self.rejects(image(b"\xff\xff" + b"\x00" * 30))

    def test_file_span_outside_image(self):
        for sector, size in ((72, 1), (71, SECTOR + 1), (0xFFFFFFFF, 1), (50, 0xFFFFFFFF)):
            with self.subTest(sector=sector, size=size):
                self.rejects(image(record("file", sector, size)))

    def test_nonempty_file_overlap_rejected_and_touching_spans_accepted(self):
        self.rejects(image(table([(("a", 50, SECTOR + 1), {}), (("b", 51, 1), {})])))
        result = self.inspect(image(table([(("a", 50, SECTOR), {}), (("b", 51, 1), {})])))
        self.assertEqual(result["counts"]["files"], 2)

    def test_file_directory_storage_overlap(self):
        child = record("inside", 50, 1)
        root = table([(("dir", 45, len(child)), {"directory": True}), (("alias", 45, 1), {})])
        self.rejects(image(root, [(45 * SECTOR, child)]))
        self.rejects(image(record("root_alias", ROOT, 1)))

    def test_reused_and_cyclic_directory_spans(self):
        child = record("inside", 50, 1)
        root = table([(("dir_a", 45, len(child)), {"directory": True}),
                      (("dir_b", 45, len(child)), {"directory": True})])
        self.rejects(image(root, [(45 * SECTOR, child)]))
        root = record("loop", ROOT, 20, directory=True)
        self.assertEqual(len(root), 20)
        self.rejects(image(root))

    def test_directory_depth_exact_limit_and_one_over(self):
        for depth in (128, 129):
            with self.subTest(depth=depth):
                chunks = []
                for level in range(depth):
                    chunks.append(((ROOT + level) * SECTOR,
                                   record(f"d{level:03}", ROOT + level + 1, 20, directory=True)))
                chunks.append(((ROOT + depth) * SECTOR, record("tail", 0, 0)))
                data = image(chunks[0][1], chunks[1:], size=(ROOT + depth + 2) * SECTOR)
                if depth == 128:
                    result = self.inspect(data)
                    self.assertEqual(result["counts"]["directories"], depth)
                    self.assertEqual(result["counts"]["files"], 1)
                else:
                    self.rejects(data)

    def test_stream_truncation_despite_claimed_size(self):
        data = self.one_file()
        with self.assertRaises(xiso.InventoryError):
            xiso.inspect(io.BytesIO(data[:ROOT * SECTOR + 3]), len(data))

    def test_reader_span_bounds_and_exact_end_empty_read(self):
        reader = xiso.Reader(io.BytesIO(b"abcd"), 4)
        self.assertEqual(reader.read(1, 3, "valid"), b"bcd")
        self.assertEqual(reader.read(4, 0, "empty"), b"")
        for offset, size in ((-1, 1), (0, -1), (5, 0), (4, 1), (1, 4)):
            with self.subTest(offset=offset, size=size):
                with self.assertRaises(xiso.InventoryError):
                    reader.read(offset, size, "invalid")
        with self.assertRaises(xiso.InventoryError):
            xiso.Reader(io.BytesIO(), -1)

    def test_total_entry_budget_across_directories_exact_and_one_over(self):
        child = table([(("one", 0, 0), {}), (("two", 0, 0), {})])
        root = record("dir", 45, len(child), directory=True)
        data = image(root, [(45 * SECTOR, child)])
        with patch.object(xiso, "MAX_ENTRIES", 3):
            self.assertEqual(len(self.inspect(data)["entries"]), 3)
        with patch.object(xiso, "MAX_ENTRIES", 2):
            self.rejects(data)

    def test_total_directory_byte_budget_exact_and_one_over(self):
        child = record("tail", 0, 0)
        root = record("dir", 45, len(child), directory=True)
        data = image(root, [(45 * SECTOR, child)])
        with patch.object(xiso, "MAX_TOTAL_DIRECTORY_BYTES", len(root) + len(child)):
            self.assertEqual(self.inspect(data)["counts"]["files"], 1)
        with patch.object(xiso, "MAX_TOTAL_DIRECTORY_BYTES", len(root) + len(child) - 1):
            self.rejects(data)

    def test_path_byte_budget_exact_and_one_over(self):
        child = record("tail", 0, 0)
        root = record("maps", 45, len(child), directory=True)
        data = image(root, [(45 * SECTOR, child)])
        with patch.object(xiso, "MAX_PATH_BYTES", 9):
            self.assertIn("maps/tail", [row["path"] for row in self.inspect(data)["entries"]])
        with patch.object(xiso, "MAX_PATH_BYTES", 8):
            self.rejects(data)

    def test_aggregate_path_byte_budget_exact_and_one_over(self):
        root = table([(("alpha", 0, 0), {}), (("beta", 0, 0), {})])
        data = image(root)
        with patch.object(xiso, "MAX_TOTAL_PATH_BYTES", 9):
            self.assertEqual(self.inspect(data)["counts"]["files"], 2)
        with patch.object(xiso, "MAX_TOTAL_PATH_BYTES", 8):
            self.rejects(data)

    def test_partially_overlapping_directory_tables(self):
        child = record("inside", 52, 1)
        root = bytearray(4096)
        root[:20] = record("dir", ROOT + 1, len(child), directory=True)
        self.rejects(image(root, [((ROOT + 1) * SECTOR, child)]))

    def test_valid_map_header_identity_only(self):
        row = self.map_row(map_header())
        self.assertTrue(row["header_identity_supported"])
        self.assertEqual(row["issues"], [])
        self.assertEqual((row["version"], row["name"], row["build"]), (5, "bloodgulch", "01.10.12.2276"))
        self.assertEqual((row["declared_file_length"], row["tag_data_offset"], row["tag_data_size"], row["stored_file_size"]),
                         (4096, 2048, 2048, 4096))
        self.assertTrue(row["stored_length_matches_declared"])
        self.assertNotIn("fully_compatible", row)

    def test_map_declared_stored_length_difference_observed_not_rejected(self):
        row = self.map_row(map_header(), stored_size=2048)
        self.assertTrue(row["header_identity_supported"])
        self.assertFalse(row["stored_length_matches_declared"])
        self.assertEqual(row["stored_file_size"], 2048)

    def test_map_latin1_name_build_and_unconstrained_reserved_words(self):
        header = map_header(name=b"ma\xffp", build=b"b\x80uild")
        struct.pack_into("<I", header, 0xC, 0xFFFFFFFF)
        struct.pack_into("<I", header, 0x60, 0xFFFFFFFF)
        row = self.map_row(header)
        self.assertTrue(row["header_identity_supported"])
        self.assertEqual((row["name"], row["build"]), ("ma\u00ffp", "b\u0080uild"))
        encoded = json.dumps(row, ensure_ascii=True)
        self.assertIn("\\u00ff", encoded)
        self.assertIn("\\u0080", encoded)

    def test_map_short_header_reports_unsupported_without_structural_error(self):
        for size in (0, 3, 2047):
            with self.subTest(size=size):
                result = self.inspect(self.one_file("short.map", size, content=bytes(size)))
                self.assertFalse(result["maps"][0]["header_identity_supported"])
                self.assertTrue(result["maps"][0]["issues"])

    def test_map_bad_signatures_version_and_unterminated_strings(self):
        edits = [(0, b"head"), (0x7FC, b"foot"), (4, struct.pack("<i", 609)),
                 (0x20, b"N" * 32), (0x40, b"B" * 32)]
        for offset, data in edits:
            with self.subTest(offset=offset, data=data):
                header = map_header()
                header[offset:offset + len(data)] = data
                row = self.map_row(header)
                self.assertFalse(row["header_identity_supported"])
                self.assertTrue(row["issues"])

    def test_map_signed_lengths_and_tag_region_bounds(self):
        changes = [{"length": -1}, {"length": 2047}, {"length": 0x11600001},
                   {"offset": -1}, {"tag_size": -1}, {"tag_size": 0x1600001},
                   {"offset": 2049}, {"offset": 0x7FFFFFFF, "tag_size": 0x7FFFFFFF}]
        for change in changes:
            with self.subTest(change=change):
                row = self.map_row(map_header(**change))
                self.assertFalse(row["header_identity_supported"])
                self.assertTrue(row["issues"])

    def test_map_tag_region_exact_end_and_zero_size(self):
        for change in ({"offset": 4096, "tag_size": 0}, {"offset": 0, "tag_size": 0},
                       {"offset": 2048, "tag_size": 2048}):
            with self.subTest(change=change):
                self.assertTrue(self.map_row(map_header(**change))["header_identity_supported"])

    def test_map_string_last_byte_terminator_empty_and_unknown_build(self):
        for name, build in ((b"n" * 31, b"b" * 31), (b"", b""), (b"different_from_filename", b"unlisted-build")):
            with self.subTest(name=name, build=build):
                row = self.map_row(map_header(name=name, build=build))
                self.assertTrue(row["header_identity_supported"])
                self.assertEqual((row["name"], row["build"]), (name.decode("ascii"), build.decode("ascii")))

    def test_inventory_rejects_changed_source_metadata(self):
        data = self.one_file()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "synthetic.iso"
            path.write_bytes(data)
            before = path.stat()
            changed = SimpleNamespace(st_size=before.st_size, st_mtime_ns=before.st_mtime_ns + 1)
            with patch.object(xiso.os, "fstat", side_effect=[before, changed]):
                with self.assertRaises(xiso.InventoryError):
                    xiso.inventory(path)
            self.assertEqual(path.read_bytes(), data)

    def test_inventory_hash_and_read_only_file(self):
        data = self.one_file()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "synthetic.iso"
            path.write_bytes(data)
            before = path.stat().st_mtime_ns
            result = xiso.inventory(path)
            self.assertEqual(result["image_sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(result["image_size"], len(data))
            self.assertEqual(path.read_bytes(), data)
            self.assertEqual(path.stat().st_mtime_ns, before)
            self.assertEqual(list(Path(temporary).iterdir()), [path])

    def cli(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", ["xiso_inventory.py", *map(str, arguments)]), redirect_stdout(stdout), redirect_stderr(stderr):
            code = xiso.main()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_cli_stdout_json_metadata_no_raw_payload_or_latin1_bytes(self):
        header = map_header(name=b"ma\xffp")
        data = self.one_file("level.map", 4096, content=header + bytes(2048))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "synthetic.iso"
            path.write_bytes(data)
            code, output, error = self.cli(path)
            self.assertEqual((code, error), (0, ""))
            result = json.loads(output)
            self.assertEqual(result["maps"][0]["name"], "ma\u00ffp")
            self.assertEqual(result["asset_bytes_extracted"], 0)
            self.assertFalse(result["production_integrated"])
            self.assertIn("\\u00ff", output)
            self.assertNotIn("\u00ff", output)
            self.assertNotIn("daeh", output)
            self.assertEqual(path.read_bytes(), data)

    def test_cli_no_maps_and_unsupported_maps_are_blocked_exit_two(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "synthetic.iso"
            for data in (self.one_file(), self.one_file("level.map", 3, content=b"bad")):
                with self.subTest(size=len(data)):
                    path.write_bytes(data)
                    code, output, error = self.cli(path)
                    self.assertEqual((code, error), (2, ""))
                    self.assertEqual(json.loads(output)["map_header_identity"], "blocked")

    def test_cli_structural_error_has_no_partial_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.iso"
            path.write_bytes(b"bad")
            code, output, error = self.cli(path)
            self.assertEqual((code, output), (1, ""))
            self.assertIn("XISO inventory failed", error)

    def test_cli_exclusive_output_preserves_input_and_existing_report(self):
        data = self.one_file("level.map", 4096, content=map_header() + bytes(2048))
        with tempfile.TemporaryDirectory() as temporary:
            path, report = Path(temporary) / "synthetic.iso", Path(temporary) / "report.json"
            path.write_bytes(data)
            code, output, error = self.cli(path, "--output", report)
            self.assertEqual((code, output, error), (0, "", ""))
            self.assertEqual(json.loads(report.read_text())["image_sha256"], hashlib.sha256(data).hexdigest())
            original_report = report.read_bytes()
            for destination in (path, report):
                with self.subTest(destination=destination):
                    code, output, error = self.cli(path, "--output", destination)
                    self.assertEqual((code, output), (1, ""))
                    self.assertIn("XISO inventory failed", error)
                    self.assertEqual(path.read_bytes(), data)
                    self.assertEqual(report.read_bytes(), original_report)


if __name__ == "__main__":
    unittest.main()
