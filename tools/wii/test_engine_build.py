"""Unit tests for the Wii engine build's tools (HWI-015): no toolchain, no game data."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))

import build  # noqa: E402
import engine_image  # noqa: E402
import engine_layout  # noqa: E402
import engine_rewrite  # noqa: E402
import engine_semantics  # noqa: E402
import engine_unsupported  # noqa: E402
from tools.wii import engine_build  # noqa: E402


class RewriteTests(unittest.TestCase):
    def test_msvc_suffixes_become_gcc_suffixes(self):
        text, count = engine_rewrite.rewrite("x = 0xff00000000ui64 | 5i64 | 7ui32 | 9i32;\n")
        self.assertEqual(text, "x = 0xff00000000ULL | 5LL | 7U | 9;\n")
        self.assertEqual(count, 4)

    def test_comments_strings_and_identifiers_are_left_alone(self):
        source = '/* 1ui64 */ char *s = "2i64"; // 3ui64\nint a1i64 = 4;\n'
        text, count = engine_rewrite.rewrite(source)
        self.assertEqual(text, source)
        self.assertEqual(count, 0)

    def test_the_rewritten_unit_still_has_suffixes_to_rewrite(self):
        # tools/wii/engine_build.py REWRITTEN must stay exact
        for unit in engine_build.REWRITTEN:  # paths relative to the checkout
            _, count = engine_rewrite.rewrite((ROOT / unit).read_text(encoding="latin-1"))
            self.assertGreater(count, 0, unit)


class SemanticsTests(unittest.TestCase):
    def test_static_inlines_get_no_weak_pragma_and_others_do(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.h").write_text(
                "static __inline int kept_static(int x) { return x; }\n"
                "__inline int shared(int x) { return x + 1; }\n"
                "D3DINLINE void __stdcall D3DDevice_Thing(DWORD a) { }\n"
                "struct tag_only;\n", encoding="ascii")
            text = engine_semantics.generate([root], [root], [root])
        self.assertIn("#pragma weak shared", text)
        self.assertIn("#pragma weak D3DDevice_Thing", text)
        self.assertNotIn("#pragma weak kept_static", text)
        self.assertIn("struct tag_only;", text)


class UnsupportedTests(unittest.TestCase):
    def test_committed_file_is_generated(self):
        committed = engine_unsupported.OUTPUT.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(committed, engine_unsupported.generate())

    def test_every_stub_reports_and_none_reports_success(self):
        text = engine_unsupported.generate()
        bodies = text.split("\n{\n")[1:]
        self.assertGreater(len(bodies), 150)
        for body in bodies:
            self.assertIn("wii_unsupported(", body.split("}")[0])
        # status returns where 0 would read as success
        self.assertEqual(engine_unsupported.failure("XInputGetState", "unsigned long __stdcall"),
                         "ERROR_DEVICE_NOT_CONNECTED")
        self.assertEqual(engine_unsupported.failure("WSAStartup", "int __stdcall"), "WSASYSNOTREADY")
        self.assertEqual(engine_unsupported.failure("halo_ws_socket", "unsigned int __stdcall"), "INVALID_SOCKET")
        self.assertEqual(engine_unsupported.failure("D3DDevice_CreateTexture", "long __stdcall"), "E_FAIL")
        self.assertEqual(engine_unsupported.failure("Direct3DCreate8", "struct Direct3D *__stdcall"), "NULL")
        self.assertIsNone(engine_unsupported.failure("D3DDevice_SetVertexData2f", "void __stdcall"))
        # an iteration ends: the engine's start-up walks modules until the end of the list
        self.assertEqual(engine_unsupported.failure("DmWalkLoadedModules", "HRESULT __stdcall"), "XBDM_ENDOFLIST")

    def test_parameters_are_named_in_declarators(self):
        self.assertEqual(engine_unsupported.named("void (__stdcall *)(void *)", 0), "void (__stdcall *a0)(void *)")
        self.assertEqual(engine_unsupported.named("float [4]", 2), "float a2[4]")
        self.assertEqual(engine_unsupported.named("struct X *", 1), "struct X * a1")


class GateTests(unittest.TestCase):
    def test_thread_local_sections_are_found(self):
        listing = ("  [ 5] .tbss             NOBITS          00000000 000040 000004 00 WAT  0   0  4\n"
                   "  [ 6] .bss              NOBITS          00000000 000040 000010 00  WA  0   0  4\n")
        self.assertTrue(build.thread_local_sections(listing))
        self.assertFalse(build.thread_local_sections(listing.splitlines()[1]))
        self.assertTrue(build.thread_local_sections("  [ 3] .tdata.counter     PROGBITS 0 0 4 00 WAT 0 0 4\n"))

    def test_read_list_ignores_blank_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "list.txt"
            path.write_text("a.o\n\n b.o \n", encoding="utf-8")
            self.assertEqual(build.read_list(path), ["a.o", "b.o"])


class PackagingTests(unittest.TestCase):
    def test_engine_source_drive_paths_are_reviewed_in_engine_artifacts_only(self):
        import package_artifacts

        data = b"\0c:\\halo\\SOURCE\\game\\game.c\0d:\\maps\\\0"
        findings, reviewed = [], {}
        package_artifacts.scan_content("engine.dol", data, [], [], findings, reviewed, True)
        self.assertEqual(findings, [])
        self.assertEqual(reviewed, {"engine.dol": 2})
        package_artifacts.scan_content("probe.dol", data, [], [], findings, {}, True)
        self.assertEqual({finding["category"] for finding in findings}, {"windows-drive-path"})
        findings = []
        package_artifacts.scan_content("engine.dol", b"\0C:\\Users\\someone\\x\0", [], [], findings, {}, True)
        self.assertEqual(len(findings), 1)


class TargetTests(unittest.TestCase):
    # tools/wii/engine_build.py works from the checkout's root, as configure.py runs
    def setUp(self):
        self.previous = os.getcwd()
        os.chdir(ROOT)

    def tearDown(self):
        os.chdir(self.previous)

    def test_units_replace_rewrite_and_add_authored_units(self):
        units = dict(engine_build.engine_units())
        self.assertEqual(units[Path("source/cache/physical_memory_map.c")],
                         Path("port/wii/engine/physical_memory_map_wii.c"))
        self.assertEqual(units[Path("source/memory/byte_swapping.c")],
                         engine_build.rewritten_path(Path("source/memory/byte_swapping.c")))
        for unit in engine_build.ENGINE_AUTHORED:
            self.assertIn(unit, units)
        self.assertGreater(len(units), 490)

    def test_engine_flags_are_engine_cflags_semantics(self):
        flags = engine_build.engine_flags(build.ENGINE_SEMANTIC_FLAGS)
        self.assertEqual(flags[:len(build.ENGINE_SEMANTIC_FLAGS)], build.ENGINE_SEMANTIC_FLAGS)
        self.assertIn("-fsigned-char", flags)
        self.assertIn("-fshort-wchar", flags)
        self.assertIn("-fno-unwind-tables", flags)

    def test_platform_sources_exist_and_objects_have_no_spaces(self):
        for path, _ in engine_build.platform_units():
            self.assertTrue((ROOT / path).is_file(), path)
        for unit, _ in engine_build.engine_units():
            self.assertNotIn(" ", engine_build.object_path(unit).as_posix())

    def test_wrapped_entry_points(self):
        for name in ("main", "halt_and_catch_fire", "game_tick", "game_frame"):
            self.assertIn(f"-Wl,--wrap={name}", engine_build.LINK_FLAGS)
        self.assertIn("-Wl,--gc-sections", engine_build.LINK_FLAGS)


MAP = """\
.text           0x80003fb0       0x40
 *(.text .text.*)
 .text.alpha    0x80003fb0       0x20 build/wii/engine/obj/source/game/game.o
 .text.a_very_long_function_name
                0x80003fd0       0x20 build/wii/engine/obj/port/wii/engine/wii_kernel.o
.bss            0x80004000      0x130
 .bss.custom_edition_maps_globals
                0x80004000      0x100 build/wii/engine/obj/port/linux/game/custom_edition_maps.o
 COMMON         0x80004100       0x30 build/wii/engine/obj/source/ai/ai.o
                0x80004100                ai_globals
"""


class ImageTests(unittest.TestCase):
    def test_map_sections_owners_and_allocations(self):
        report = engine_image.measure(MAP)
        self.assertEqual(report["image_end"], "0x80004130")
        self.assertEqual(report["bytes_by_kind"], {"bss": 0x130, "code": 0x40})
        self.assertEqual(report["bytes_by_owner"]["engine"], {"code": 0x20, "bss": 0x130})
        self.assertEqual(report["bytes_by_owner"]["wii_platform"], {"code": 0x20})
        names = [(item["name"], item["bytes"]) for item in report["largest_allocations"]]
        self.assertEqual(names, [("custom_edition_maps_globals", 0x100), ("ai_globals", 0x30)])
        self.assertEqual(report["named_groups"]["custom_edition_map_support"], {"bss": 0x100})
        self.assertEqual(report["mem1_after_image"], 0x81800000 - 0x80004130)


LISTING = """\
 <1><2d>: Abbrev Number: 5 (DW_TAG_structure_type)
    <2e>   DW_AT_name        : (indirect string, offset: 0x10): sample
    <32>   DW_AT_byte_size   : 8
 <2><36>: Abbrev Number: 6 (DW_TAG_member)
    <37>   DW_AT_name        : a
    <3b>   DW_AT_data_member_location: 0
 <2><40>: Abbrev Number: 7 (DW_TAG_member)
    <41>   DW_AT_name        : flags
    <45>   DW_AT_bit_size    : 3
    <46>   DW_AT_data_bit_offset: %d
 <1><50>: Abbrev Number: 5 (DW_TAG_structure_type)
    <51>   DW_AT_name        : forward
    <55>   DW_AT_declaration : 1
"""


class LayoutTests(unittest.TestCase):
    def test_dwarf_layouts_and_classification(self):
        ppc = engine_layout.dwarf_layouts((LISTING % 61).splitlines())
        host = engine_layout.dwarf_layouts((LISTING % 32).splitlines())
        key = ("", "struct", "sample")
        self.assertEqual(sorted(ppc), [key])
        self.assertEqual(engine_layout.classify(ppc[key], ppc[key]), "identical")
        self.assertEqual(engine_layout.classify(ppc[key], host[key]), "bitfield_order")
        other = engine_layout.dwarf_layouts((LISTING % 32).replace("byte_size   : 8", "byte_size   : 12").splitlines())
        self.assertEqual(engine_layout.classify(ppc[key], other[key]), "different")
        report = engine_layout.compare_dwarf(ppc, other)
        self.assertEqual(report["classes"], {"different": 1})

    def test_layouts_are_keyed_by_compile_unit(self):
        unit = (" <0><b>: Abbrev Number: 1 (DW_TAG_compile_unit)\n"
                "    <c>   DW_AT_name        : (indirect string, offset: 0x5): source/game/game.c\n")
        layouts = engine_layout.dwarf_layouts((unit + LISTING % 61).splitlines())
        self.assertEqual(sorted(layouts), [("source/game/game.c", "struct", "sample")])

    def test_census_table_rows_parse(self):
        rows = engine_layout.i686_rows(ROOT / "tools/wii/game_state_census_table.c")
        self.assertGreater(len(rows), 70)
        self.assertEqual(rows[0][0], "header")


if __name__ == "__main__":
    unittest.main()
