"""Tests for the game-state census and schema-export tools (no compiler, no game data)."""
from pathlib import Path
import re
import unittest

import export_tag_schema
import game_state_census as census

ROOT = Path(__file__).resolve().parents[2]


class CensusSites(unittest.TestCase):
    def test_every_allocation_call_site_is_listed(self):
        self.assertEqual(census.check_sites(), [])
        self.assertGreaterEqual(len(census.source_call_sites()), 72)

    def test_rows_have_known_kinds_and_positive_multiplicity(self):
        for label, source, kind, count, size, multiplicity, call in census.SITES:
            self.assertIn(kind, census.KIND_CODES, label)
            self.assertGreaterEqual(multiplicity, 1, label)
            self.assertTrue((ROOT / source).is_file(), source)
            if call:
                self.assertTrue((ROOT / call).is_file(), call)

    def test_c_table_lists_each_row_with_its_measured_bytes(self):
        result = {
            "sources_sha256": "0" * 64, "compiler": "test",
            "sites": [{"label": 'a "quoted" name', "kind": "data", "count": 2, "element_size": 12,
                       "bytes_each": 80, "multiplicity": 3}],
            "cpu_requested": 240, "gpu_requested": 0,
            "fixed": {"sizeof(struct data_array)": 56, "sizeof(struct memory_pool)": 56},
        }
        text = census.c_table(result)
        self.assertIn('\t{"a \\"quoted\\" name", 1, 2, 12, 80, 3},', text)
        self.assertIn("const unsigned gs_census_row_count = 1u;", text)
        self.assertIn("const unsigned long gs_census_data_array_header = 56ul;", text)

    def test_committed_table_matches_the_census_rows(self):
        text = (ROOT / "tools/wii/game_state_census_table.c").read_text(encoding="utf-8")
        rows = re.findall(r'^\t\{"((?:[^"\\]|\\.)*)", (\d+), (\d+), (\d+), (\d+), (\d+)\},$', text, re.M)
        self.assertEqual([row[0] for row in rows], [site[0] for site in census.SITES])
        for row, site in zip(rows, census.SITES):
            self.assertEqual(int(row[1]), census.KIND_CODES[site[2]], site[0])
            self.assertEqual(int(row[5]), site[5], site[0])
        cpu = sum(int(row[4]) * int(row[5]) for row in rows if int(row[1]) != census.KIND_CODES["gpu"])
        self.assertIn(f"gs_census_cpu_requested = {cpu}ul;", text)


class SchemaTables(unittest.TestCase):
    def test_generated_tables_name_the_current_schema_sources(self):
        text = (ROOT / "tools/wii/cache_schema_tables.c").read_text(encoding="utf-8")
        self.assertIn(f"schema sources SHA-256 {export_tag_schema.schema_sources_hash(ROOT)}", text)

    def test_counts_agree_with_the_tables(self):
        text = (ROOT / "tools/wii/cache_schema_tables.c").read_text(encoding="utf-8")
        counts = re.search(r"cache_schema_counts = \{(\d+), (\d+), (\d+), (\d+), (\d+)\};", text)
        fields = re.findall(r"^\t\{-?\d+, -?\d+, -?\d+, -?\d+, -?\d+, -?\d+, -?\d+, -?\d+, -?\d+\},$", text, re.M)
        definitions = re.findall(r"^\t\{\d+, \d+, \d+\}, /\* \w+ \*/$", text, re.M)
        pointers = re.findall(r"^\t\{\d+, \d+\}, /\* \w+ \*/$", text, re.M)
        self.assertIsNotNone(counts)
        self.assertEqual(int(counts.group(1)), len(fields))
        self.assertEqual(int(counts.group(2)), len(definitions))
        self.assertEqual(int(counts.group(5)), len(pointers))


if __name__ == "__main__":
    unittest.main()
