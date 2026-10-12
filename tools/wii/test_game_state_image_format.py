"""Tests of the canonical game-state image's reader and the engine source it mirrors (HWI-076)."""
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/wii"))

import game_state_image_format as image  # noqa: E402

SOURCE = ROOT / "source/saved games/game_state_image.c"
HEADER_SOURCE = ROOT / "source/saved games/game_state_image.h"
XBOX = ROOT / "source/saved games/game_state_xbox.c"
GAME_STATE = ROOT / "source/saved games/game_state.c"


def words_with_zeros():
    words = [0] * 5000
    words[3] = 7
    words[10:14] = [1, 0, 0, 2]          # a run of two zeros stays literal
    words[100] = image.POINTER_TAG | 0x1234
    words[4000:4000 + 1500] = range(1, 1501)  # literals past the segment maximum
    return words


class FormatTests(unittest.TestCase):
    def test_engine_crc_is_zlib_crc_inverted(self):
        self.assertEqual(image.engine_crc(b"123456789"), 0xCBF43926 ^ 0xFFFFFFFF)

    def test_round_trip(self):
        words = words_with_zeros()
        data = image.build_image(words, map_name="levels\\test\\x", build_number="01.01.14.2342")
        header, back = image.read_image(data)
        self.assertEqual(back, words)
        self.assertEqual(header["map_name"], "levels\\test\\x")
        self.assertEqual(header["chunk_count"], 1)

    def test_segments_follow_the_engine_rule(self):
        payload = image.encode_words([5, 0, 0, 6, 0, 0, 0, 7])
        controls = []
        at = 0
        while at < len(payload):
            control = struct.unpack_from("<I", payload, at)[0]
            controls.append(control)
            at += 4 + (0 if control & image.ZERO_RUN_BIT else 4 * control)
        # [5 0 0 6] literal, a run of 3 zeros, [7] literal
        self.assertEqual(controls, [4, image.ZERO_RUN_BIT | 3, 1])
        long = image.encode_words(list(range(1, 2049)))
        self.assertEqual(struct.unpack_from("<I", long, 0)[0], image.LITERAL_MAXIMUM_WORDS)

    def test_many_chunks(self):
        words = list(range(1, 40000))
        header, back = image.read_image(image.build_image(words))
        self.assertEqual(back, words)
        self.assertEqual(header["chunk_count"], 3)

    def refusal(self, data):
        with self.assertRaises(image.ImageError) as caught:
            image.read_image(data)
        return caught.exception.refusal

    def test_damage_is_refused(self):
        good = bytearray(image.build_image(list(range(1, 40000))))
        flipped = bytearray(good)
        flipped[image.HEADER_BYTES + image.CHUNK_HEADER_BYTES + 1000] ^= 0x10
        self.assertEqual(self.refusal(bytes(flipped)), "chunk_checksum")
        # the chunk's CRC made again: the payload's SHA-256 still refuses it
        size = struct.unpack_from("<I", flipped, image.HEADER_BYTES + 8)[0]
        start = image.HEADER_BYTES + image.CHUNK_HEADER_BYTES
        struct.pack_into("<I", flipped, image.HEADER_BYTES + 12, image.engine_crc(bytes(flipped[start:start + size])))
        self.assertEqual(self.refusal(bytes(flipped)), "digest")
        header = bytearray(good)
        header[80] ^= 1
        self.assertEqual(self.refusal(bytes(header)), "header_checksum")
        self.assertEqual(self.refusal(bytes(good[:len(good) // 2])), "read")
        self.assertEqual(self.refusal(bytes(good[:-1])), "read")
        self.assertEqual(self.refusal(b""), "read")
        self.assertEqual(self.refusal(b"XXXX" + bytes(good[4:])), "magic")

    def test_header_edits_are_refused(self):
        def edited(**fields):
            data = bytearray(image.build_image([1, 2, 3, 0, 0, 0, 4]))
            for name, at, fmt in image.FIELDS:
                if name in fields:
                    struct.pack_into(fmt, data, at, fields[name])
            struct.pack_into("<I", data, 508, image.engine_crc(bytes(data[:508])))
            return bytes(data)
        self.assertEqual(self.refusal(edited(version=3)), "version")
        self.assertEqual(self.refusal(edited(flags=0)), "incomplete")
        self.assertEqual(self.refusal(edited(chunk_count=2)), "length")
        self.assertEqual(self.refusal(edited(state_bytes=32)), "layout")

    def test_word_offsets_follow_the_cpu_then_gpu_parts(self):
        header = {"cpu_used": 8, "gpu_used": 8, "state_size": 64}
        self.assertEqual([image.word_offset(header, index) for index in range(4)], [0, 4, 56, 60])


class SourceTests(unittest.TestCase):
    def test_reader_offsets_are_the_engine_header(self):
        text = SOURCE.read_text(encoding="utf-8")
        engine = {name.lower(): int(value) for name, value in re.findall(r"\tHEADER_([A-Z_]+) = (\d+),", text)}
        mine = {name: at for name, at, _ in image.FIELDS}
        for name, at in mine.items():
            key = {"header_checksum": "checksum", "header_bytes": "bytes"}.get(name, name)
            self.assertEqual(engine.get(key), at, name)
        self.assertEqual(engine["map_name"], 72)
        self.assertEqual(engine["build_number"], 328)
        self.assertEqual(engine["digest"], 360)

    def test_engine_constants(self):
        text = SOURCE.read_text(encoding="utf-8")
        header = HEADER_SOURCE.read_text(encoding="utf-8")
        self.assertIn("LITERAL_MAXIMUM_WORDS = 1024", text)
        self.assertIn("ZERO_RUN_MINIMUM_WORDS = 3", text)
        self.assertIn("POINTER_TAG = 0x40000000", text)
        self.assertIn("GAME_STATE_IMAGE_CHUNK_BYTES = 0x10000", header)
        self.assertIn("GAME_STATE_IMAGE_VERSION = 2", header)
        self.assertIn("GAME_STATE_IMAGE_HEADER_BYTES = 512", header)

    def test_no_second_game_state_buffer(self):
        """the saved-game path no longer reads a whole image beside the game state"""
        for path in (XBOX, GAME_STATE):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("game_state_image_accept", text)
            self.assertIsNone(re.search(r"\bmalloc\s*\(", text), path.name)

    def test_every_load_checks_before_it_takes(self):
        text = SOURCE.read_text(encoding="utf-8")
        body = text[text.index("static short image_read("):text.index("/* ---------- public code */")]
        first_pass = body.index("reader_pass(&reader")
        before = body.index("before_taking();")
        second_pass = body.index("reader_pass(&reader", first_pass + 1)
        self.assertLess(first_pass, before)
        self.assertLess(before, second_pass)


if __name__ == "__main__":
    unittest.main()
