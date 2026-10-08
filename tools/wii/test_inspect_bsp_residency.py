"""Invented BSP residency fixtures; no commercial assets, runtime or SDK access."""
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

import inspect_bsp_residency as inspector


class Fixture:
    SCENARIO = 256
    REFERENCES = 2048
    BSP_BASE = inspector.BASE + 8192
    SCENARIO_DATUM = 0x12340000
    BSP_DATUM = 0x23450001

    def __init__(self, size=1001, references=1):
        self.tags = bytearray(b'Z' * 4096)
        self.bsp = bytearray(b'Q' * size)
        self.map_bytes = 2048
        struct.pack_into('<9I', self.tags, 0, inspector.BASE + 36, self.SCENARIO_DATUM,
                         0x87654321, 2, 0, 0xFFFFFFFF, 0, 0x87654321, 0x74616773)
        struct.pack_into('<8I', self.tags, 36, 0x73636E72, 0xFFFFFFFF, 0xFFFFFFFF,
                         self.SCENARIO_DATUM, inspector.BASE + 180, inspector.BASE + self.SCENARIO,
                         0x12345678, 0xFEDCBA98)
        struct.pack_into('<8I', self.tags, 68, 0x73627370, 0xFFFFFFFF, 0xFFFFFFFF,
                         self.BSP_DATUM, inspector.BASE + 190, 0, 0x87654321, 0xAABBCCDD)
        struct.pack_into('<iII', self.tags, self.SCENARIO + 0x5A4,
                         references, inspector.BASE + self.REFERENCES, 0xFEDC1234)
        for index in range(references):
            struct.pack_into('<ii6I', self.tags, self.REFERENCES + index * 32,
                             512, size, self.BSP_BASE, 0xA1B2C3D4, 0x73627370,
                             inspector.BASE + 190, 0, self.BSP_DATUM)
        struct.pack_into('<6I', self.bsp, 0, self.BSP_BASE + 24,
                         1, self.BSP_BASE + 704, 1, self.BSP_BASE + 728, 0x73627370)
        struct.pack_into('<3I', self.bsp, 704, 0x81234567, self.BSP_BASE + 900, 0xFEDCBA98)
        struct.pack_into('<3I', self.bsp, 728, 0xABCDEF01, self.BSP_BASE + 901, 0x80000000)
        self.tags[180:190] = b'FAKE-NAME\0'
        self.tags[190:200] = b'SECRET-X\0\0'

    def tag_word(self, offset, value):
        struct.pack_into('<I', self.tags, offset, value & 0xFFFFFFFF)

    def bsp_word(self, offset, value):
        struct.pack_into('<I', self.bsp, offset, value & 0xFFFFFFFF)

    def inspect(self, ordinal=0, **kwargs):
        return inspector.inspect_bytes(bytes(self.tags), bytes(self.bsp), self.map_bytes, ordinal, **kwargs)


class ResolverTests(unittest.TestCase):
    WINDOWS = ((0x1000, 10), (0x2000, 12))

    def test_both_exact_windows_and_one_byte_end(self):
        self.assertEqual(inspector.resolve_windows(self.WINDOWS, 0x1000, 2, 5),
                         {'window': 0, 'offset': 0, 'bytes': 10})
        self.assertEqual(inspector.resolve_windows(self.WINDOWS, 0x200B, 1, 1),
                         {'window': 1, 'offset': 11, 'bytes': 1})

    def test_gap_boundaries_and_cross_window_spans_reject(self):
        for address, count, stride in ((0xFFF, 1, 1), (0x100A, 1, 1), (0x1800, 1, 1),
                                      (0x200C, 1, 1), (0x1009, 2, 1), (0x1000, 1, 0x1001)):
            with self.subTest(address=address):
                with self.assertRaisesRegex(inspector.InspectionError, 'valid windows'):
                    inspector.resolve_windows(self.WINDOWS, address, count, stride)

    def test_zero_count_ignores_pointer_and_zero_stride(self):
        self.assertEqual(inspector.resolve_windows(self.WINDOWS, 0xFFFFFFFF, 0, 0),
                         {'window': None, 'offset': 0, 'bytes': 0})

    def test_signed_counts_arguments_and_encoded_overflow(self):
        for count, stride, address in ((-1, 12, 0x1000), (True, 1, 0x1000),
                                      (1, 0, 0x1000), (0x7FFFFFFF, 12, 0x1000),
                                      (1, 2, 0xFFFFFFFF)):
            with self.subTest(count=count, stride=stride):
                with self.assertRaises(inspector.InspectionError):
                    inspector.resolve_windows(self.WINDOWS, address, count, stride)
        with self.assertRaisesRegex(inspector.InspectionError, 'window overflow'):
            inspector.resolve_windows(((0xFFFFFFFF, 2), (0x1000, 10)), 0x1000, 1, 1)

    def test_adjacent_windows_remain_separate_and_overlap_rejects(self):
        windows = ((0x1000, 10), (0x100A, 10))
        with self.assertRaises(inspector.InspectionError):
            inspector.resolve_windows(windows, 0x1009, 2, 1)
        with self.assertRaisesRegex(inspector.InspectionError, 'overlap'):
            inspector.resolve_windows(((0x1000, 10), (0x1009, 10)), 0x1000, 1, 1)

    def test_last_uint32_address_is_a_valid_one_byte_window(self):
        self.assertEqual(inspector.resolve_windows(((0xFFFFFFFF, 1), (0, 0)), 0xFFFFFFFF, 1, 1),
                         {'window': 0, 'offset': 0, 'bytes': 1})


class InspectorTests(unittest.TestCase):
    def rejects(self, fixture, reason, **kwargs):
        with self.assertRaisesRegex(inspector.InspectionError, reason):
            fixture.inspect(**kwargs)

    def test_independent_identity_goldens_and_source_unchanged(self):
        f = Fixture()
        before = bytes(f.tags), bytes(f.bsp)
        result = f.inspect()
        self.assertEqual(result['goldens']['datum'], f.BSP_DATUM)
        self.assertEqual(result['goldens']['root_offset'], 24)
        self.assertEqual(result['goldens']['file_bytes'], 1001)
        self.assertEqual(result['goldens']['rounded_reservation_bytes'], 1024)
        self.assertEqual(result['goldens']['serialized_crc32'], zlib.crc32(before[1]))
        self.assertEqual(result['goldens']['serialized_sha256'], hashlib.sha256(before[1]).hexdigest())
        self.assertEqual(result['tag_input_sha256'], hashlib.sha256(before[0]).hexdigest())
        self.assertEqual(result['BSP_root'], {'window': 1, 'offset': 24, 'bytes': 648})
        self.assertEqual([t['data_spans_checked'] for t in result['descriptor_tables']], [1, 1])
        self.assertEqual((bytes(f.tags), bytes(f.bsp)), before)

    def test_opaque_words_zero_count_pointers_and_unloaded_root_preserved(self):
        f = Fixture()
        for count_at, pointer_at in ((4, 8), (12, 16)):
            f.bsp_word(count_at, 0)
            f.bsp_word(pointer_at, 0xFFFFFFFF)
        result = f.inspect()
        self.assertEqual(result['BSP_header_words'][2:5:2], [0xFFFFFFFF, 0xFFFFFFFF])
        self.assertEqual(result['selected_reference_words'][3], 0xA1B2C3D4)
        self.assertEqual(result['BSP_instance_words'][5], 0)
        self.assertEqual([t['count'] for t in result['descriptor_tables']], [0, 0])

    def test_source_16_references_accept_last_17_and_negative_reject(self):
        self.assertEqual(Fixture(references=16).inspect(ordinal=15)['goldens']['bsp_ordinal'], 15)
        for count in (17, -1):
            f = Fixture()
            f.tag_word(f.SCENARIO + 0x5A4, count)
            self.rejects(f, 'signed count')

    def test_empty_block_pointer_ignored_but_selection_rejected(self):
        f = Fixture()
        f.tag_word(f.SCENARIO + 0x5A4, 0)
        f.tag_word(f.SCENARIO + 0x5A8, 0xFFFFFFFF)
        self.rejects(f, 'ordinal outside block')

    def test_ordinal_and_public_argument_validation(self):
        f = Fixture()
        for length, ordinal, base in ((0, 0, inspector.BASE), (0x80000000, 0, inspector.BASE),
                                     (2048, -1, inspector.BASE), (2048, 16, inspector.BASE),
                                     (2048, True, inspector.BASE), (2048, 0, 0xFFFFFFFF)):
            with self.subTest(length=length, ordinal=ordinal):
                with self.assertRaises(inspector.InspectionError):
                    inspector.inspect_bytes(bytes(f.tags), bytes(f.bsp), length, ordinal, base)
        self.rejects(f, 'ordinal outside block', ordinal=1)

    def test_immutable_type_size_and_header_signature(self):
        f = Fixture()
        for tags, bsp in ((f.tags, bytes(f.bsp)), (bytes(f.tags), f.bsp),
                          (bytes(35), bytes(f.bsp)), (bytes(f.tags), bytes(23)),
                          (bytes(inspector.MAX_BYTES + 1), bytes(f.bsp))):
            with self.assertRaises(inspector.InspectionError):
                inspector.inspect_bytes(tags, bsp, 2048, 0)
        f.tag_word(32, 0)
        self.rejects(f, 'tag header signature')

    def test_tag_header_signed_counts_table_bounds_and_overlap(self):
        for offset, value, reason in ((12, -1, 'signed counts'), (12, 0, 'signed counts'),
                                      (12, 65536, 'signed counts'), (16, -1, 'signed counts'),
                                      (24, -1, 'signed counts'), (0, inspector.BASE + 4090, 'valid windows'),
                                      (0, inspector.BASE, 'overlap')):
            f = Fixture()
            f.tag_word(offset, value)
            self.rejects(f, reason)

    def test_scenario_full_datum_group_root_and_reference_bounds(self):
        for offset, value, reason in ((4, 0xFFFFFFFF, 'datum ordinal'), (4, 0x12350000, 'full instance'),
                                      (36, 0x73627370, 'group identity'),
                                      (36 + 20, inspector.BASE + 4096 - 1455, 'valid windows'),
                                      (Fixture.SCENARIO + 0x5A8, inspector.BASE + 4096 - 31, 'valid windows')):
            f = Fixture()
            f.tag_word(offset, value)
            self.rejects(f, reason)

    def test_scenario_reference_table_overlap_rejects(self):
        f = Fixture()
        f.tag_word(f.SCENARIO + 0x5A8, inspector.BASE + f.SCENARIO + 100)
        self.rejects(f, 'overlap')

    def test_reference_full_datum_group_instance_and_unloaded_policy(self):
        for offset, value, reason in ((Fixture.REFERENCES + 28, 0x23460001, 'full instance'),
                                      (Fixture.REFERENCES + 28, 0x23450002, 'datum ordinal'),
                                      (Fixture.REFERENCES + 16, 0x73636E72, 'reference group'),
                                      (68, 0x73636E72, 'instance group'),
                                      (68 + 20, Fixture.BSP_BASE + 24, 'unloaded')):
            f = Fixture()
            f.tag_word(offset, value)
            self.rejects(f, reason)

    def test_signed_file_extents_exact_sidecar_and_declared_length(self):
        for offset, value, reason in ((0, -1, 'signed file'), (4, -1, 'signed file'),
                                      (4, 23, 'signed file'), (4, 1000, 'sidecar length'),
                                      (0, 2048 - 1001 + 1, 'declared map')):
            f = Fixture()
            f.tag_word(f.REFERENCES + offset, value)
            self.rejects(f, reason)
        f = Fixture()
        f.map_bytes = 512 + 1001  # Logical bytes fit; rounded read need not fit the map declaration.
        self.assertEqual(f.inspect()['goldens']['file_bytes'], 1001)

    def test_rounded_512_reservation_edge_not_logical_only(self):
        f = Fixture()
        f.tag_word(f.REFERENCES + 8, inspector.BASE + inspector.MAX_BYTES - 1024)
        # Rebase only the six known descriptor/root words and two Data words.
        for offset in (0, 8, 16, 708, 732):
            value = struct.unpack_from('<I', f.bsp, offset)[0]
            f.bsp_word(offset, value - f.BSP_BASE + inspector.BASE + inspector.MAX_BYTES - 1024)
        self.assertEqual(f.inspect()['goldens']['rounded_reservation_bytes'], 1024)
        f.tag_word(f.REFERENCES + 8, inspector.BASE + inspector.MAX_BYTES - 1001)
        self.rejects(f, 'rounded reservation')

    def test_bsp_placement_must_be_above_tags_and_inside_reservation(self):
        for base in (inspector.BASE - 1, inspector.BASE + 4095,
                     inspector.BASE + inspector.MAX_BYTES, 0xFFFFFFFF):
            f = Fixture()
            f.tag_word(f.REFERENCES + 8, base)
            self.rejects(f, 'rounded reservation')

    def test_bsp_signature_root_bounds_and_header_overlap(self):
        for offset, value, reason in ((20, 0, 'header signature'),
                                      (0, Fixture.BSP_BASE + 1001 - 647, 'valid windows'),
                                      (0, Fixture.BSP_BASE + 23, 'overlap'),
                                      (0, inspector.BASE + 256, 'wrong region'),
                                      (0, inspector.BASE + 6000, 'unread gap')):
            f = Fixture()
            f.bsp_word(offset, value)
            self.rejects(f, reason)

    def test_signed_descriptor_counts_extent_and_wrong_region(self):
        for count_at, address_at in ((4, 8), (12, 16)):
            for value, reason in ((-1, 'signed descriptor'), (0x7FFFFFFF, 'overflow')):
                f = Fixture()
                f.bsp_word(count_at, value)
                self.rejects(f, reason)
            for address, reason in ((Fixture.BSP_BASE + 990, 'valid windows'),
                                     (inspector.BASE + 2000, 'wrong region')):
                f = Fixture()
                f.bsp_word(address_at, address)
                self.rejects(f, reason)

    def test_every_descriptor_Data_is_checked_not_only_first(self):
        f = Fixture()
        f.bsp_word(4, 2)
        struct.pack_into('<3I', f.bsp, 716, 0x80000000, f.BSP_BASE + 1000, 0xFFFFFFFF)
        self.assertEqual(f.inspect()['descriptor_tables'][0]['data_spans_checked'], 2)
        f.bsp_word(720, inspector.BASE + 6000)
        self.rejects(f, 'unread gap')

    def test_descriptor_table_overlap_with_root_header_and_each_other(self):
        for offset, address in ((8, Fixture.BSP_BASE), (8, Fixture.BSP_BASE + 24),
                                (16, Fixture.BSP_BASE + 704)):
            f = Fixture()
            f.bsp_word(offset, address)
            self.rejects(f, 'overlap')

    def test_descriptor_Data_one_byte_last_byte_and_gap_tail_rejects(self):
        f = Fixture()
        f.bsp_word(708, f.BSP_BASE + 1000)
        self.assertEqual(f.inspect()['descriptor_tables'][0]['maximum_data_address'], f.BSP_BASE + 1000)
        for pointer, reason in ((f.BSP_BASE + 1001, 'valid windows'), (f.BSP_BASE + 1023, 'valid windows'),
                                (inspector.BASE + 6000, 'unread gap'), (inspector.BASE + 180, 'wrong region'),
                                (0xFFFFFFFF, 'valid windows')):
            f = Fixture()
            f.bsp_word(708, pointer)
            self.rejects(f, reason)

    def test_nondefault_encoded_tag_base(self):
        f = Fixture()
        delta = 0x100000
        for offset in (0, 36 + 16, 36 + 20, 68 + 16, f.SCENARIO + 0x5A8,
                       f.REFERENCES + 8, f.REFERENCES + 20):
            f.tag_word(offset, struct.unpack_from('<I', f.tags, offset)[0] + delta)
        for offset in (0, 8, 16, 708, 732):
            f.bsp_word(offset, struct.unpack_from('<I', f.bsp, offset)[0] + delta)
        self.assertEqual(f.inspect(tag_base=inspector.BASE + delta)['goldens']['root_offset'], 24)

    def test_metadata_contains_no_input_names_scripts_or_raw_arrays(self):
        text = json.dumps(Fixture().inspect())
        self.assertNotIn('FAKE-NAME', text)
        self.assertNotIn('SECRET-X', text)
        self.assertNotIn('QQQQ', text)
        self.assertIn('geometry_nested_fields_and_Data_payload_sizes_not_validated', text)


class FileTests(unittest.TestCase):
    def files(self, directory):
        f = Fixture()
        tags, bsp = Path(directory) / 'private-tags.bin', Path(directory) / 'private-bsp.bin'
        tags.write_bytes(f.tags)
        bsp.write_bytes(f.bsp)
        return tags, bsp

    def test_readonly_files_and_source_hash_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            before = tags.read_bytes(), bsp.read_bytes()
            result = inspector.inspect_files(tags, bsp, 2048, 0)
            self.assertEqual((tags.read_bytes(), bsp.read_bytes()), before)
            self.assertIn('tools/wii/inspect_bsp_residency.py', result['source_hashes'])

    def test_checkout_inputs_and_output_rejected(self):
        with self.assertRaisesRegex(inspector.InspectionError, 'outside'):
            inspector.inspect_files(inspector.CHECKOUT / '.local/tags', Path('other'), 2048, 0)
        with self.assertRaisesRegex(inspector.InspectionError, 'outside'):
            inspector.write_exclusive(inspector.CHECKOUT / '.local/result', {})

    def test_descriptor_mutation_guard_each_input(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            actual = inspector.os.fstat
            for changed_call in (3, 4):
                calls = 0

                def changed(fd):
                    nonlocal calls
                    value = actual(fd)
                    calls += 1
                    return SimpleNamespace(st_dev=value.st_dev, st_ino=value.st_ino, st_size=value.st_size,
                        st_mtime_ns=value.st_mtime_ns + (calls == changed_call), st_ctime_ns=value.st_ctime_ns)

                with patch.object(inspector.os, 'fstat', side_effect=changed):
                    with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                        inspector.inspect_files(tags, bsp, 2048, 0)

    def test_path_replacement_guard_without_descriptor_change(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            actual = Path.stat

            def changed(path, *args, **kwargs):
                value = actual(path, *args, **kwargs)
                if path == tags:
                    return SimpleNamespace(st_dev=value.st_dev, st_ino=value.st_ino + 1,
                        st_size=value.st_size, st_mtime_ns=value.st_mtime_ns, st_ctime_ns=value.st_ctime_ns)
                return value

            with patch.object(Path, 'stat', changed):
                with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                    inspector.inspect_files(tags, bsp, 2048, 0)

    def test_mutation_after_snapshot_during_inspection_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            actual = inspector.inspect_bytes
            before = tags.stat()

            def inspecting(*args):
                result = actual(*args)
                inspector.os.utime(tags, ns=(before.st_atime_ns, before.st_mtime_ns + 2_000_000_000))
                return result

            with patch.object(inspector, 'inspect_bytes', side_effect=inspecting):
                with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                    inspector.inspect_files(tags, bsp, 2048, 0)

    def test_descriptor_path_ctime_semantics_may_differ(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            actual = inspector.os.fstat

            def descriptor(fd):
                value = actual(fd)
                return SimpleNamespace(st_dev=value.st_dev, st_ino=value.st_ino, st_size=value.st_size,
                    st_mtime_ns=value.st_mtime_ns, st_ctime_ns=value.st_ctime_ns + 100000)

            with patch.object(inspector.os, 'fstat', side_effect=descriptor):
                self.assertEqual(inspector.inspect_files(tags, bsp, 2048, 0)['goldens']['file_bytes'], 1001)

    def test_exclusive_fsynced_publication_preserves_existing_and_cleans_temp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.json'
            with patch.object(inspector.os, 'fsync', wraps=inspector.os.fsync) as sync:
                inspector.write_exclusive(path, {'numeric': 17})
                self.assertEqual(sync.call_count, 1)
            before = path.read_bytes()
            with self.assertRaises(FileExistsError):
                inspector.write_exclusive(path, {'numeric': 42})
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual([p.name for p in Path(directory).iterdir()], ['result.json'])

    def test_link_failure_no_success_output_or_temporary(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(inspector.os, 'link', side_effect=OSError(5, 'private-location')):
                with self.assertRaises(OSError):
                    inspector.write_exclusive(Path(directory) / 'result.json', {})
            self.assertFalse(list(Path(directory).iterdir()))

    def test_cli_numeric_privacy_no_overwrite_and_no_source_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            output = Path(directory) / 'result.json'
            before = tags.read_bytes(), bsp.read_bytes()
            args = [str(tags), str(bsp), '--declared-map-length', '2048', '--bsp-ordinal', '0',
                    '--output', str(output)]
            stdout, stderr = io.StringIO(), io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                self.assertEqual(inspector.main(args), 0)
                first = output.read_bytes()
                self.assertEqual(inspector.main(args), 1)
            self.assertEqual(output.read_bytes(), first)
            self.assertEqual((tags.read_bytes(), bsp.read_bytes()), before)
            text = output.read_text() + stdout.getvalue() + stderr.getvalue()
            for private in (directory, 'private-tags', 'private-bsp', 'FAKE-NAME', 'SECRET-X'):
                self.assertNotIn(private, text)
            self.assertEqual(json.loads(output.read_text())['goldens']['serialized_bytes'], 1001)

    def test_cli_missing_and_invalid_input_no_output_or_private_error_path(self):
        with tempfile.TemporaryDirectory() as directory:
            tags, bsp = self.files(directory)
            output = Path(directory) / 'out.json'
            tags.unlink()
            args = [str(tags), str(bsp), '--declared-map-length', '2048', '--bsp-ordinal', '0',
                    '--output', str(output)]
            for content in (None, b'bad'):
                if content is not None:
                    tags.write_bytes(content)
                stderr = io.StringIO()
                with redirect_stderr(stderr):
                    self.assertEqual(inspector.main(args), 1)
                self.assertNotIn(directory, stderr.getvalue())
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
