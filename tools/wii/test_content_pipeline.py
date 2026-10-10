"""End-to-end tests of the content pipeline on authored synthetic maps.

Covers: bounded cache parsing and its rejections, atomic publication
(interruption, simulated disk-full, insufficient space, tampering),
path rules, deterministic conversion, source immutability, the inventory's
public/private split, and the audio/movie format helpers.
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_convert as cc  # noqa: E402
import content_fixtures as fx  # noqa: E402
import content_inventory as ci  # noqa: E402
import content_publish as cp  # noqa: E402
import halo_cache as hc  # noqa: E402
import media_formats as mf  # noqa: E402


def open_bytes(blob):
    header, body, facts = hc.inflate_stream(io.BytesIO(blob))
    return hc.CacheMap(header, body, facts)


def tree_hashes(directory):
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(Path(directory).rglob("*")) if p.is_file()}


class CacheParsingTests(unittest.TestCase):
    def test_fixture_map_parses(self):
        cache = open_bytes(fx.build_map())
        self.assertEqual(cache.group_counts(), {"bitm": 1, "scnr": 1, "snd!": 1})
        self.assertEqual((cache.header["category"], cache.header["build"]), ("campaign", "01.10.12.2276"))
        self.assertEqual(len(cache.body), cache.header["declared_bytes"])

    def test_malformed_headers_and_streams_reject(self):
        good = fx.build_map()
        cases = {
            "head": b"XXXX" + good[4:],
            "foot": good[:0x7FC] + b"XXXX" + good[0x800:],
            "version": fx.build_map(mutate=lambda s: s.update(version=6)),
            "declared above limit": fx.build_map(mutate=lambda s: s.update(declared=hc.MAP_LIMIT + 1)),
            "declared longer than stream": fx.build_map(mutate=lambda s: s.update(declared=len(s["body"]) + 9999)),
            "declared shorter than stream": fx.build_map(mutate=lambda s: s.update(declared=len(s["body"]) + 16)),
            "tag offset outside": fx.build_map(mutate=lambda s: s.update(tag_offset_field=0x7FFFFFF0)),
            "tag size zero": fx.build_map(mutate=lambda s: s.update(tag_bytes=0)),
            "truncated header": good[:0x700],
            "truncated stream": good[:-40],
            "corrupt stream": good[:0x810] + bytes(64) + good[0x850:],
        }
        for name, blob in cases.items():
            with self.subTest(case=name), self.assertRaises(hc.CacheError):
                open_bytes(blob)

    def test_malformed_tag_index_rejects(self):
        def poke(offset, fmt, *values):
            return lambda s: struct.pack_into(fmt, s["tags"].data, offset, *values)
        cases = {
            "signature": poke(32, "<I", 0x12345678),
            "instance count zero": poke(12, "<I", 0),
            "instance count huge": poke(12, "<I", 70000),
            "instances below base": poke(0, "<I", 0x1000),
            "datum ordinal": poke(36 + 12, "<I", 0xE1740005),
            "scenario datum": poke(4, "<I", 0xE1740001),
        }
        for name, mutate in cases.items():
            with self.subTest(case=name), self.assertRaises(hc.CacheError):
                open_bytes(fx.build_map(mutate=mutate))

    def test_checked_accessors_reject_bad_counts_and_spans(self):
        cache = open_bytes(fx.build_map())
        with self.assertRaises(hc.CacheError):
            cache.offset(hc.TAG_BASE - 4, 4)
        with self.assertRaises(hc.CacheError):
            cache.offset(hc.TAG_BASE + len(cache.tags) - 2, 4)
        with self.assertRaises(hc.CacheError):
            cache.file_span(len(cache.body) - 4, 8)
        with self.assertRaises(hc.CacheError):
            cache.file_span(-1, 1)


class PathRuleTests(unittest.TestCase):
    def test_relative_path_rules(self):
        for good in ("textures/m01/00001-000.hwt", "sounds/x/1.hws"):
            self.assertEqual(cp.check_relative(good), good)
        for bad in ("", "../x", "a/../b", "/abs/x", "C:/x", "c:x", "a\\b", "A/b", "con.hwt", "lpt1", "a//b",
                    "./a", "manifest.json", "CURRENT", ".staging-1/x", "a/" + "b" * 65, "/".join("a" * 7),
                    "x\0y", "a/b/"):
            with self.subTest(path=bad), self.assertRaises(cp.PublishError):
                cp.check_relative(bad)

    def test_manifest_rules(self):
        entry = {"bytes": 1, "sha256": "0" * 64}
        with self.assertRaises(cp.PublishError):
            cp.validate_manifest({"outputs": [dict(entry, path="a/x"), dict(entry, path="a/x")]})
        with self.assertRaises(cp.PublishError):
            cp.validate_manifest({"outputs": [dict(entry, path="b"), dict(entry, path="a")]})
        with self.assertRaises(cp.PublishError):
            cp.validate_manifest({"outputs": [dict(entry, path="a", bytes=1 << 32)]})
        with self.assertRaises(cp.PublishError):
            cp.validate_manifest({"outputs": [dict(entry, path="a", sha256="g" * 64)]})


class ConverterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.staging = fx.write_staging(root / "staging", {"alpha": fx.build_map("alpha"),
                                                           "beta": fx.build_map("beta", category=1)})
        self.out = root / "out"
        self.source_hashes = tree_hashes(self.staging)

    def tearDown(self):
        self.assertEqual(tree_hashes(self.staging), self.source_hashes)  # sources never modified
        self.temp.cleanup()

    def convert(self, **kwargs):
        args = dict(staging_dir=self.staging, map_names=None, categories=list(cc.CATEGORIES),
                    profile_name="gx-baseline-v1", output_root=self.out)
        args.update(kwargs)
        return cc.run(**args)

    def test_publish_and_repeat_are_identical(self):
        first = self.convert()
        published = cp.read_current(self.out, cc.verify_output)
        self.assertEqual(published["generation"], first["generation"])
        self.assertEqual(first["totals"], {"texture": {"files": 8, "bytes": first["totals"]["texture"]["bytes"]},
                                           "animation": {"files": 2, "bytes": first["totals"]["animation"]["bytes"]},
                                           "sound": {"files": 4, "bytes": first["totals"]["sound"]["bytes"]}})
        before = tree_hashes(self.out / "generations")
        second = self.convert()
        self.assertEqual((second["generation"], second["reused_existing"]), (first["generation"], True))
        self.assertEqual(tree_hashes(self.out / "generations"), before)
        other = self.convert(output_root=Path(self.temp.name) / "fresh")
        self.assertEqual(other["manifest_sha256"], first["manifest_sha256"])
        self.assertEqual(tree_hashes(Path(self.temp.name) / "fresh" / "generations"), before)
        manifest = published["manifest"]
        self.assertNotIn(self.temp.name, json.dumps(manifest))
        self.assertIn("modules_sha256", manifest["converter"])
        for kind in ("texture:dxt1:2d", "texture:r5g6b5:cube", "sound:xbox_adpcm:1:22050"):
            self.assertIn(kind, first["verification"])

    def test_profile_or_input_change_gives_a_new_generation(self):
        first = self.convert()
        subset = self.convert(map_names=["alpha"])
        self.assertNotEqual(first["generation"], subset["generation"])
        self.assertEqual(cp.read_current(self.out)["generation"], subset["generation"])
        self.assertTrue((self.out / "generations" / first["generation"]).is_dir())

    def assert_previous_survives(self, expected_generation, expected_tree):
        current = cp.read_current(self.out, cc.verify_output)
        self.assertEqual(current["generation"], expected_generation)
        self.assertEqual(tree_hashes(self.out / "generations" / expected_generation), expected_tree)
        self.assertFalse([p for p in self.out.iterdir() if p.name.startswith(".staging-")])

    def test_disk_full_and_interruptions_preserve_last_valid_generation(self):
        first = self.convert(map_names=["alpha"])
        tree = tree_hashes(self.out / "generations" / first["generation"])
        counter = cp.FaultInjector()
        self.convert(output_root=Path(self.temp.name) / "measure", fault=counter)
        for after in (0, 1, 1000, counter.written // 2, counter.written - 1):
            with self.subTest(after=after), self.assertRaises(OSError):
                self.convert(fault=cp.FaultInjector(after_bytes=after))
            self.assert_previous_survives(first["generation"], tree)
        for stage in ("before-manifest", "before-validate", "before-promote", "before-current"):
            with self.subTest(stage=stage), self.assertRaises(InterruptedError):
                self.convert(fault=cp.FaultInjector(stage=stage))
            self.assert_previous_survives(first["generation"], tree)
        # a run killed mid-write leaves a staging directory; the next run discards it
        (self.out / ".staging-deadbeefdeadbeef" / "textures").mkdir(parents=True)
        result = self.convert()
        self.assertEqual(result["discarded_stale_staging"], 1)

    def test_insufficient_space_refuses_before_writing(self):
        class Usage:
            free = 1024
        with self.assertRaises(cp.PublishError):
            self.convert(disk_usage=lambda path: Usage())
        self.assertIsNone(cp.read_current(self.out))
        self.assertFalse([p for p in self.out.iterdir() if p.name.startswith(".staging-")])

    def test_tampered_generation_is_detected(self):
        result = self.convert(map_names=["alpha"])
        target = next((self.out / "generations" / result["generation"] / "textures").rglob("*.hwt"))
        data = bytearray(target.read_bytes())
        data[-1] ^= 1
        target.write_bytes(bytes(data))
        with self.assertRaises(cp.PublishError):
            cp.read_current(self.out)
        target.write_bytes(bytes(data) + b"x")
        with self.assertRaises(cp.PublishError):
            cp.read_current(self.out)
        (self.out / "CURRENT").write_text("../../etc " + "0" * 64, encoding="ascii")
        with self.assertRaises(cp.PublishError):
            cp.read_current(self.out)

    def test_invalid_items_reject_the_whole_run(self):
        first = self.convert(map_names=["alpha"])

        def bad_mips(s):
            struct.pack_into("<h", s["tags"].data, s["table"] + 0x14, 9)

        def pixels_past_end(s):
            struct.pack_into("<i", s["tags"].data, s["table"] + 0x18, 0x7FFFFF00)

        def bad_block_count(s):
            struct.pack_into("<i", s["tags"].data, s["group"] + 0x60, -1)

        def stream_outside(s):
            struct.pack_into("<I", s["tags"].data, s["anim_table"] + 44 + 12, hc.TAG_BASE + 0x7FFFFF)

        def adpcm_partial(s):
            struct.pack_into("<i", s["tags"].data, s["perms"] + 0x40, 35)

        def wrong_compression(s):
            struct.pack_into("<h", s["tags"].data, s["perms"] + 0x28, 3)

        for mutate in (bad_mips, pixels_past_end, bad_block_count, stream_outside, adpcm_partial, wrong_compression):
            with self.subTest(case=mutate.__name__):
                staging = fx.write_staging(Path(self.temp.name) / mutate.__name__,
                                           {"alpha": fx.build_map("alpha", mutate=mutate)})
                with self.assertRaises(cc.ConversionError):
                    self.convert(staging_dir=staging)
                self.assertEqual(cp.read_current(self.out)["generation"], first["generation"])

    def test_staging_and_output_location_rules(self):
        record = json.loads((self.staging / "staging.json").read_text())
        for name in ("../evil.map", "maps/x.map", "UPPER.map", "x.bin"):
            record["files"][0]["path"] = name
            (self.staging / "staging.json").write_text(json.dumps(record))
            with self.subTest(name=name), self.assertRaises(cc.ConversionError):
                self.convert()
        record["files"][0]["path"] = "alpha.map"
        record["files"][0]["sha256"] = "1" * 64
        (self.staging / "staging.json").write_text(json.dumps(record))
        with self.assertRaises(cc.ConversionError):
            self.convert()          # recorded hash differs from the file
        (self.staging / "staging.json").write_text(json.dumps(json.loads(json.dumps(record))))
        self.source_hashes = tree_hashes(self.staging)
        with self.assertRaises(cc.ConversionError):
            self.convert(output_root=self.staging / "maps")
        with self.assertRaises(cc.ConversionError):
            self.convert(output_root=Path(__file__).resolve().parent / "never")
        with self.assertRaises(cc.ConversionError):
            self.convert(map_names=["gamma"])
        with self.assertRaises(cc.ConversionError):
            self.convert(categories=["movies"])

    def test_cli_exit_codes_and_run_record(self):
        quiet = contextlib.ExitStack()
        quiet.enter_context(contextlib.redirect_stdout(io.StringIO()))
        quiet.enter_context(contextlib.redirect_stderr(io.StringIO()))
        self.addCleanup(quiet.close)
        record = Path(self.temp.name) / "run.json"
        status = cc.main(["--staging", str(self.staging), "--output-root", str(self.out), "--record", str(record),
                          "--maps", "alpha"])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(record.read_text())["status"], 0)
        status = cc.main(["--staging", str(self.staging), "--output-root", str(self.out), "--record",
                          str(Path(self.temp.name) / "full.json"), "--fault-after-bytes", "100"])
        self.assertEqual(status, 4)
        self.assertEqual(json.loads((Path(self.temp.name) / "full.json").read_text())["status"], 4)
        status = cc.main(["--staging", str(self.staging), "--output-root", str(self.out),
                          "--fault-stage", "before-promote"])
        self.assertEqual(status, 4)
        self.assertEqual(cc.main(["--staging", str(self.staging), "--output-root", str(self.out), "--record",
                                  str(record)]), 1)  # existing run record is never overwritten


class InventoryTests(unittest.TestCase):
    def test_public_summary_has_counts_but_no_names_or_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            staging = fx.write_staging(Path(temp) / "s", {"alpha": fx.build_map("alpha"),
                                                          "beta": fx.build_map("beta", category=1)})
            before = tree_hashes(staging)
            private, public = ci.run(staging)
            self.assertEqual(tree_hashes(staging), before)
            self.assertEqual(public["maps_by_category"], {"campaign": 1, "multiplayer": 1})
            self.assertEqual(public["totals"]["bitmaps"], 8)
            self.assertEqual(public["bitmaps_by_format"], {"a8r8g8b8": 2, "ay8": 2, "dxt1": 2, "r5g6b5": 2})
            self.assertEqual(public["maps"][0]["recorded_animations"]["round_trip_exact"], 1)
            text = json.dumps(public)
            for secret in ("fixture\\\\bitmaps", "alpha", "beta", temp.replace("\\", "\\\\")):
                self.assertNotIn(secret, text)
            self.assertIn("fixture\\bitmaps", private["maps"][0]["bitmap_rows"][0][0])
            self.assertEqual(ci.run(staging)[1], public)  # deterministic


class MediaFormatTests(unittest.TestCase):
    def test_xbox_adpcm_block_decoding(self):
        block = struct.pack("<hBB", 1000, 0, 0) + bytes([0x17] + [0] * 31)
        samples = mf.decode_xbox_adpcm(block, 1)
        self.assertEqual(len(samples), 64)
        # header sample first; nibble 7 at step 7 adds 11 (index -> 8), nibble 1 at step 16 adds 6
        self.assertEqual(samples[:3], [1000, 1011, 1017])
        stereo = mf.decode_xbox_adpcm(fx.adpcm_payload(2, 2, 3), 2)
        self.assertEqual(len(stereo), 2 * 2 * 64)
        with self.assertRaises(mf.MediaError):
            mf.adpcm_frames(37, 1)
        self.assertEqual(mf.pcm16_be([1, -2]), b"\x00\x01\xff\xfe")
        self.assertEqual(mf.pcm16le_to_be(b"\x01\x00\xfe\xff", 1), b"\x00\x01\xff\xfe")

    def test_bink_header(self):
        header = b"BIKi" + struct.pack("<10I", 1000 - 8, 300, 4096, 0, 640, 480, 2997, 100, 0, 1)
        header += struct.pack("<I", 0) + struct.pack("<HH", 44100, 0x6000) + struct.pack("<I", 0)
        facts = mf.parse_bink(header, 1000)
        self.assertEqual((facts["revision"], facts["frames"], facts["width"], facts["audio_tracks"][0]["channels"]),
                         ("i", 300, 640, 2))
        self.assertAlmostEqual(facts["duration_seconds"], 10.01, places=2)
        for broken, size in ((header, 999), (header[:40], 1000), (b"BIK!" + header[4:], 1000)):
            with self.assertRaises(mf.MediaError):
                mf.parse_bink(broken, size)

    def test_sniffing(self):
        wav = b"RIFF" + struct.pack("<I", 36) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 22050, 0, 4, 16)
        self.assertEqual(mf.sniff("x", wav, 44)["codec"], "pcm")
        self.assertEqual(mf.sniff("x", b"\x89PNG\r\n\x1a\n" + bytes(8) + struct.pack(">II", 3, 4), 24)["width"], 3)
        self.assertEqual(mf.sniff("x", b"zzzz", 4)["container"], "unknown")


if __name__ == "__main__":
    unittest.main()
