"""ADR-018 adoption helpers: engine flag set, wide-character gate, extraction
and the comparer's pinned classifications (authored inputs, no game data)."""
import unittest

import build
import compare_abi_semantics as compare
import run_abi_semantics as semantics


class EngineFlags(unittest.TestCase):
    def test_engine_semantics_are_the_adopted_rules(self):
        flags = build.ENGINE_SEMANTIC_FLAGS
        for flag in ("-fsigned-char", "-fshort-wchar", "-fno-strict-aliasing", "-fwrapv",
                     "-fno-delete-null-pointer-checks", "-fno-builtin-wcslen"):
            self.assertIn(flag, flags)
        self.assertNotIn("-funsigned-char", flags)
        self.assertEqual(semantics.ENGINE, flags)

    def test_every_suppressed_builtin_is_gated(self):
        for name in build.WIDE_BUILTINS:
            self.assertIn(name, build.WIDE_LIBRARY_FUNCTIONS)


class WideGate(unittest.TestCase):
    def test_nm_undefined_wide_symbols_are_reported(self):
        listing = "         U memcpy\n         U wcslen\n         U _wcsicmp\n         U towlower\n\n"
        self.assertEqual(build.wide_references_from_nm(listing), ["_wcsicmp", "towlower", "wcslen"])

    def test_narrow_symbols_pass(self):
        listing = "         U memcpy\n         U strlen\n         U vsnprintf\n         U halo_sin\n"
        self.assertEqual(build.wide_references_from_nm(listing), [])

    def test_object_header_lines_are_ignored(self):
        self.assertEqual(build.wide_references_from_nm("\nengine_probe.o:\n         U wmemcpy\n"), ["wmemcpy"])


class Extraction(unittest.TestCase):
    def test_statement_runs_to_its_top_level_semicolon(self):
        text = ("int before;\nstatic byte const sizes[NUMBEROF(table)] =\n{\n\t0,\n\tsizeof(struct x),\n};\n"
                "int after;\n")
        piece = semantics.extract_statement(text, "static byte const sizes[NUMBEROF(table)] =")
        self.assertTrue(piece.startswith("static byte const sizes"))
        self.assertTrue(piece.endswith("};"))
        self.assertNotIn("after", piece)

    def test_statement_typedef_with_parameter_list(self):
        text = "typedef void (*proc)(\n\tint a,\n\tchar const *b);\nint x;\n"
        self.assertEqual(semantics.extract_statement(text, "typedef void (*proc)("),
                         "typedef void (*proc)(\n\tint a,\n\tchar const *b);")

    def test_statement_anchor_must_be_unique(self):
        with self.assertRaises(ValueError):
            semantics.extract_statement("int a;\nint a;\n", "int a")


def report(entries):
    return {"entries": entries, "sha256": "", "begin": "", "end": "", "summary": ""}


class Classification(unittest.TestCase):
    def test_pinned_layout_difference_is_classified(self):
        key = "engine.layout.players_switch.state_byte_2_12"
        result = compare.compare(report({key: {"kind": "OBS", "value": "0xc2"}}),
                                 report({key: {"kind": "OBS", "value": "0x2c"}}))
        self.assertEqual(result["unexplained"], 0)
        self.assertEqual(result["classified"][0]["classification"], "in_memory_bitfield_layout")

    def test_other_values_stay_unexplained(self):
        key = "engine.layout.players_switch.state_byte_2_12"
        result = compare.compare(report({key: {"kind": "OBS", "value": "0xc2"}}),
                                 report({key: {"kind": "OBS", "value": "0x2d"}}))
        self.assertEqual(result["unexplained"], 1)
        self.assertEqual(result["classified"], [])

    def test_math_classification_needs_its_preconditions(self):
        def math(value, inputs="i"):
            return {"kind": "MATH", "value": value, "inputs": inputs, "count": "1", "blocks": "0" * 128,
                    "subnormal_inputs": 0, "subnormal_results": 0, "zero_results": 0, "nan_results": 1}
        host = {"musl.atan.f64": math("a"), "musl.atan.nan_canonical": math("c"),
                "musl.atan.narrowed_f32": math("n")}
        ppc = dict(host, **{"musl.atan.f64": math("b")})
        self.assertEqual(compare.compare(report(host), report(ppc))["unexplained"], 0)
        ppc["musl.atan.nan_canonical"] = math("d")
        result = compare.compare(report(host), report(ppc))
        self.assertEqual(sorted(item["id"] for item in result["differences"]),
                         ["musl.atan.f64", "musl.atan.nan_canonical"])


if __name__ == "__main__":
    unittest.main()
