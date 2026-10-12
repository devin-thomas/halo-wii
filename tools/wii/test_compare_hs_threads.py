"""Tests of compare_hs_threads.py on authored thread arrays (HWI-033)."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_hs_threads as cht  # noqa: E402

DATA = 0x91000038  # the array's elements, right after its 56-byte header at 0x91000000


def dump(order, ticks_cell):
    """one thread: a root frame (4 bytes of values) and a frame above it holding a long expression index and
    a short cell; order is '>' (the Wii) or '<' (the host); ticks_cell packs the short's 4-byte cell"""
    header = bytearray(cht.DATA_ARRAY_BYTES)
    header[0:9] = b"hs thread"
    struct.pack_into(order + "hh", header, 32, 256, cht.THREAD_BYTES)
    struct.pack_into(order + "Ihhhh", header, 40, 0x64407440, 1, 1, 1, -3222)
    struct.pack_into(order + "I", header, 52, DATA)
    thread = bytearray(cht.THREAD_BYTES)
    stack = DATA + cht.THREAD_HEADER_BYTES
    root, top = stack, stack + 4 + 16
    struct.pack_into(order + "hBBiiiIi", thread, 0, -3223, 0, 0, 0, 200, 0, top, 0)
    struct.pack_into(order + "IiIh", thread, 24, 0, -1, root + 14, 4)
    struct.pack_into(order + "IiIh", thread, 24 + 20, root, 0x0E37E000, root + 14, 8)
    struct.pack_into(order + "i", thread, 24 + 20 + 14, -0x1C81FFF5)
    thread[24 + 20 + 18:24 + 20 + 22] = ticks_cell
    return bytes(header + thread)


class CompareThreadTests(unittest.TestCase):
    def test_same_values_in_the_same_places(self):
        # a short cell as HWI-016C stores it: the value in its first two bytes, each port in its own order
        wii = dump(">", struct.pack(">hh", 200, -1))
        host = dump("<", struct.pack("<hh", 200, -1))
        (thread,) = cht.compare_dump(wii, host)
        self.assertFalse(thread["identical"])
        self.assertEqual(thread["live_frames"], 2)
        self.assertTrue(thread["live_same_values_same_places"])

    def test_a_different_value_is_found(self):
        wii = dump(">", struct.pack(">hh", 200, -1))
        host = dump("<", struct.pack("<hh", 201, -1))
        (thread,) = cht.compare_dump(wii, host)
        self.assertFalse(thread["live_same_values_same_places"])
        self.assertTrue(any("frame values" in m for m in thread["mismatched"]))

    def test_a_long_and_a_short_pair_are_alike_in_bytes(self):
        # (the documented limit: typing is not in the bytes)
        wii = dump(">", struct.pack(">i", (-1 << 16) | 200))
        host = dump("<", struct.pack("<hh", 200, -1))
        (thread,) = cht.compare_dump(wii, host)
        self.assertTrue(thread["live_same_values_same_places"])

    def test_cells(self):
        self.assertTrue(cht.cells_match(b"\x12\x34\x56\x78\x9a", b"\x78\x56\x34\x12\x9a"))
        self.assertTrue(cht.cells_match(b"\x12\x34\x56\x78", b"\x34\x12\x78\x56"))
        self.assertFalse(cht.cells_match(bytes([0, 0, 0, 0xC8]), bytes([0, 0, 0, 0xC9])))


if __name__ == "__main__":
    unittest.main()
