"""Authored geometry-section selection fixtures: no owned assets or runtime."""
import struct
import unittest

import select_geometry_section as selector
from test_inspect_material_graph import Fixture

SURFACES = 5000
POSITIONS = 4700
CORNERS = ((0, 1, 1), (1, 0, 0), (0, 1, 0), (1, 1, 1), (0, 0, 1))
BITS = ((0x3F800000, 0xC0200000, 0x80000000), (0x00000001, 0x42C80000, 0xBF800000))


def authored():
    f = Fixture()
    for ordinal, corners in enumerate(CORNERS):
        struct.pack_into('<3H', f.bsp, SURFACES + ordinal * 6, *corners)
    for vertex, bits in enumerate(BITS):
        struct.pack_into('<3I', f.bsp, POSITIONS + vertex * 32, *bits)
    return f


def select(f, material='auto'):
    return selector.select_bytes(bytes(f.tags), bytes(f.bsp), f.map_bytes, 0, material)


class SelectionTests(unittest.TestCase):
    def test_rule_selects_compressed_environment_material_and_validates_section(self):
        f = authored()
        before = bytes(f.tags), bytes(f.bsp)
        result = select(f)
        self.assertEqual(result['rule_choice'], 0)
        self.assertTrue(result['selected_matches_rule'])
        self.assertEqual(result['selected']['surface_count'], 5)
        self.assertEqual(result['selected']['environment_vertices'], 2)
        validation = result['validation']
        self.assertEqual(validation['degenerate_triangles'], 5)
        self.assertEqual(validation['referenced_vertices'], 2)
        self.assertEqual(validation['subnormal_position_words'], 1)
        self.assertEqual(validation['negative_zero_position_words'], 1)
        self.assertEqual(result['extents']['min'][1], -2.5)
        self.assertEqual(result['extents']['max'][1], 100.0)
        self.assertEqual(result['consumed_bytes']['native_positions_f32'], 24)
        self.assertEqual((bytes(f.tags), bytes(f.bsp)), before)

    def test_repeated_selection_is_identical(self):
        self.assertEqual(select(authored()), select(authored()))

    def test_index_hash_changes_with_one_corner(self):
        f = authored()
        first = select(f)['hashes']
        struct.pack_into('<H', f.bsp, SURFACES + 4, 0)
        second = select(f)['hashes']
        self.assertNotEqual(first['indices_sha256'], second['indices_sha256'])
        self.assertEqual(first['positions_sha256'], second['positions_sha256'])

    def test_out_of_bounds_index_rejects(self):
        f = authored()
        struct.pack_into('<H', f.bsp, SURFACES + 2, 2)
        with self.assertRaisesRegex(selector.InspectionError, 'triangle index'):
            select(f)

    def test_infinity_and_nan_positions_reject(self):
        for bits in (0x7F800000, 0xFF800000, 0x7FC00001, 0x7F800001):
            f = authored()
            struct.pack_into('<I', f.bsp, POSITIONS + 32 + 8, bits)
            with self.assertRaisesRegex(selector.InspectionError, 'non-finite'):
                select(f)

    def test_uncompressed_material_rejects(self):
        with self.assertRaisesRegex(selector.InspectionError, 'not compressed environment'):
            select(authored(), 1)

    def test_unknown_material_rejects(self):
        with self.assertRaisesRegex(selector.InspectionError, 'outside the published graph'):
            select(authored(), 2)


if __name__ == '__main__':
    unittest.main()
