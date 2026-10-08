"""Synthetic streaming/index tests; no commercial map or runtime proof."""
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
import zlib

import inspect_cache_residency as cache


BASE = 0x803A6000
SCENARIO = 0x73636E72
BSP = 0x73627370
ROOT = 128
BSP_TABLE = 1648


def tag_bytes(*, size=2048, bsp_count=1):
    """Author tiny invented metadata using independently specified ABI offsets."""
    tags = bytearray(size)
    struct.pack_into("<9I", tags, 0, BASE + 36, 0x10000, 0x12345678,
                     2, 0, 0, 0, 0, 0x74616773)
    struct.pack_into("<8I", tags, 36, SCENARIO, 0, 0, 0x10000,
                     BASE + 100, BASE + ROOT, 0, 0)
    struct.pack_into("<8I", tags, 68, BSP, 0, 0, 0x20001,
                     BASE + 120, 0, 0, 0)
    tags[100:119] = b"synthetic/scenario\0"
    tags[120:128] = b"fakebsp\0"
    struct.pack_into("<3I", tags, ROOT + 0x5A4, bsp_count, BASE + BSP_TABLE, 0)
    for ordinal in range(bsp_count):
        struct.pack_into("<8I", tags, BSP_TABLE + 32 * ordinal,
                         2048 + size + ordinal * 64, 64,
                         BASE + size + 4096 + ordinal * 64, 0,
                         BSP, BASE + 120, 7, 0x20001)
    return tags


def map_bytes(tags=None, *, prefix=b"", suffix=b"S" * 4096, padding=b"",
              declared_delta=0, header_edits=()):
    tags = tag_bytes() if tags is None else tags
    body = prefix + bytes(tags) + suffix
    header = bytearray(2048)
    struct.pack_into("<6I", header, 0, 0x68656164, 5,
                     2048 + len(body) + declared_delta, 0,
                     2048 + len(prefix), len(tags))
    struct.pack_into("<I", header, 2044, 0x666F6F74)
    for offset, value in header_edits:
        struct.pack_into("<I", header, offset, value)
    compressed = zlib.compress(body)
    return bytes(header) + compressed + padding, bytes(tags), body, compressed


class CacheResidencyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "synthetic.map"

    def inspect(self, data, **kwargs):
        self.source.write_bytes(data)
        before = self.source.stat()
        result = cache.inspect(self.source, **kwargs)
        self.assertEqual(self.source.read_bytes(), data)
        self.assertEqual(self.source.stat().st_mtime_ns, before.st_mtime_ns)
        return result

    def rejects(self, data, *, message=None, **kwargs):
        self.source.write_bytes(data)
        before = self.source.stat()
        with self.assertRaises((ValueError, zlib.error)) as caught:
            cache.inspect(self.source, **kwargs)
        if message:
            self.assertIn(message, str(caught.exception))
        self.assertEqual(self.source.read_bytes(), data)
        self.assertEqual(self.source.stat().st_mtime_ns, before.st_mtime_ns)

    def test_valid_metadata_and_independent_hash_size_oracles(self):
        data, tags, body, compressed = map_bytes()
        result = self.inspect(data)
        self.assertEqual(result["input_sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(result["tag_sha256"], hashlib.sha256(tags).hexdigest())
        self.assertEqual(result["tag_crc32"], zlib.crc32(tags))
        self.assertEqual(result["inflated_body_crc32_noncanonical"], zlib.crc32(body))
        self.assertEqual((result["stored_bytes"], result["declared_bytes"],
                          result["compressed_stream_bytes"], result["trailing_stored_bytes"]),
                         (len(data), 2048 + len(body), len(compressed), 0))
        self.assertEqual((result["header_bytes"], result["instance_stride"],
                          result["instance_table_bytes"], result["instance_count"]), (36, 32, 64, 2))
        self.assertEqual(result["classes"], {"73636e72": 1, "73627370": 1})
        self.assertEqual(result["scenario_datum"], 0x10000)
        self.assertEqual(result["header_checksum_observed_not_validated"], 0x12345678)
        self.assertEqual(result["roots_outside_tag_bytes"], 1)
        self.assertEqual(result["bsps"], [{"ordinal": 0, "file_offset": 4096,
                         "bytes": 64, "encoded_address": BASE + 6144,
                         "tag_window_end_offset": 6208}])
        self.assertEqual(result["maximum_tag_window_end"], 6208)
        self.assertEqual(result["maximum_bsp_bytes"], 64)
        self.assertTrue(result["source_metadata_unchanged"])
        self.assertFalse(result["conversion_performed"])
        self.assertEqual(list(self.directory.iterdir()), [self.source])

    def test_nonzero_prefix_tag_extent_spans_multiple_small_inflate_chunks(self):
        data, tags, body, compressed = map_bytes(prefix=b"P" * 37)
        with patch.object(cache, "CHUNK", 17):
            result = self.inspect(data)
        self.assertEqual(result["tag_file_offset"], 2085)
        self.assertEqual(result["tag_sha256"], hashlib.sha256(tags).hexdigest())
        self.assertEqual(result["inflated_body_crc32_noncanonical"], zlib.crc32(body))
        self.assertEqual(result["compressed_stream_bytes"], len(compressed))
        self.assertEqual(result["inflate_chunk_bound"], 17)

    def test_zero_padding_exact_compressed_byte_accounting_after_output_limit(self):
        data, tags, body, compressed = map_bytes(suffix=b"Z" * 140000,
                                               padding=bytes(70000))
        result = self.inspect(data)
        self.assertEqual(result["compressed_stream_bytes"], len(compressed))
        self.assertEqual(result["trailing_stored_bytes"], 70000)
        self.assertEqual(result["declared_bytes"], 2048 + len(body))
        self.assertEqual(result["tag_sha256"], hashlib.sha256(tags).hexdigest())

    def test_arbitrary_trailing_and_second_stream_observed_without_parsing(self):
        for trailer in (b"nonzero trailer\xff", zlib.compress(b"second invented stream")):
            with self.subTest(trailer=trailer):
                data, tags, body, compressed = map_bytes(suffix=b"Z" * 140000,
                                                       padding=trailer)
                result = self.inspect(data)
                self.assertEqual(result["compressed_stream_bytes"], len(compressed))
                self.assertEqual(result["trailing_stored_bytes"], len(trailer))
                self.assertEqual(result["inflated_body_crc32_noncanonical"], zlib.crc32(body))
                self.assertEqual(result["tag_sha256"], hashlib.sha256(tags).hexdigest())

    def test_uncompressed_body_is_not_a_zlib_stream(self):
        data, _, body, _ = map_bytes()
        self.rejects(data[:2048] + body)

    def test_truncated_compressed_stream_at_header_payload_and_checksum(self):
        data, _, _, compressed = map_bytes()
        for length in (0, 1, 2, len(compressed) // 2, len(compressed) - 1,
                       len(compressed) - 4):
            with self.subTest(compressed_length=length):
                self.rejects(data[:2048 + length])

    def test_corrupt_zlib_checksum_rejected_without_output(self):
        data = bytearray(map_bytes()[0])
        data[-1] ^= 1
        output = self.directory / "private.tags"
        self.rejects(data, tag_output=output)
        self.assertFalse(output.exists())
        self.assertFalse(output.with_suffix(".tags.partial").exists())

    def test_inflated_output_exact_length_required_in_both_directions(self):
        for delta in (-1, 1):
            with self.subTest(delta=delta):
                self.rejects(map_bytes(declared_delta=delta)[0])

    def test_short_header_and_identity(self):
        for length in (0, 1, 2047):
            with self.subTest(length=length):
                self.rejects(map_bytes()[0][:length], message="truncated")
        for offset, value in ((0, 0), (4, 6), (2044, 0)):
            with self.subTest(offset=offset):
                self.rejects(map_bytes(header_edits=[(offset, value)])[0], message="identity")

    def test_map_tag_declared_size_and_extent_bounds(self):
        for offset, value in ((8, 2047), (8, 0x11600001), (16, 2047),
                              (16, 0xFFFFFFFF), (20, 35), (20, 0x1600001),
                              (20, 0xFFFFFFFF)):
            with self.subTest(offset=offset, value=value):
                self.rejects(map_bytes(header_edits=[(offset, value)])[0])

    def test_profile_limits_exact_and_one_over_without_large_allocation(self):
        data, tags, body, _ = map_bytes()
        with patch.object(cache, "TAG_LIMIT", len(tags)), patch.object(cache, "MAP_LIMIT", 2048 + len(body)):
            # No BSP means no reservation requirement beyond the reduced tag bound.
            struct.pack_into("<I", tags := bytearray(tags), ROOT + 0x5A4, 0)
            self.inspect(map_bytes(tags)[0])
        with patch.object(cache, "TAG_LIMIT", len(tags) - 1):
            self.rejects(data)
        with patch.object(cache, "MAP_LIMIT", 2048 + len(body) - 1):
            self.rejects(data)

    def test_tag_index_signature_and_signed_count_bounds(self):
        for offset, value in ((32, 0), (12, 0), (12, 65536),
                              (12, 0xFFFFFFFF), (16, 0x80000000), (24, 0xFFFFFFFF)):
            with self.subTest(offset=offset, value=value):
                tags = tag_bytes()
                struct.pack_into("<I", tags, offset, value)
                self.rejects(map_bytes(tags)[0])

    def test_instance_table_address_and_exact_end_extent(self):
        for address in (BASE - 1, BASE + 2048, 0xFFFFFFFF):
            with self.subTest(address=address):
                tags = tag_bytes()
                struct.pack_into("<I", tags, 0, address)
                self.rejects(map_bytes(tags)[0])
        tags = tag_bytes()
        tags[-64:] = tags[36:100]
        struct.pack_into("<I", tags, 0, BASE + len(tags) - 64)
        self.assertEqual(self.inspect(map_bytes(tags)[0])["instance_count"], 2)

    def test_vertex_and_index_buffer_spans_width_twelve(self):
        for count_offset, pointer_offset in ((16, 20), (24, 28)):
            with self.subTest(count_offset=count_offset):
                tags = tag_bytes()
                struct.pack_into("<II", tags, count_offset, 1, BASE + len(tags) - 12)
                self.inspect(map_bytes(tags)[0])
                struct.pack_into("<I", tags, pointer_offset, BASE + len(tags) - 11)
                self.rejects(map_bytes(tags)[0])
                struct.pack_into("<II", tags, count_offset, 0, 0xFFFFFFFF)
                self.inspect(map_bytes(tags)[0])

    def test_datum_low_ordinal_and_scenario_full_identity(self):
        for offset, value in ((36 + 12, 0x10001), (68 + 12, 0x20000),
                              (4, 0x20000), (4, 0x10002), (36, BSP)):
            with self.subTest(offset=offset, value=value):
                tags = tag_bytes()
                struct.pack_into("<I", tags, offset, value)
                self.rejects(map_bytes(tags)[0])

    def test_names_require_address_and_bounded_terminator(self):
        for address in (BASE - 1, BASE + 2048, 0xFFFFFFFF):
            with self.subTest(address=address):
                tags = tag_bytes()
                struct.pack_into("<I", tags, 36 + 16, address)
                self.rejects(map_bytes(tags)[0])
        tags = tag_bytes()
        struct.pack_into("<I", tags, 36 + 16, BASE + len(tags) - 1)
        self.inspect(map_bytes(tags)[0])
        tags[-1] = 65
        self.rejects(map_bytes(tags)[0], message="terminator")

    def test_non_scenario_external_and_null_roots_are_counted_not_dereferenced(self):
        for address in (0, BASE - 1, BASE + 2048, 0xFFFFFFFF):
            with self.subTest(address=address):
                tags = tag_bytes()
                struct.pack_into("<I", tags, 68 + 20, address)
                self.assertEqual(self.inspect(map_bytes(tags)[0])["roots_outside_tag_bytes"], 1)

    def test_scenario_root_extent_exact_end_and_one_over(self):
        tags = tag_bytes(bsp_count=0)
        root = len(tags) - (0x5A4 + 12)
        tags[root:root + 0x5A4 + 12] = bytes(0x5A4 + 12)
        struct.pack_into("<I", tags, 36 + 20, BASE + root)
        self.assertEqual(self.inspect(map_bytes(tags)[0])["bsps"], [])
        struct.pack_into("<I", tags, 36 + 20, BASE + root + 1)
        self.rejects(map_bytes(tags)[0])

    def test_bsp_count_zero_ignores_pointer_and_schema_maximum(self):
        tags = tag_bytes(bsp_count=0)
        struct.pack_into("<I", tags, ROOT + 0x5A4 + 4, 0xFFFFFFFF)
        result = self.inspect(map_bytes(tags)[0])
        self.assertEqual((result["bsps"], result["maximum_bsp_bytes"],
                          result["maximum_tag_window_end"]), ([], 0, len(tags)))
        self.assertEqual(len(self.inspect(map_bytes(tag_bytes(size=4096, bsp_count=16))[0])["bsps"]), 16)
        for count in (17, 0xFFFFFFFF):
            with self.subTest(count=count):
                struct.pack_into("<I", tags, ROOT + 0x5A4, count)
                self.rejects(map_bytes(tags)[0], message="schema limit")

    def test_bsp_table_span(self):
        for address in (BASE - 1, BASE + 2048 - 31, 0xFFFFFFFF):
            with self.subTest(address=address):
                tags = tag_bytes()
                struct.pack_into("<I", tags, ROOT + 0x5A4 + 4, address)
                self.rejects(map_bytes(tags)[0])

    def test_bsp_file_extent_exact_end_and_one_over(self):
        tags = tag_bytes()
        declared = 2048 + len(tags) + 4096
        struct.pack_into("<II", tags, BSP_TABLE, declared - 64, 64)
        self.inspect(map_bytes(tags)[0])
        for offset, size in ((declared - 63, 64), (declared + 1, 0),
                             (0xFFFFFFFF, 64), (4096, 0xFFFFFFFF)):
            with self.subTest(offset=offset, size=size):
                struct.pack_into("<II", tags, BSP_TABLE, offset, size)
                self.rejects(map_bytes(tags)[0])

    def test_bsp_runtime_extent_exact_reservation_end_and_one_over(self):
        tags = tag_bytes()
        struct.pack_into("<I", tags, BSP_TABLE + 8, BASE + 0x1600000 - 64)
        self.assertEqual(self.inspect(map_bytes(tags)[0])["maximum_tag_window_end"], 0x1600000)
        for address in (BASE - 1, BASE + 0x1600000 - 63, 0xFFFFFFFF):
            with self.subTest(address=address):
                struct.pack_into("<I", tags, BSP_TABLE + 8, address)
                self.rejects(map_bytes(tags)[0])

    def test_bsp_zero_size_cannot_hide_outside_runtime_address(self):
        tags = tag_bytes()
        struct.pack_into("<II", tags, BSP_TABLE + 4, 0, 0xFFFFFFFF)
        self.rejects(map_bytes(tags)[0])

    def test_bsp_minimum_header_size_exact_and_one_short(self):
        tags = tag_bytes()
        struct.pack_into("<I", tags, BSP_TABLE + 4, 24)
        self.assertEqual(self.inspect(map_bytes(tags)[0])["maximum_bsp_bytes"], 24)
        for size in (0, 1, 23):
            with self.subTest(size=size):
                struct.pack_into("<I", tags, BSP_TABLE + 4, size)
                self.rejects(map_bytes(tags)[0])

    def test_span_checks_counts_addresses_and_32bit_end_without_allocating(self):
        self.assertEqual(cache.span(BASE + 8, 2, 4, 16), (8, 8))
        self.assertEqual(cache.span(BASE + 16, 0, 4, 16), (16, 0))
        for arguments in ((BASE, -1, 4, 16), (BASE, 1, 0, 16),
                          (BASE - 1, 0, 1, 16), (BASE + 17, 0, 1, 16),
                          (BASE + 9, 2, 4, 16), (0xFFFFFFFF, 2, 1, 0x100000000)):
            with self.subTest(arguments=arguments):
                with self.assertRaises(ValueError):
                    cache.span(*arguments)

    def test_changed_input_metadata_rejected_before_private_output(self):
        data = map_bytes()[0]
        self.source.write_bytes(data)
        before = self.source.stat()
        changed = SimpleNamespace(st_size=before.st_size, st_mtime_ns=before.st_mtime_ns + 1)
        destination = self.directory / "private.tags"
        with patch.object(cache.os, "fstat", side_effect=[before, changed]):
            with self.assertRaisesRegex(ValueError, "metadata changed"):
                cache.inspect(self.source, destination)
        self.assertEqual(self.source.read_bytes(), data)
        self.assertFalse(destination.exists())

    def test_exclusive_tag_output_and_partial_preserve_existing_bytes(self):
        data, tags, _, _ = map_bytes()
        destination = self.directory / "private.tags"
        self.inspect(data, tag_output=destination)
        self.assertEqual(destination.read_bytes(), tags)
        self.assertFalse((self.directory / "private.tags.partial").exists())
        self.rejects(data, tag_output=destination, message="overwrite")
        self.assertEqual(destination.read_bytes(), tags)
        destination.unlink()
        partial = self.directory / "private.tags.partial"
        partial.write_bytes(b"existing partial")
        self.rejects(data, tag_output=destination, message="overwrite")
        self.assertEqual(partial.read_bytes(), b"existing partial")
        self.assertFalse(destination.exists())
        self.rejects(data, tag_output=self.source, message="overwrite")

    def cli(self, *arguments):
        output, error = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", ["inspect_cache_residency.py", *map(str, arguments)]), \
                redirect_stdout(output), redirect_stderr(error):
            try:
                cache.main()
                code = 0
            except SystemExit as failure:
                code = failure.code
        return code, output.getvalue(), error.getvalue()

    def test_cli_metadata_report_and_optional_tag_output(self):
        data, tags, _, _ = map_bytes()
        self.source.write_bytes(data)
        report, destination = self.directory / "report.json", self.directory / "private.tags"
        code, output, error = self.cli(self.source, "--output", report, "--tag-output", destination)
        self.assertEqual((code, error), (0, ""))
        self.assertIn("2 tags / 2048 bytes", output)
        result = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result["tag_sha256"], hashlib.sha256(tags).hexdigest())
        self.assertNotIn("synthetic/scenario", report.read_text(encoding="utf-8"))
        self.assertEqual(destination.read_bytes(), tags)
        self.assertEqual(self.source.read_bytes(), data)

    def test_cli_exclusive_report_and_input_alias(self):
        data = map_bytes()[0]
        self.source.write_bytes(data)
        report = self.directory / "report.json"
        report.write_bytes(b"existing report")
        for destination in (report, self.source):
            with self.subTest(destination=destination):
                code, output, error = self.cli(self.source, "--output", destination)
                self.assertEqual((code, output), (2, ""))
                self.assertIn("overwrite inspection report", error)
                self.assertEqual(self.source.read_bytes(), data)
                self.assertEqual(report.read_bytes(), b"existing report")

    def test_cli_bad_stream_does_not_create_report_or_tag_output(self):
        self.source.write_bytes(map_bytes()[0][:-1])
        report, destination = self.directory / "report.json", self.directory / "private.tags"
        code, output, error = self.cli(self.source, "--output", report, "--tag-output", destination)
        self.assertEqual((code, output), (1, ""))
        self.assertIn("cache inspection failed", error)
        self.assertFalse(report.exists())
        self.assertFalse(destination.exists())
        self.assertFalse((self.directory / "private.tags.partial").exists())


if __name__ == "__main__":
    unittest.main()
