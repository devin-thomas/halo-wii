"""Synthetic numeric-only graph tests; no engine or commercial asset fixture."""
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

import inspect_widget_graph as widget


class Blob:
    """Independent LE fixture builder with bounded roots and invented names."""
    def __init__(self, count=1):
        self.count = count
        self.names = 36 + count * 32
        start = (self.names + count * 16 + 3) & ~3
        self.roots = [start + index * 1004 for index in range(count)]
        self.data = bytearray(start + count * 1004)
        struct.pack_into("<9I", self.data, 0, widget.BASE + 36, widget.NONE, 123,
                         count, 0, 0, 0, 0, 0x74616773)
        for index, root in enumerate(self.roots):
            self.data[self.names + index * 16:self.names + index * 16 + 7] = b"secret\0"
            struct.pack_into("<8I", self.data, 36 + index * 32, widget.GROUP,
                             widget.NONE, widget.NONE, self.datum(index),
                             widget.BASE + self.names + index * 16, widget.BASE + root, 0, 0)
            for offset, group in zip((56, 236, 252, 340, 356, 420), widget.REFERENCE_GROUPS):
                self.reference(root + offset, None, group)

    def datum(self, ordinal):
        return 0xABCD0000 + ordinal

    def reference(self, offset, target, group=widget.GROUP):
        struct.pack_into("<4I", self.data, offset, group,
                         0 if target is None else widget.BASE + self.names + target * 16,
                         0, widget.NONE if target is None else self.datum(target))

    def block(self, node, kind, count):
        stride = (36, 72, 34, 80, 80)[kind]
        offset = len(self.data)
        self.data.extend(bytes(count * stride))
        struct.pack_into("<iII", self.data, self.roots[node] + (72, 84, 96, 724, 992)[kind],
                         count, widget.BASE + offset, 0)
        for index in range(count):
            at = offset + index * stride
            if kind == 1:
                self.reference(at + 8, None)
                self.reference(at + 24, None, 0x736E6421)
            elif kind in (3, 4):
                self.reference(at, None)
        return offset

    def children(self, node, targets):
        offset = self.block(node, 4, len(targets))
        for index, target in enumerate(targets):
            self.reference(offset + index * 80, target)
        return offset

    def inspect(self, capacity=None, ordinal=0):
        return widget.inspect_bytes(bytes(self.data), ordinal, capacity or self.count)


class InspectorTests(unittest.TestCase):
    def rejects(self, blob, reason, capacity=None):
        with self.assertRaisesRegex(widget.InspectionError, reason):
            blob.inspect(capacity)

    def test_single_root_independent_digest_and_numeric_fields(self):
        blob = Blob()
        root = blob.roots[0]
        struct.pack_into("<4h", blob.data, root + 36, -12, 32767, -32768, 42)
        struct.pack_into("<4I", blob.data, root + 268, 0x80000000, 0x7FC00001, 0x3F800000, 0)
        result = blob.inspect()
        raw = bytes(blob.data[root:root + 1004])
        self.assertEqual(result["goldens"], {"root_datum": 0xABCD0000, "node_count": 1,
                         "serialized_bytes": 1004, "serialized_crc32": zlib.crc32(raw),
                         "serialized_sha256": hashlib.sha256(raw).hexdigest()})
        self.assertEqual(result["nodes"][0]["bounds"], [-12, 32767, -32768, 42])
        self.assertEqual(result["nodes"][0]["text_color_bits"][1], 0x7FC00001)
        self.assertNotIn("secret", json.dumps(result))

    def test_serialization_root_then_five_blocks_then_dfs_roots(self):
        blob = Blob(3)
        spans = [blob.block(0, kind, 1) for kind in range(5)]
        blob.reference(spans[4], 1)
        child = blob.children(1, [2])
        result = blob.inspect()
        expected = bytes(blob.data[blob.roots[0]:blob.roots[0] + 1004])
        expected += b"".join(bytes(blob.data[o:o + n]) for o, n in zip(spans, (36, 72, 34, 80, 80)))
        expected += bytes(blob.data[blob.roots[1]:blob.roots[1] + 1004])
        expected += bytes(blob.data[child:child + 80])
        expected += bytes(blob.data[blob.roots[2]:blob.roots[2] + 1004])
        self.assertEqual([n["datum"] for n in result["nodes"]], [blob.datum(i) for i in range(3)])
        self.assertEqual(result["goldens"]["serialized_sha256"], hashlib.sha256(expected).hexdigest())
        self.assertEqual(result["goldens"]["serialized_crc32"], zlib.crc32(expected))
        self.assertEqual(result["goldens"]["serialized_bytes"], len(expected))

    def test_all_block_limits_and_over_limit(self):
        for kind, maximum in enumerate((64, 32, 32, 32, 32)):
            with self.subTest(kind=kind):
                blob = Blob()
                blob.block(0, kind, maximum)
                self.assertEqual(blob.inspect()["nodes"][0]["blocks"][kind]["count"], maximum)
                struct.pack_into("<i", blob.data, blob.roots[0] + widget.BLOCK_OFFSETS[kind], maximum + 1)
                self.rejects(blob, "block count")

    def test_negative_block_count(self):
        blob = Blob()
        struct.pack_into("<i", blob.data, blob.roots[0] + 72, -1)
        self.rejects(blob, "block count")

    def test_zero_blocks_and_none_ignore_meaningless_pointers(self):
        blob = Blob()
        for field in widget.BLOCK_OFFSETS:
            struct.pack_into("<III", blob.data, blob.roots[0] + field, 0, 0xFFFFFFFF, 0xABABABAB)
        for field in widget.REFERENCE_OFFSETS:
            struct.pack_into("<4I", blob.data, blob.roots[0] + field, 17, 0xFFFFFFFF, 0xFFFFFFFF, widget.NONE)
        self.assertEqual(blob.inspect()["goldens"]["node_count"], 1)

    def test_nonnull_name_length_is_opaque_and_names_need_not_match(self):
        blob = Blob(2)
        blob.reference(blob.roots[0] + 420, 1)
        struct.pack_into("<II", blob.data, blob.roots[0] + 424, widget.BASE + blob.names, 0xFFFFFFFF)
        result = blob.inspect()
        self.assertEqual(result["nodes"][0]["references"][5]["name_length_word"], 0xFFFFFFFF)
        self.assertEqual(result["goldens"]["node_count"], 1)

    def test_event_and_conditional_validate_but_do_not_traverse(self):
        blob = Blob(2)
        event = blob.block(0, 1, 1)
        conditional = blob.block(0, 3, 1)
        blob.reference(event + 8, 1)
        blob.reference(conditional, 1)
        struct.pack_into("<h", blob.data, blob.roots[1], -1)
        self.assertEqual(blob.inspect()["goldens"]["node_count"], 1)
        struct.pack_into("<I", blob.data, conditional + 12, blob.datum(1) ^ 0x10000)
        self.rejects(blob, "full datum")

    def test_column_extended_edge_after_children(self):
        blob = Blob(3)
        struct.pack_into("<h", blob.data, blob.roots[0], 3)
        blob.children(0, [1])
        blob.reference(blob.roots[0] + 420, 2)
        self.assertEqual([n["datum"] for n in blob.inspect()["nodes"]], [blob.datum(i) for i in range(3)])

    def test_duplicate_completed_child_counted_once(self):
        blob = Blob(2)
        blob.children(0, [1, 1, None])
        self.assertEqual(blob.inspect()["goldens"]["node_count"], 2)

    def test_self_cycle_and_two_node_cycle(self):
        for count in (1, 2):
            blob = Blob(count)
            for index in range(count):
                blob.children(index, [(index + 1) % count])
            self.rejects(blob, "cycle")

    def test_depth_32_edges_allowed_33_rejected(self):
        for count in (33, 34):
            blob = Blob(count)
            for index in range(count - 1):
                blob.children(index, [index + 1])
            if count == 33:
                self.assertEqual(blob.inspect()["goldens"]["node_count"], 33)
            else:
                self.rejects(blob, "depth")

    def test_completed_subtree_height_checks_longer_dag_path(self):
        blob = Blob(35)
        blob.children(0, [1, 33])
        for index in range(1, 32):
            blob.children(index, [index + 1])
        blob.children(33, [34])
        blob.children(34, [1])
        self.rejects(blob, "depth")

    def test_capacity_exhaustion(self):
        blob = Blob(2)
        blob.children(0, [1])
        self.rejects(blob, "capacity", capacity=1)

    def test_argument_limits(self):
        for ordinal, capacity in ((-1, 1), (1, 1), (0, 0), (0, 65536), (True, 1), (0, True)):
            with self.subTest(ordinal=ordinal, capacity=capacity):
                with self.assertRaises(widget.InspectionError):
                    widget.inspect_bytes(bytes(Blob().data), ordinal, capacity)

    def test_header_size_signature_and_signed_counts(self):
        for size in (0, 35, widget.MAX_BYTES + 1):
            with self.assertRaises(widget.InspectionError):
                widget.inspect_bytes(bytes(size), 0, 1)
        for offset, value in ((32, 0), (12, 0), (12, 65536), (12, 0xFFFFFFFF),
                              (16, 0xFFFFFFFF), (24, 0xFFFFFFFF)):
            blob = Blob()
            struct.pack_into("<I", blob.data, offset, value)
            self.rejects(blob, "header")

    def test_header_descriptors_and_table_spans(self):
        for count_at, pointer_at, stride in ((12, 0, 32), (16, 20, 12), (24, 28, 12)):
            blob = Blob()
            struct.pack_into("<I", blob.data, count_at, 1)
            struct.pack_into("<I", blob.data, pointer_at, widget.BASE + len(blob.data) - stride + 1)
            self.rejects(blob, "span")

    def test_instance_and_reference_full_identity_group_and_ordinal(self):
        for word, value, reason in ((0, 12, "reference group"), (12, 0xABCE0001, "full datum"),
                                    (12, 0xABCD0002, "ordinal")):
            blob = Blob(2)
            blob.reference(blob.roots[0] + 420, 1)
            struct.pack_into("<I", blob.data, blob.roots[0] + 420 + word, value)
            self.rejects(blob, reason)
        blob = Blob()
        struct.pack_into("<I", blob.data, 36, 0x6269746D)
        self.rejects(blob, "instance group")
        struct.pack_into("<I", blob.data, 48, 0xABCD0001)
        self.rejects(blob, "ordinal")

    def test_root_and_block_span_truncation(self):
        blob = Blob()
        del blob.data[-1:]
        self.rejects(blob, "span")
        blob = Blob()
        blob.block(0, 0, 1)
        del blob.data[-1:]
        self.rejects(blob, "span")

    def test_visited_span_overlap_within_and_across_nodes(self):
        blob = Blob()
        struct.pack_into("<iII", blob.data, blob.roots[0] + 72, 1, widget.BASE + blob.roots[0], 0)
        self.rejects(blob, "overlap")
        blob = Blob(2)
        blob.children(0, [1])
        struct.pack_into("<I", blob.data, 36 + 32 + 20, widget.BASE + blob.roots[0])
        self.rejects(blob, "overlap")

    def test_root_enum_bounds(self):
        for field, limit in ((0, 7), (2, 5), (284, 3)):
            for value in (-1, limit):
                blob = Blob()
                struct.pack_into("<h", blob.data, blob.roots[0] + field, value)
                self.rejects(blob, "enum")

    def test_fixed_strings_root_script_search_and_child(self):
        for kind, relative in ((None, 4), (1, 40), (2, 0), (3, 16), (4, 16)):
            blob = Blob()
            offset = blob.roots[0] if kind is None else blob.block(0, kind, 1)
            blob.data[offset + relative:offset + relative + 32] = b"x" * 32
            self.rejects(blob, "string")

    def test_addressed_name_bounds_and_termination(self):
        for pointer in (widget.BASE - 1, widget.BASE + 100000):
            blob = Blob()
            struct.pack_into("<I", blob.data, 36 + 16, pointer)
            self.rejects(blob, "span")
        blob = Blob()
        blob.data.append(120)
        struct.pack_into("<I", blob.data, 36 + 16, widget.BASE + len(blob.data) - 1)
        self.rejects(blob, "unterminated")

    def test_work_and_name_scan_budget(self):
        with patch.object(widget, "MAX_WORK", 1):
            self.rejects(Blob(), "workload")
        with patch.object(widget, "MAX_NAME_SCAN", 3):
            self.rejects(Blob(), "scan limit")

    def test_external_path_rejects_checkout(self):
        with self.assertRaisesRegex(widget.InspectionError, "outside"):
            widget.external_path(widget.CHECKOUT / ".local" / "blob")

    def test_atomic_exclusive_output_preserves_existing_and_cleans_temp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.json"
            widget.write_exclusive(path, {"numeric": 17})
            original = path.read_bytes()
            with self.assertRaises(FileExistsError):
                widget.write_exclusive(path, {"numeric": 42})
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual([p.name for p in Path(directory).iterdir()], ["result.json"])

    def test_atomic_link_failure_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.json"
            with patch.object(widget.os, "link", side_effect=OSError(5, "private-path")):
                with self.assertRaises(OSError):
                    widget.write_exclusive(path, {})
            self.assertFalse(list(Path(directory).iterdir()))

    def test_cli_numeric_only_no_private_path_and_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private-asset-name.bin"
            output = Path(directory) / "result.json"
            path.write_bytes(Blob().data)
            args = [str(path), "--root-ordinal", "0", "--node-capacity", "1", "--output", str(output)]
            stdout, stderr = io.StringIO(), io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                self.assertEqual(widget.main(args), 0)
                self.assertEqual(widget.main(args), 1)
            report = output.read_text()
            self.assertNotIn(directory, report + stdout.getvalue() + stderr.getvalue())
            self.assertNotIn("private-asset-name", report)
            self.assertNotIn("secret", report)
            self.assertEqual(json.loads(report)["goldens"]["node_count"], 1)

    def test_cli_missing_and_malformed_input_has_no_output_or_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path, output = Path(directory) / "private.bin", Path(directory) / "out.json"
            args = [str(path), "--root-ordinal", "0", "--node-capacity", "1", "--output", str(output)]
            for content in (None, b"bad"):
                if content is not None:
                    path.write_bytes(content)
                stderr = io.StringIO()
                with redirect_stderr(stderr):
                    self.assertEqual(widget.main(args), 1)
                self.assertNotIn(directory, stderr.getvalue())
                self.assertFalse(output.exists())

    def test_file_mutation_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "blob.bin"
            path.write_bytes(Blob().data)
            actual = widget.os.fstat
            calls = 0

            def changed(fd):
                nonlocal calls
                result = actual(fd)
                calls += 1
                if calls == 2:
                    from types import SimpleNamespace
                    return SimpleNamespace(st_dev=result.st_dev, st_ino=result.st_ino,
                                           st_size=result.st_size, st_mtime_ns=result.st_mtime_ns + 1,
                                           st_ctime_ns=result.st_ctime_ns)
                return result

            with patch.object(widget.os, "fstat", side_effect=changed):
                with self.assertRaisesRegex(widget.InspectionError, "changed"):
                    widget.inspect_file(path, 0, 1)

    def test_path_and_descriptor_ctime_semantics_may_differ(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "blob.bin"
            path.write_bytes(Blob().data)
            actual = widget.os.fstat

            def descriptor(fd):
                result = actual(fd)
                return SimpleNamespace(st_dev=result.st_dev, st_ino=result.st_ino,
                                       st_size=result.st_size, st_mtime_ns=result.st_mtime_ns,
                                       st_ctime_ns=result.st_ctime_ns + 100000)

            with patch.object(widget.os, "fstat", side_effect=descriptor):
                self.assertEqual(widget.inspect_file(path, 0, 1)["goldens"]["node_count"], 1)


if __name__ == "__main__":
    unittest.main()
