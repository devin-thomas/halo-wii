"""Authored-stream tests for explicit recorded-animation decoding (ADR-018)."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_fixtures as fx  # noqa: E402
import recorded_animation as ra  # noqa: E402


class CurrentCodecTests(unittest.TestCase):
    def test_every_event_type_and_delta_kind_decodes_explicitly(self):
        stream = fx.recorded_stream_v4()
        decoded = ra.decode(stream, 4, 4, fx.V4_TICKS)
        self.assertEqual(decoded["unit_control"][:5], [3, 200, -2, 7, 0x1234])
        self.assertEqual(decoded["unit_control"][5], struct.unpack("<I", struct.pack("<f", 0.5))[0])
        self.assertEqual(decoded["unit_control"][-3:], [-70000, 300, -1])
        self.assertEqual(decoded["state"], [10, -20, 30, -40, 999, -999])
        events = decoded["events"]
        self.assertEqual([e[0] for e in events], [2, 3, 4, 5, 6] + list(range(7, 23)) + [0, 1])
        self.assertEqual([(e[1], e[2]) for e in events[:5]], [(0, 0), (1, 1), (2, 2), (3, 300), (2, 255)])
        self.assertEqual(events[2][3], (-3,))
        self.assertEqual(events[5][3], (-7, 7))            # signed chars, whatever the host's char
        self.assertEqual(events[13][3], (-15000, 1015))
        self.assertEqual(decoded["total_ticks"] + 1, fx.V4_TICKS)
        self.assertEqual(decoded["tail"], b"")

    def test_header_byte_uses_explicit_bits_not_bitfield_order(self):
        # 0x0D: kind 1 (bits 0-1), type 3 (bits 2-7); PPC EABI bit-fields read kind 0, type 13.
        stream = bytes(52) + bytes(12) + bytes([0x0D, 9, 0x04 | 1])
        decoded = ra.decode(stream, 4, 1)
        self.assertEqual(decoded["events"][0], (3, 1, 1, (9,)))
        self.assertEqual(decoded["events"][1], (1, 1, 1, ()))

    def test_round_trip_and_canonical_big_endian_record(self):
        stream = fx.recorded_stream_v4()
        canonical, facts = ra.convert(stream, 4, 4, fx.V4_TICKS)
        self.assertEqual((facts["round_trip"], facts["events"], facts["tail_bytes"]), ("exact", 23, 0))
        magic, version, anim, ucv, ticks, fields, events, source, total, tail, _ = ra.HRA_HEADER.unpack_from(canonical)
        self.assertEqual((magic, version, anim, ucv, ticks, fields, events, source, total, tail),
                         (b"HRA1", 1, 4, 4, fx.V4_TICKS, 19, 23, len(stream), fx.V4_TICKS - 1, 0))
        unit = ra.HRA_HEADER.size
        self.assertEqual(canonical[unit + 8:unit + 12], b"\xff\xff\xff\xfe")     # s16 -2 sign-extended
        events_at = unit + 4 * (19 + 6)
        self.assertEqual(canonical[events_at:events_at + 8], bytes([2, 0, 0, 0, 0, 0, 0, 5]))
        char_event = events_at + 16 * 5
        self.assertEqual(canonical[char_event:char_event + 12],
                         bytes([7, 1, 0, 1]) + b"\xff\xff\xff\xf9" + b"\x00\x00\x00\x07")
        self.assertEqual(ra.encode_xbox(ra.unpack_canonical(canonical)), stream)
        self.assertEqual(canonical, ra.convert(bytes(stream), 4, 4, fx.V4_TICKS)[0])

    def test_damaged_streams_reject(self):
        stream = fx.recorded_stream_v4()
        for cut in range(1, len(stream)):
            with self.subTest(cut=cut), self.assertRaises(ra.AnimationError):
                ra.decode(stream[:cut], 4, 4)
        unit_and_state = 52 + 4 + 2 + 2 + 12
        bad = [
            stream[:unit_and_state] + bytes([23 << 2]),          # unknown event type
            stream[:unit_and_state] + bytes([2 << 2 | 2, 1, 0]),  # byte delta not above 1
            stream[:unit_and_state] + bytes([2 << 2 | 3, 255, 0, 0]),  # word delta not above 255
            b"",
        ]
        for case in bad:
            with self.subTest(case=case[-4:]), self.assertRaises(ra.AnimationError):
                ra.decode(case, 4, 4)
        for version, ucv in ((0, 4), (5, 4), (4, 5)):
            with self.subTest(version=version, ucv=ucv), self.assertRaises(ra.AnimationError):
                ra.decode(stream, version, ucv)
        with self.assertRaises(ra.AnimationError):
            ra.decode(bytes(ra.MAX_STREAM_BYTES + 1), 4, 4)

    def test_unit_control_versions_follow_the_engine_tables(self):
        self.assertEqual(len(ra.unit_control_fields(0)), 16)
        self.assertEqual(len(ra.unit_control_fields(1)), 16)
        self.assertEqual(len(ra.unit_control_fields(2)), 17)
        self.assertEqual(sum(size for _, size in ra.unit_control_fields(4)), 60)
        self.assertIsNone(ra.unit_control_fields(5))

    def test_canonical_record_rejects_inconsistent_lengths(self):
        canonical, _ = ra.convert(fx.recorded_stream_v4(), 4, 4, fx.V4_TICKS)
        for broken in (canonical[:20], b"HRA2" + canonical[4:], canonical + b"\0", canonical[:-1]):
            with self.subTest(size=len(broken)), self.assertRaises(ra.AnimationError):
                ra.unpack_canonical(broken)


class VersionOneCodecTests(unittest.TestCase):
    def test_v1_stream_round_trips(self):
        stream = fx.recorded_stream_v1()
        for version in (1, 2, 3):
            canonical, facts = ra.convert(stream, version, 1, 22)
            self.assertEqual((facts["codec"], facts["events"], facts["total_ticks"]), (1, 7, 21))
            decoded = ra.unpack_canonical(canonical)
            self.assertEqual(decoded["events"][1], (4, ra.KIND_NONE, 2, (0xBEEF,)))
            self.assertEqual(ra.encode_xbox(decoded), stream)

    def test_v1_truncation_and_unknown_type_reject(self):
        stream = fx.recorded_stream_v1()
        for cut in range(1, len(stream)):
            with self.subTest(cut=cut), self.assertRaises(ra.AnimationError):
                ra.decode(stream[:cut], 1, 1)
        unknown = stream[:52] + struct.pack("<hH", 40, 0)
        with self.assertRaises(ra.AnimationError):
            ra.decode(unknown, 1, 1)


if __name__ == "__main__":
    unittest.main()
