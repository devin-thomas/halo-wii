"""Host tests of the Wii content loaders (port/wii/content) on authored fixtures.

The loader sources compile for the host with the same strict flags as the Wii
build (C11, -Wall -Wextra -Werror). An authored map with every content
category is converted, a case list is staged with content_loader_cases.py
(every GX format the fixtures produce, both recorded-animation codecs, mono and
stereo ADPCM, models, lightmap geometry, collision, model animations with a
compressed block, fonts and unicode text, every malformed variant and the
wrong-format controls), and host_check must decode every valid file to the
host digests and reject every malformed one for the stated reason. Skipped
when no host C compiler is installed; the Dolphin run covers the Wii build.
"""
from pathlib import Path
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import be_records as br  # noqa: E402
import content_convert as cc  # noqa: E402
import content_decoded as cd  # noqa: E402
import content_fixtures as fx  # noqa: E402
import content_geometry as cg  # noqa: E402
import content_loader_cases as lc  # noqa: E402

CONTENT = Path(__file__).resolve().parents[2] / "port" / "wii" / "content"
SOURCES = ("host_check.c", "content_check.c", "content_common.c", "content_sections.c", "hwt_texture.c",
           "hra_animation.c", "hws_sound.c", "hwm_model.c", "hwl_lightmap.c", "hwc_collision.c", "hma_graph.c",
           "hwf_font.c", "hus_strings.c")
BITMAPS = [("dxt1", 16, 16, 1, "2d", 2, False), ("a8r8g8b8", 8, 8, 1, "2d", 3, False),
           ("ay8", 16, 8, 1, "2d", 1, False), ("r5g6b5", 8, 8, 1, "cube", 1, False),
           ("a8y8", 8, 8, 1, "2d", 2, False), ("a1r5g5b5", 8, 8, 1, "2d", 1, False),
           ("a4r4g4b4", 4, 4, 1, "2d", 0, False), ("a8", 8, 8, 1, "2d", 1, False),
           ("x8r8g8b8", 4, 4, 4, "3d", 2, False), ("dxt5", 8, 8, 1, "2d", 1, False),
           ("r5g6b5", 12, 6, 1, "2d", 0, True)]
V1_TICKS = 22
DECODED = ("model", "lightmap", "collision", "model_animation", "font", "strings")


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


@unittest.skipUnless(compiler(), "no host C compiler")
class HostLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        cls.root = root
        cls.exe = root / ("host_check.exe" if sys.platform == "win32" else "host_check")
        build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                                "-o", str(cls.exe), *(str(CONTENT / name) for name in SOURCES)],
                               capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("host build failed:\n" + build.stderr[-4000:])
        streams = [(4, 4, fx.V4_TICKS, fx.recorded_stream_v4()), (1, 1, V1_TICKS, fx.recorded_stream_v1())]
        sounds = [(0, 0, 1, fx.adpcm_payload(3, 1, 5)), (0, 0, 1, fx.adpcm_payload(70, 1, 9))]
        stereo = [(1, 1, 1, fx.adpcm_payload(2, 2, 6)), (1, 1, 1, fx.adpcm_payload(5, 2, 7))]
        staging = fx.write_staging(root / "staging", {
            "alpha": fx.build_map("alpha", bitmaps=BITMAPS, animation_streams=streams, sounds=sounds,
                                  categories=set()),
            "beta": fx.build_map("beta", category=1, sounds=stereo, categories=set())})
        cls.out = root / "content"
        cc.run(staging, None, list(cc.CATEGORIES), "gx-baseline-v1", cls.out)
        cls.cases = root / "cases"
        cls.summary = lc.build(cls.out, cls.cases)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def host_check(self, listing, prefix=None):
        prefix = prefix or str(self.cases.as_posix()) + "/"
        return subprocess.run([str(self.exe), str(listing), prefix], capture_output=True, text=True)

    def rows(self):
        listing = self.cases / "cases-host.txt"
        return [line for line in listing.read_text(encoding="ascii").splitlines() if line.startswith("case ")]

    def single(self, row, data, name):
        """A one-case list for row with its file replaced by data."""
        parts = row.split()
        changed = self.cases / name
        changed.write_bytes(bytes(data))
        parts[4], parts[5], parts[6] = changed.as_posix(), str(len(data)), lc.sha(data)
        listing = self.cases / (name + ".txt")
        listing.write_text(" ".join(parts) + "\n", encoding="ascii")
        return self.host_check(listing)

    def test_every_case_passes_on_the_host_build(self):
        result = self.host_check(self.cases / "cases-host.txt")
        lines = result.stdout.splitlines()
        failed = [line for line in lines if line.startswith("CASE ") and not line.endswith("result=pass")]
        self.assertEqual((result.returncode, failed), (0, []), result.stdout[-3000:])
        self.assertEqual(lines[-1], "SUMMARY cases=%d passed=%d" % (self.summary["cases"], self.summary["cases"]))
        kinds = {line.split()[2] for line in lines if line.startswith("CASE ") and "expect=ok" in line}
        self.assertEqual(kinds, {"kind=" + kind for kind in lc.KINDS})
        rejected = {(line.split()[3], line.split()[4]) for line in lines if " expect=ok " not in line
                    and line.startswith("CASE ")}
        self.assertTrue(all(expect[7:] == got[4:] for expect, got in rejected))
        for error in ("truncated", "length", "magic", "version", "enum", "dimensions", "count", "reserved",
                      "event", "delta", "value", "identity", "section", "range"):
            self.assertIn("expect=" + error, result.stdout, error)
        decoded = [line for line in lines if " expect=ok " in line and line.split()[2][5:] in DECODED]
        self.assertTrue(all(" decoded_bytes=0 " not in line for line in decoded if " kind=model " in line))

    def test_samples_cover_each_format_and_the_controls(self):
        public = json.loads((self.cases / "cases-public.json").read_text(encoding="utf-8"))
        text = json.dumps(public)
        for gx in ("CMPR", "RGBA8", "I8", "IA8", "RGB565", "RGB5A3"):
            self.assertIn('"gx": "%s"' % gx, text, gx)
        for kind in ('"type": "cube"', '"type": "3d"', '"version": 1', '"version": 4', '"channels": 2'):
            self.assertIn(kind, text, kind)
        self.assertNotIn('"map"', text)
        cases = public["cases"]
        for kind in DECODED:
            valid = [c for c in cases if c["kind"] == kind and c["expect"] == "ok"]
            self.assertTrue(valid and all(c["roles"] for c in valid), kind)
            self.assertGreaterEqual(sum(1 for c in cases if c["kind"] == kind and c["expect"] != "ok"), 10, kind)
        roles = {role for c in cases for role in c.get("roles") or ()}
        self.assertTrue({"largest", "largest_collision_model", "largest_structure_bsp", "compressed_largest",
                         "most_glyphs", "longest_list", "hud_text"} <= roles, roles)
        self.assertTrue(all(c["source_identity"] for c in cases if "source_identity" in c))
        controls = [c for c in cases if c.get("control") == "wrong_format"]
        self.assertEqual(sorted((c["kind"], c["expect"]) for c in controls),
                         [("lightmap", "magic"), ("lightmap", "section"), ("strings", "magic")])

    def test_a_changed_texel_sample_or_vertex_fails_its_case(self):
        rows = self.rows()
        for kind, at in (("texture", 65), ("sound", 40)):   # a texel of image 0; a nibble of block 0
            row = next(r for r in rows if r.split()[2] == kind and r.split()[3] == "ok")
            data = bytearray(Path(row.split()[4]).read_bytes())
            data[at] ^= 0x01
            result = self.single(row, data, "changed-" + kind)
            self.assertEqual(result.returncode, 1, kind)
            self.assertIn("decoded_match=0", result.stdout, kind)
        # one bit of a packed model normal: still a valid container, a different decoded vertex
        row = next(r for r in rows if r.split()[2] == "model" and r.split()[3] == "ok")
        data = bytearray(Path(row.split()[4]).read_bytes())
        vertices = lc._table(bytes(data))[1][3]
        data[vertices + 12 + 3] ^= 0x04
        result = self.single(row, data, "changed-model")
        self.assertIn("got=ok", result.stdout)
        self.assertIn("decoded_match=0", result.stdout)

    def test_packed_vector_extremes_match_the_host_decoder(self):
        packed = [0, 0x400, 0x3FF, 0x7FF, 0x400 << 11, 0x3FF << 11, 0x200 << 22, 0x1FF << 22, 0xFFFFFFFF, 0x80000000,
                  0x7FFFFFFF, 0x12345678]
        vertices = b"".join(struct.pack(">3f3I2h2Bh", 1.5, -2.0, 0.0, p, p ^ 0xFFFFFFFF, p >> 1, -32768, 32767, 0, 3,
                                        -1) for p in packed)
        part = struct.pack(">HHIhbbhhIIIIIhhIIII", 0, 0, 0, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 5, 1, len(packed), 0, 3, 0)
        blob = br.pack_container(b"HWM1", [(1, 56, 1, part), (2, 32, len(packed), vertices),
                                           (3, 2, 3, struct.pack(">3H", 0, len(packed) - 1, 5))])
        expected, _ = cd.model_digest(blob)
        path = self.cases / "extremes.hwm"
        path.write_bytes(blob)
        listing = self.cases / "extremes.txt"
        listing.write_text("case x model ok %s %d %s %s\n" % (path.as_posix(), len(blob), lc.sha(blob), expected),
                           encoding="ascii")
        result = self.host_check(listing)
        self.assertEqual(result.returncode, 0, result.stdout)
        # the engine formula on the extremes: (2c + 1) / 2047 and (2c + 1) / 1023 in f32
        self.assertEqual(cd.unpack_vector(0x400 | 0x3FF << 11 | 0x200 << 22),
                         (cd.f32(-2047 * cd.INV_2047), cd.f32(2047 * cd.INV_2047), cd.f32(-1023 * cd.INV_1023)))
        self.assertEqual(cd.unpack_vector(0)[0], cd.f32(1 / 2047))

    def test_sweep_lists_every_decoded_output_and_each_passes(self):
        summary = lc.sweep(self.out, self.root / "sweep")
        # both authored maps carry every category
        self.assertEqual(summary["by_kind"], {"model": 2, "lightmap": 2, "collision": 4, "model_animation": 2,
                                              "font": 2, "strings": 4})
        self.assertEqual(summary["source_identity_matches"], 8)
        for listing in sorted((self.root / "sweep").glob("sweep-*.txt")):
            result = self.host_check(listing, summary["prefix"])
            self.assertEqual(result.returncode, 0, result.stdout[-2000:])

    def test_malformed_case_list_is_refused(self):
        bad = self.cases / "bad.txt"
        bad.write_text("case x texture ok /elsewhere/x.hwt 10 %s %s\n" % ("0" * 64, "0" * 64), encoding="ascii")
        self.assertIn("CASES ok=0", self.host_check(bad).stdout)

    def test_collision_fixture_indices_follow_the_engine_encoding(self):
        entry = next(e for e in json.loads(next((self.out / "generations").glob("*/manifest.json")).read_text())
                     ["outputs"] if e["kind"] == "collision")
        blob = next((self.out / "generations").glob("*/" + entry["path"])).read_bytes()
        sections = cg.unpack_collision(blob)
        nodes = br.decode_records(sections[2][1], "iii", sections[2][0], ">")
        self.assertIn(-1, nodes)                                  # NONE child
        self.assertTrue(any(v < -1 for v in nodes[1::3] + nodes[2::3]))   # leaf children


if __name__ == "__main__":
    unittest.main()
