"""Authored material metadata fixtures: no owned assets, compilers or runtime."""
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

import inspect_material_graph as inspector


class Fixture:
    BASE = inspector.BASE
    BSP_BASE = BASE + 16384
    ROOT = 128
    LIGHTMAPS = 800
    MATERIALS = (1024, 1280)
    DATUM = 0x34560002
    GROUP = 0x73656E76

    def __init__(self):
        self.tags = bytearray(8192)
        self.bsp = bytearray((n * 13 + 7) % 256 for n in range(8192))
        self.map_bytes = 131072
        struct.pack_into('<9I', self.tags, 0, self.BASE + 36, 0x12340000, 0xDEADBEEF,
                         3, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0x74616773)
        for ordinal, group, root, name in ((0, 0x73636E72, 256, 6000),
                                           (1, 0x73627370, 0, 6032), (2, self.GROUP, 7000, 6064)):
            struct.pack_into('<8I', self.tags, 36 + ordinal * 32, group, inspector.SHADER_GROUP,
                             0xFFFFFFFF, (0x12340000, 0x23450001, self.DATUM)[ordinal],
                             self.BASE + name, self.BASE + root if root else 0, 0, 0)
            self.tags[name:name + 12] = b'AUTHORED-X\0\0'
        self.tags[6100:6112] = b'PRIVATE-NAME'
        self.tags[6112] = 0
        struct.pack_into('<iII', self.tags, 256 + 0x5A4, 1, self.BASE + 2048, 0x12345678)
        struct.pack_into('<ii6I', self.tags, 2048, 2048, len(self.bsp), self.BSP_BASE,
                         0xCAFEBABE, 0x73627370, self.BASE + 6032, 0, 0x23450001)
        struct.pack_into('<6I', self.bsp, 0, self.BSP_BASE + self.ROOT,
                         2, self.BSP_BASE + 24, 2, self.BSP_BASE + 48, 0x73627370)
        for kind, table, payloads in ((0, 24, (4096, 4300)), (1, 48, (4200, 4500))):
            for ordinal, payload in enumerate(payloads):
                struct.pack_into('<3I', self.bsp, table + ordinal * 12,
                                 0x80000000 | kind, self.BSP_BASE + payload, 0xABCDEF00 | ordinal)
        struct.pack_into('<iII', self.bsp, self.ROOT + 248, 10, self.BSP_BASE + 5000, 0xFFFFFFFF)
        struct.pack_into('<iII', self.bsp, self.ROOT + 260, 2,
                         self.BSP_BASE + self.LIGHTMAPS, 0xDEADBEEF)
        for i, material in enumerate(self.MATERIALS):
            lm = self.LIGHTMAPS + i * 32
            struct.pack_into('<hH', self.bsp, lm, -1, 0xA000 + i)
            struct.pack_into('<iII', self.bsp, lm + 20, 1, self.BSP_BASE + material, 0x12345678 + i)
            struct.pack_into('<4IhHii3I', self.bsp, material, self.GROUP, self.BASE + 6100,
                             (0, 0xFFFFFFFF)[i], self.DATUM, -2, 0x8000 + i, i * 5, 5,
                             0x7FC00001, 0x80000000, 0x3F800000)
            for kind, count, vertex_type in ((0, (2, 3)[i], (1, 0)[i]), (1, (1, 2)[i], (3, 2)[i])):
                struct.pack_into('<hHiiII', self.bsp, material + (176, 196)[kind],
                                 vertex_type, 0xABCD, count, -123, 0xFFFFFFFF,
                                 self.BSP_BASE + (24, 48)[kind] + i * 12)
            for kind, size, address in ((0, 0, 0xFFFFFFFF), (1, (72, 112)[i], self.BSP_BASE + 4700)):
                struct.pack_into('<iIiII', self.bsp, material + (216, 236)[kind],
                                 size, 0x87654321, -99, address, 0xDEADBEEF)

    def word(self, offset, value, tag=False):
        struct.pack_into('<I', self.tags if tag else self.bsp, offset, value & 0xFFFFFFFF)

    def half(self, offset, value):
        struct.pack_into('<H', self.bsp, offset, value & 0xFFFF)

    def inspect(self):
        return inspector.inspect_bytes(bytes(self.tags), bytes(self.bsp), self.map_bytes, 0)


class MaterialTests(unittest.TestCase):
    def rejects(self, fixture, reason):
        with self.assertRaisesRegex(inspector.InspectionError, reason):
            fixture.inspect()

    def test_full_graph_independent_raw_concat_and_source_immutability(self):
        f = Fixture()
        before = bytes(f.tags), bytes(f.bsp)
        result = f.inspect()
        raw = before[1][128:776] + before[1][800:864] + before[1][1024:1280] + before[1][1280:1536]
        self.assertEqual(result['goldens'], dict(scenario_datum=0x12340000, bsp_datum=0x23450001,
            lightmap_count=2, material_count=2, environment_vertices=5, lightmap_vertices=3,
            serialized_bytes=1224, serialized_crc32=zlib.crc32(raw),
            serialized_sha256=hashlib.sha256(raw).hexdigest()))
        self.assertEqual(result['vertex_type_counts'], [1, 1, 1, 1])
        self.assertEqual(result['totals']['environment_payload_bytes'], 64 + 168)
        self.assertEqual(result['totals']['lightmap_payload_bytes'], 8 + 40)
        self.assertEqual(result['totals']['centroid_nonzero_bit_words'], 6)
        self.assertEqual((bytes(f.tags), bytes(f.bsp)), before)

    def test_opaque_unknown_words_preserved_by_concat_identity(self):
        f = Fixture()
        old = f.inspect()
        f.bsp[Fixture.MATERIALS[0] + 100] ^= 0x80
        new = f.inspect()
        self.assertNotEqual(old['goldens']['serialized_sha256'], new['goldens']['serialized_sha256'])
        self.assertEqual(old['numeric_projection_sha256'], new['numeric_projection_sha256'])

    def test_numeric_metadata_words_including_negative_offsets_are_hashed(self):
        f = Fixture()
        old = f.inspect()
        f.word(1024 + 184, -124)
        self.assertNotEqual(old['numeric_projection_sha256'], f.inspect()['numeric_projection_sha256'])

    def test_NONE_reference_does_not_follow_unused_group_names_or_length(self):
        f = Fixture()
        struct.pack_into('<4I', f.bsp, 1024, 0x12345678, 0xFFFFFFFF, 0x80000000, 0xFFFFFFFF)
        r = f.inspect()
        self.assertEqual(r['totals']['shader_NONE'], 1)
        self.assertEqual(r['totals']['shader_identity_checked'], 1)

    def test_reference_name_length_zero_and_negative_word_are_unused(self):
        f = Fixture()
        self.assertEqual(f.inspect()['totals']['shader_identity_checked'], 2)
        for value in (0, 0x80000000, 0x7FFFFFFF):
            f.word(1024 + 8, value)
            self.assertEqual(f.inspect()['totals']['shader_identity_checked'], 2)

    def test_shader_full_salt_and_ordinal_are_checked(self):
        for datum in (Fixture.DATUM ^ 0x10000, 0x12340003):
            f = Fixture(); f.word(1024 + 12, datum)
            self.rejects(f, 'datum')

    def test_primary_group_must_equal_reference_group(self):
        f = Fixture(); f.word(1024, 0x73676C61)
        self.rejects(f, 'group')

    def test_shader_primary_or_either_ancestor_accepts(self):
        for slot in (0, 1, 2):
            f = Fixture()
            f.word(100, inspector.SHADER_GROUP if slot == 0 else f.GROUP, True)
            f.word(104, inspector.SHADER_GROUP if slot == 1 else 0xFFFFFFFF, True)
            f.word(108, inspector.SHADER_GROUP if slot == 2 else 0xFFFFFFFF, True)
            f.word(1024, inspector.SHADER_GROUP if slot == 0 else f.GROUP)
            f.word(1280, inspector.SHADER_GROUP if slot == 0 else f.GROUP)
            self.assertEqual(f.inspect()['totals']['shader_identity_checked'], 2)

    def test_nonshader_target_rejects(self):
        f = Fixture(); f.word(104, 0xFFFFFFFF, True)
        self.rejects(f, 'not shader')

    def test_record_and_instance_names_confined_to_tag_window(self):
        for tag, offset in ((False, 1028), (True, 116)):
            for address in (Fixture.BSP_BASE + 4096, Fixture.BASE + 9000, Fixture.BASE - 1):
                f = Fixture(); f.word(offset, address, tag)
                self.rejects(f, 'region|window')

    def test_bounded_NUL_required_for_both_names(self):
        for tag, offset in ((False, 1028), (True, 116)):
            f = Fixture(); f.tags[-1] = 65; f.word(offset, f.BASE + len(f.tags) - 1, tag)
            self.rejects(f, 'unterminated')

    def test_name_scan_cache_and_diagnostic_limit(self):
        f = Fixture()
        self.assertEqual(f.inspect()['name_scan']['distinct_names'], 2)
        with patch('inspect_widget_graph.MAX_NAME_SCAN', 1):
            self.rejects(f, 'scan limit')

    def test_lightmap_and_material_signed_counts_and_source_limits(self):
        for offset, values in ((Fixture.ROOT + 260, (-1, 129, 0x7FFFFFFF)),
                               (Fixture.LIGHTMAPS + 20, (-1, 2049, 0x7FFFFFFF))):
            for value in values:
                f = Fixture(); f.word(offset, value)
                self.rejects(f, 'signed count/source limit')

    def test_positive_block_address_span_and_overflow_rejects(self):
        for offset in (Fixture.ROOT + 264, Fixture.LIGHTMAPS + 24):
            for address in (0xFFFFFFFF, Fixture.BASE + 4096, Fixture.BSP_BASE + 8191):
                f = Fixture(); f.word(offset, address)
                self.rejects(f, 'overflow|region|window')

    def test_zero_lightmaps_ignore_pointer_and_serialize_root_only(self):
        f = Fixture(); f.word(Fixture.ROOT + 260, 0); f.word(Fixture.ROOT + 264, 0xFFFFFFFF)
        r = f.inspect()
        self.assertEqual(r['goldens']['material_count'], 0)
        self.assertEqual(r['goldens']['serialized_bytes'], 648)

    def test_zero_material_block_ignores_pointer(self):
        f = Fixture(); f.word(820, 0); f.word(824, 0xFFFFFFFF)
        f.word(1280 + 20, 0)
        r = f.inspect()
        self.assertEqual(r['goldens']['material_count'], 1)
        self.assertEqual(r['goldens']['serialized_bytes'], 968)

    def test_all_known_metadata_claims_are_disjoint(self):
        for offset, address in ((Fixture.ROOT + 264, Fixture.BSP_BASE),
                                (Fixture.ROOT + 264, Fixture.BSP_BASE + 24),
                                (Fixture.LIGHTMAPS + 24, Fixture.BSP_BASE + Fixture.ROOT),
                                (Fixture.LIGHTMAPS + 24, Fixture.BSP_BASE + Fixture.LIGHTMAPS),
                                (Fixture.LIGHTMAPS + 32 + 24, Fixture.BSP_BASE + 1024)):
            f = Fixture(); f.word(offset, address)
            self.rejects(f, 'overlap')

    def test_surface_signed_root_limit_and_range_extent(self):
        for offset, value in ((Fixture.ROOT + 248, -1), (Fixture.ROOT + 248, 131073),
                              (1024 + 20, -1), (1024 + 24, -1), (1024 + 24, 11),
                              (1280 + 20, 6), (1280 + 24, 0x7FFFFFFF)):
            f = Fixture(); f.word(offset, value)
            self.rejects(f, 'signed count|surface range')

    def test_repeated_overlapping_and_out_of_order_surface_ranges_accept(self):
        for first, second in ((0, 0), (0, 4), (5, 0)):
            f = Fixture(); f.word(1024 + 20, first); f.word(1280 + 20, second)
            r = f.inspect()
            self.assertEqual(r['totals']['surface_count'], 10)
            self.assertEqual(r['goldens']['material_count'], 2)

    def test_zero_surface_span_ignores_pointer(self):
        f = Fixture(); f.word(Fixture.ROOT + 248, 0); f.word(Fixture.ROOT + 252, 0xFFFFFFFF)
        for offset in f.MATERIALS:
            f.word(offset + 20, 0); f.word(offset + 24, 0)
        self.assertEqual(f.inspect()['totals']['surface_count'], 0)

    def test_vertex_signed_count_source_max_and_types(self):
        for offset, value, half in ((1024 + 180, -1, False), (1024 + 180, 64001, False),
                                     (1024 + 200, 64001, False), (1024 + 176, -1, True),
                                     (1024 + 176, 2, True), (1024 + 196, 1, True)):
            f = Fixture()
            (f.half if half else f.word)(offset, value)
            self.rejects(f, 'vertex type|signed count')

    def test_hardware_NULL_allowed_without_resource_extent_proof(self):
        f = Fixture()
        for offset in f.MATERIALS:
            f.word(offset + 192, 0); f.word(offset + 212, 0)
        r = f.inspect()
        self.assertEqual(r['totals']['hardware_null'], 4)
        self.assertEqual(r['totals']['environment_payload_bytes'], 0)

    def test_hardware_membership_alignment_and_wrong_kind(self):
        for address in (Fixture.BSP_BASE + 48, Fixture.BSP_BASE + 25,
                         Fixture.BSP_BASE + 24 + 24, Fixture.BASE + 36, 0xFFFFFFFF):
            f = Fixture(); f.word(1024 + 192, address)
            self.rejects(f, 'descriptor table')

    def test_type_indexed_payload_extents_not_fixed_compressed_stride(self):
        f = Fixture()
        f.word(36 + 4, f.BSP_BASE + len(f.bsp) - 100)
        self.rejects(f, 'window')  # type0 count3 requires168, not96
        f = Fixture(); f.word(60 + 4, f.BSP_BASE + len(f.bsp) - 20)
        self.rejects(f, 'window')  # type2 count2 requires40, not16

    def test_payload_wrong_window_gap_and_encoded_overflow(self):
        for address in (Fixture.BASE + 5000, Fixture.BASE + 10000, 0xFFFFFFFE):
            f = Fixture(); f.word(28, address)
            self.rejects(f, 'overflow|region|window')

    def test_zero_vertex_count_retains_resource_whose_Data_is_root(self):
        f = Fixture(); f.word(1024 + 200, 0); f.word(52, f.BSP_BASE + f.ROOT)
        r = f.inspect()
        self.assertEqual(r['totals']['zero_count_payloads'], 1)
        self.assertEqual(r['totals']['hardware_identity_checked'], 4)

    def test_tag_data_signed_size_positive_bounds_and_zero_pointer_ignore(self):
        for value in (-1, 0x7FFFFFFF):
            f = Fixture(); f.word(1024 + 236, value)
            self.rejects(f, 'signed size|window|overflow')
        for address in (Fixture.BASE + 4000, Fixture.BASE + 10000, 0xFFFFFFFF):
            f = Fixture(); f.word(1024 + 248, address)
            self.rejects(f, 'overflow|region|window')
        self.assertEqual(Fixture().inspect()['totals']['uncompressed_data_bytes'], 0)

    def test_Xbox_minimum_allows_extra_bytes_and_rejects_one_less(self):
        f = Fixture(); f.word(1024 + 236, 73)
        self.assertEqual(f.inspect()['totals']['compressed_data_bytes'], 185)
        f.word(1024 + 236, 71)
        self.rejects(f, 'compressed count minimum')

    def test_source_tag_data_exact_limits_accept_and_one_over_rejects(self):
        for relative, exact in ((216, 4864000), (236, 2560000)):
            f = Fixture()
            f.bsp.extend(bytes(exact + 1 - len(f.bsp)))
            f.word(2048 + 4, len(f.bsp), True)
            f.map_bytes = exact + 4096
            f.word(1024 + relative, exact)
            f.word(1024 + relative + 12, f.BSP_BASE)
            r = f.inspect()
            self.assertEqual(r['totals'][('uncompressed_data_bytes' if relative == 216
                                          else 'compressed_data_bytes')],
                             exact if relative == 216 else exact + 112)
            f.word(1024 + relative, exact + 1)
            self.rejects(f, 'tag data signed size/source limit')

    def test_compact_numeric_output_no_asset_names_or_per_material_bytes(self):
        r = Fixture().inspect(); text = json.dumps(r)
        self.assertNotIn('PRIVATE-NAME', text)
        self.assertNotIn('AUTHORED-X', text)
        self.assertLess(len(text), inspector.MAX_OUTPUT_BYTES)
        self.assertNotIn('materials', r)
        with patch.object(inspector, 'MAX_OUTPUT_BYTES', 10):
            self.rejects(Fixture(), 'output size')

    def test_all_2048_materials_validate_without_per_material_output_growth(self):
        f = Fixture()
        template = bytearray(f.bsp[1024:1280])
        struct.pack_into('<4I', template, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0x80000000, inspector.NONE)
        struct.pack_into('<ii', template, 20, 0, 0)
        for relative in (176, 196):
            struct.pack_into('<i', template, relative + 4, 0)
            struct.pack_into('<I', template, relative + 16, 0)
        for relative in (216, 236):
            struct.pack_into('<i', template, relative, 0)
            struct.pack_into('<I', template, relative + 12, 0xFFFFFFFF)
        f.bsp = f.bsp[:1024] + template * 2048
        f.word(2048 + 4, len(f.bsp), True)
        f.map_bytes = 1024 * 1024
        f.word(f.ROOT + 260, 1)
        f.word(f.LIGHTMAPS + 20, 2048)
        r = f.inspect()
        self.assertEqual(r['goldens']['material_count'], 2048)
        self.assertEqual(r['totals']['shader_NONE'], 2048)
        self.assertEqual(r['goldens']['serialized_bytes'], 648 + 32 + 2048 * 256)
        self.assertLess(len(json.dumps(r)), 10000)
        f.word(1024 + 2047 * 256 + 180, -1)
        self.rejects(f, 'signed count')

    def test_all_128_lightmaps_supported_without_truncation(self):
        f = Fixture(); f.word(f.ROOT + 260, 128)
        for index in range(128):
            struct.pack_into('<iII', f.bsp, 800 + index * 32 + 20, 0, 0xFFFFFFFF, index)
        r = f.inspect()
        self.assertEqual(len(r['lightmaps']), 128)
        self.assertEqual(r['goldens']['material_count'], 0)
        self.assertEqual(r['goldens']['serialized_bytes'], 648 + 128 * 32)

    def test_aggregate_data_total_rejects_uint32_overflow_without_wrapping(self):
        f = Fixture()
        template = bytearray(f.bsp[1024:1280])
        struct.pack_into('<4I', template, 0, 0, 0, 0, inspector.NONE)
        struct.pack_into('<ii', template, 20, 0, 0)
        for relative in (176, 196):
            struct.pack_into('<i', template, relative + 4, 0)
            struct.pack_into('<I', template, relative + 16, 0)
        struct.pack_into('<i', template, 236, 2 * 1024 * 1024)
        struct.pack_into('<I', template, 248, f.BSP_BASE)
        f.bsp = f.bsp[:1024] + template * 2048
        f.bsp.extend(bytes(2 * 1024 * 1024 - len(f.bsp)))
        f.word(2048 + 4, len(f.bsp), True)
        f.map_bytes = 3 * 1024 * 1024
        f.word(f.ROOT + 260, 1); f.word(f.LIGHTMAPS + 20, 2048)
        self.rejects(f, 'aggregate numeric total overflow')

    def test_surface_count_uses_root_limit_without_inventing_material_cap(self):
        f = Fixture()
        f.bsp.extend(bytes(140000))
        f.word(2048 + 4, len(f.bsp), True)
        f.map_bytes = 200000
        f.word(f.ROOT + 248, 20001)
        f.word(f.ROOT + 252, f.BSP_BASE + 8192)
        f.word(1024 + 24, 20001)
        f.word(1280 + 20, 20001); f.word(1280 + 24, 0)
        self.assertEqual(f.inspect()['totals']['surface_count'], 20001)

    def test_invalid_immutable_inputs_and_parent_identity_proof_preserved(self):
        f = Fixture()
        with self.assertRaises(inspector.InspectionError):
            inspector.inspect_bytes(f.tags, bytes(f.bsp), f.map_bytes, 0)
        f.word(2048 + 28, 0x99990001, True)
        self.rejects(f, 'datum')


class FileTests(unittest.TestCase):
    def write_fixture(self, folder):
        f = Fixture(); tags = folder / 'secret-tags'; bsp = folder / 'secret-bsp'
        tags.write_bytes(f.tags); bsp.write_bytes(f.bsp)
        return f, tags, bsp

    def test_external_snapshot_provenance_and_input_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp))
            result = inspector.inspect_files(tags, bsp, f.map_bytes, 0)
            self.assertEqual(tags.read_bytes(), bytes(f.tags))
            self.assertEqual(bsp.read_bytes(), bytes(f.bsp))
            self.assertIn('tools/wii/inspect_material_graph.py', result['source_hashes'])
            self.assertNotIn(tmp, json.dumps(result))

    def test_checkout_inputs_and_outputs_reject(self):
        with self.assertRaisesRegex(inspector.InspectionError, 'outside'):
            inspector.external_path(inspector.CHECKOUT / 'forbidden.json')

    def test_changed_source_during_inspection_rejects(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); real = inspector.inspect_bytes
            def changed(*args):
                result = real(*args); tags.write_bytes(tags.read_bytes() + b'!'); return result
            with patch.object(inspector, 'inspect_bytes', side_effect=changed):
                with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                    inspector.inspect_files(tags, bsp, f.map_bytes, 0)

    def test_changed_BSP_during_inspection_rejects(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); real = inspector.inspect_bytes
            def changed(*args):
                result = real(*args); bsp.write_bytes(bsp.read_bytes() + b'!'); return result
            with patch.object(inspector, 'inspect_bytes', side_effect=changed):
                with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                    inspector.inspect_files(tags, bsp, f.map_bytes, 0)

    def test_changed_path_inode_rejected_with_same_size_and_mtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); real = inspector.inspect_bytes
            original_stat = Path.stat
            inspected = False
            def changed(*args):
                nonlocal inspected
                result = real(*args); inspected = True; return result
            def changed_stat(path, *args, **kwargs):
                s = original_stat(path, *args, **kwargs)
                if not inspected or path != tags:
                    return s
                return SimpleNamespace(st_dev=s.st_dev, st_ino=s.st_ino + 1,
                    st_size=s.st_size, st_mtime_ns=s.st_mtime_ns, st_ctime_ns=s.st_ctime_ns)
            with patch.object(inspector, 'inspect_bytes', side_effect=changed), patch.object(Path, 'stat', changed_stat):
                with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                    inspector.inspect_files(tags, bsp, f.map_bytes, 0)

    def test_descriptor_ctime_change_is_rejected_even_stable_cross_API_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); original = inspector.os.fstat
            calls = 0
            def changed(fd):
                nonlocal calls
                calls += 1
                s = original(fd)
                return SimpleNamespace(st_dev=s.st_dev, st_ino=s.st_ino, st_size=s.st_size,
                                       st_mtime_ns=s.st_mtime_ns, st_ctime_ns=s.st_ctime_ns + (calls > 2))
            with patch.object(inspector.os, 'fstat', side_effect=changed):
                with self.assertRaisesRegex(inspector.InspectionError, 'changed'):
                    inspector.inspect_files(tags, bsp, f.map_bytes, 0)

    def test_same_API_ctime_guard_without_cross_API_comparison(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); original = inspector.os.fstat
            def different_ctime(fd):
                s = original(fd)
                return SimpleNamespace(st_dev=s.st_dev, st_ino=s.st_ino, st_size=s.st_size,
                                       st_mtime_ns=s.st_mtime_ns, st_ctime_ns=s.st_ctime_ns + 100)
            with patch.object(inspector.os, 'fstat', side_effect=different_ctime):
                self.assertEqual(inspector.inspect_files(tags, bsp, f.map_bytes, 0)['goldens']['material_count'], 2)

    def test_bounded_file_size_before_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); tags.write_bytes(b'X' * 35)
            with self.assertRaisesRegex(inspector.InspectionError, 'size'):
                inspector.inspect_files(tags, bsp, f.map_bytes, 0)

    def test_exclusive_fsynced_publication_and_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'numeric.json'
            with patch('inspect_widget_graph.os.fsync', wraps=inspector.os.fsync) as fsync:
                inspector.write_exclusive(output, Fixture().inspect())
                self.assertEqual(fsync.call_count, 1)
            before = output.read_bytes()
            with self.assertRaises(FileExistsError):
                inspector.write_exclusive(output, {'replacement': 1})
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual([p.name for p in Path(tmp).iterdir()], ['numeric.json'])

    def test_publication_link_failure_does_not_leave_partial_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'numeric.json'
            with patch('inspect_widget_graph.os.link', side_effect=OSError(1, 'test')):
                with self.assertRaises(OSError):
                    inspector.write_exclusive(output, Fixture().inspect())
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_CLI_success_existing_output_and_path_sanitized_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            f, tags, bsp = self.write_fixture(Path(tmp)); output = Path(tmp) / 'numeric.json'
            args = [str(tags), str(bsp), '--declared-map-length', str(f.map_bytes),
                    '--bsp-ordinal', '0', '--output', str(output)]
            with redirect_stdout(io.StringIO()):
                self.assertEqual(inspector.main(args), 0)
            with redirect_stderr(io.StringIO()) as stderr:
                self.assertEqual(inspector.main(args), 1)
            self.assertNotIn(tmp, stderr.getvalue())
            output.unlink(); bsp.unlink()
            with redirect_stderr(io.StringIO()) as stderr:
                self.assertEqual(inspector.main(args), 1)
            self.assertNotIn(tmp, stderr.getvalue())
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
