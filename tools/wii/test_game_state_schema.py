"""HWI-085: the typed game-state image's production schema and codec (source/saved games/game_state_schema.c),
compiled for the host and checked against wire bytes specified by hand (game_state_schema_check.c), and the
image's dispatch around it (game_state_image.c, game_state.c): version 2 is written and read as it was, a
version-3 image is written only when the completeness gate passes and refused before anything is taken."""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAVED = ROOT / "source" / "saved games"
IMAGE = SAVED / "game_state_image.c"
IMAGE_HEADER = SAVED / "game_state_image.h"
GAME_STATE = SAVED / "game_state.c"
SCHEMA = SAVED / "game_state_schema.c"
CLASSES = SAVED / "game_state_schema_classes.c"


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


def function_body(text, name):
    start = re.search(r"^\w[\w *]*\b" + name + r"\(\n", text, re.M)
    if not start:
        raise AssertionError(name)
    end = text.index("\n}\n", start.end())
    return text[start.end():end]


@unittest.skipUnless(compiler(), "no host C compiler")
class CodecFixtureTests(unittest.TestCase):
    def test_production_codec_against_specified_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            name = "game_state_schema_check.exe" if sys.platform == "win32" else "game_state_schema_check"
            exe = Path(temp) / name
            build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(ROOT / "source"), "-o", str(exe),
                                    str(ROOT / "tools" / "wii" / "game_state_schema_check.c")],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-4000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            match = re.fullmatch(r"ok (\d+) cases\s*", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            self.assertGreaterEqual(int(match.group(1)), 120)


class DispatchTests(unittest.TestCase):
    def setUp(self):
        self.image = IMAGE.read_text(encoding="utf-8")
        self.header = IMAGE_HEADER.read_text(encoding="utf-8")
        self.game_state = GAME_STATE.read_text(encoding="utf-8")

    def test_saves_stay_version_2(self):
        self.assertIn("GAME_STATE_IMAGE_VERSION = 2,", self.header)
        self.assertIn("GAME_STATE_IMAGE_TYPED_VERSION = 3,", self.header)
        self.assertRegex(self.header, r"#define GAME_STATE_IMAGE_WRITE_VERSION GAME_STATE_IMAGE_VERSION\n")
        body = function_body(self.game_state, "game_state_port_image_write")
        self.assertIn("GAME_STATE_IMAGE_WRITE_VERSION", body)
        # version 2 goes through the unchanged writer
        body = function_body(self.image, "game_state_image_write_version")
        self.assertRegex(body, r"if \(version == GAME_STATE_IMAGE_VERSION\)\n\t\treturn game_state_image_write\(")
        self.assertIn("image_write_or_measure(layout, header, file, digest, GAME_STATE_IMAGE_VERSION)",
                      function_body(self.image, "game_state_image_write"))
        self.assertIn("image_write_or_measure(layout, header, NULL, bytes, GAME_STATE_IMAGE_VERSION)",
                      function_body(self.image, "game_state_image_digest"))

    def test_typed_write_is_gated_before_anything_is_written(self):
        body = function_body(self.image, "image_write_or_measure")
        gate = body.index("game_state_schema_coverage(&typed_state, &coverage)")
        refused = body.index("return FALSE;", gate)
        self.assertIn("if (!coverage.complete)", body[gate:refused])
        self.assertIn("_game_state_image_refusal_schema", body[gate:refused])
        self.assertLess(refused, body.index("file->write("))
        self.assertLess(refused, body.index("image_allocate("))

    def test_version_2_reading_is_retained(self):
        body = function_body(self.image, "reader_pass")
        typed = body.index("if (reader->typed)")
        version_2 = body.index("else", typed)
        order = body.index("_game_state_image_refusal_byte_order", version_2)
        # the byte-order refusal of version 2, as it was
        self.assertIn("image_port_big_endian() ? IMAGE_BIG_ENDIAN : IMAGE_LITTLE_ENDIAN", body[version_2:order])
        self.assertIn("IMAGE_ENCODING_WORDS", body[order:order + 300])
        # version 2's walk of the allocations, then the rest of the CPU and GPU parts
        self.assertIn("reader_data_array(reader, allocation", body)
        self.assertIn("reader_memory_pool(reader, allocation)", body)
        self.assertIn("!reader->typed &&", body)
        self.assertIn("reader_take(reader, layout->size - layout->gpu_used, layout->gpu_used)", body)

    def test_typed_reading_is_gated_before_the_payload(self):
        body = function_body(self.image, "reader_pass")
        gate = body.index("game_state_schema_coverage(&typed_state, &coverage)")
        self.assertIn("_game_state_image_refusal_schema", body[gate:gate + 800])
        self.assertIn("HEADER_SCHEMA_IDENTITY", body[gate:gate + 1200])
        self.assertLess(gate, body.index("reader_typed(reader, &typed_state)"))
        self.assertLess(gate, body.index("reader->chunk_checksums = image_allocate"))
        # the load still checks the whole image before it takes it, and a
        # failure while taking is reported, never retried here
        read = self.image[self.image.index("static short image_read("):self.image.index("/* ---------- public code */")]
        first = read.index("reader_pass(&reader")
        self.assertLess(first, read.index("before_taking();"))
        self.assertLess(read.index("before_taking();"), read.index("reader_pass(&reader", first + 1))
        self.assertIn("result = _game_state_image_failed_while_taking;", read)

    def test_header_accepts_only_versions_2_and_3(self):
        body = function_body(self.image, "image_header_check")
        self.assertIn("GAME_STATE_IMAGE_VERSION", body)
        self.assertIn("GAME_STATE_IMAGE_TYPED_VERSION", body)
        self.assertRegex(self.image, r"\tHEADER_SCHEMA_IDENTITY = 412,")
        self.assertRegex(self.image, r"\tHEADER_TYPED_BYTES = 416,")

    def test_refusal_names_follow_the_enum(self):
        names = re.search(r"names\[NUMBER_OF_GAME_STATE_IMAGE_REFUSALS\] =\s*\{(.*?)\};", self.image, re.S).group(1)
        enum = re.search(r"enum game_state_image_refusal\s*\{(.*?)\};", self.header, re.S).group(1)
        values = re.findall(r"_game_state_image_refusal_(\w+),", enum)
        self.assertEqual(re.findall(r'"(\w+)"', names), values)
        self.assertEqual(values[-1], "schema")


class SchemaSourceTests(unittest.TestCase):
    def test_registrations_of_the_first_classes(self):
        classes = CLASSES.read_text(encoding="utf-8")
        for name in ('"header"', '"object name list"', '"lightnings"', '"light volumes"'):
            self.assertIn("{ " + name + ",", classes)
        game_time = (ROOT / "source/game/game_time.c").read_text(encoding="utf-8")
        self.assertIn("game_state_port_schema(game_time_globals, &game_time_globals_schema);", game_time)
        hs = (ROOT / "source/hs/hs_runtime.c").read_text(encoding="utf-8")
        self.assertIn("game_state_port_schema(hs_thread_data, &hs_thread_schema);", hs)
        self.assertIn("game_state_port_schema(hs_global_data, &hs_global_schema);", hs)
        # HS cells stay unresolved: never typed as an integer of a width
        self.assertRegex(hs, r'"value", _game_state_schema_hs_cell')
        self.assertRegex(hs, r'"result", _game_state_schema_hs_cell')
        self.assertRegex(hs, r'"stack_data", _game_state_schema_unresolved')

    def test_every_allocation_is_a_region(self):
        text = GAME_STATE.read_text(encoding="utf-8")
        for name in ("game_state_malloc", "game_state_gpu_malloc"):
            self.assertIn("game_state_region_record(name, pointer, size, ", function_body(text, name))
        for name, kind in (("game_state_data_new", "data_array"), ("game_state_memory_pool_new", "memory_pool"),
                           ("game_state_lruv_cache_new", "lruv_cache")):
            self.assertIn("_game_state_schema_region_" + kind, function_body(text, name))

    def test_no_untyped_bytes_are_written(self):
        """the codec has no opaque or host-memory field type: unresolved bytes are refused"""
        text = SCHEMA.read_text(encoding="utf-8")
        encode = function_body(text, "schema_encode_instance")
        self.assertRegex(encode, r"case _game_state_schema_hs_cell:\n\t\tcase _game_state_schema_unresolved:\n"
                                 r"\t\t\treturn _game_state_schema_stream_unresolved;")
        self.assertNotIn("memcpy", text)


if __name__ == "__main__":
    unittest.main()
