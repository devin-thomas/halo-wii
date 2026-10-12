"""Engine reads of a multi-byte field through a byte pointer at a fixed offset (HWI-030).

The decompiled engine read byte 0xb6 of a unit to test a damage flag. On the
little-endian Xbox that byte is the low byte of the object's damage_flags; on a
big-endian port it is the high byte, so the Wii opened automatic doors the Xbox
kept shut (a10, a forced switch into BSP 5: device group 67 at tick 484). Such
reads must go through the field as a value, so every port tests the same bit.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "source"
# *((byte *)name + 0x..) or *(byte *)((byte *)name + 0x..): a raw byte at a fixed offset of a structure
RAW_BYTE_READ = re.compile(r"\*\s*\(\s*\(\s*(?:byte|unsigned char)\s*\*\s*\)\s*\w+\s*\+\s*0x[0-9a-fA-F]+\s*\)")


def code(path):
    """the file without its comments"""
    text = path.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class EngineByteReads(unittest.TestCase):
    def test_no_raw_byte_read_of_a_structure_field(self):
        found = []
        for path in sorted(SOURCE.rglob("*.c")):
            if path.parts[len(SOURCE.parts)] in ("bitmaps", "tool"):
                continue
            for match in RAW_BYTE_READ.finditer(code(path)):
                found.append(f"{path.relative_to(ROOT).as_posix()}: {match.group(0)}")
        self.assertEqual(found, [])

    def test_door_reads_the_damage_flags_low_byte_as_a_value(self):
        text = code(SOURCE / "devices" / "device_machines.c")
        self.assertIn("TEST_FLAG((byte)unit->object.damage_flags, _object_function2_active_bit)", text)


if __name__ == "__main__":
    unittest.main()
