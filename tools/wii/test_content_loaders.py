"""Host tests of the Wii content loaders (port/wii/content) on authored fixtures.

The loader sources compile for the host with the same strict flags as the Wii
build (C11, -Wall -Wextra -Werror). An authored map is converted, a case list
is staged with content_loader_cases.py (every GX format the fixtures produce,
both recorded-animation codecs, mono and stereo ADPCM, and every malformed
variant), and host_check must decode every valid file to the host digests and
reject every malformed one for the stated reason. Skipped when no host C
compiler is installed; the Dolphin run covers the Wii build.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_convert as cc  # noqa: E402
import content_fixtures as fx  # noqa: E402
import content_loader_cases as lc  # noqa: E402

CONTENT = Path(__file__).resolve().parents[2] / "port" / "wii" / "content"
SOURCES = ("host_check.c", "content_check.c", "content_common.c", "hwt_texture.c", "hra_animation.c",
           "hws_sound.c")
BITMAPS = [("dxt1", 16, 16, 1, "2d", 2, False), ("a8r8g8b8", 8, 8, 1, "2d", 3, False),
           ("ay8", 16, 8, 1, "2d", 1, False), ("r5g6b5", 8, 8, 1, "cube", 1, False),
           ("a8y8", 8, 8, 1, "2d", 2, False), ("a1r5g5b5", 8, 8, 1, "2d", 1, False),
           ("a4r4g4b4", 4, 4, 1, "2d", 0, False), ("a8", 8, 8, 1, "2d", 1, False),
           ("x8r8g8b8", 4, 4, 4, "3d", 2, False), ("dxt5", 8, 8, 1, "2d", 1, False),
           ("r5g6b5", 12, 6, 1, "2d", 0, True)]
V1_TICKS = 22


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
        cls.exe = root / ("host_check.exe" if sys.platform == "win32" else "host_check")
        build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(cls.exe),
                                *(str(CONTENT / name) for name in SOURCES)], capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("host build failed:\n" + build.stderr[-4000:])
        streams = [(4, 4, fx.V4_TICKS, fx.recorded_stream_v4()), (1, 1, V1_TICKS, fx.recorded_stream_v1())]
        sounds = [(0, 0, 1, fx.adpcm_payload(3, 1, 5)), (0, 0, 1, fx.adpcm_payload(70, 1, 9))]
        stereo = [(1, 1, 1, fx.adpcm_payload(2, 2, 6)), (1, 1, 1, fx.adpcm_payload(5, 2, 7))]
        staging = fx.write_staging(root / "staging", {
            "alpha": fx.build_map("alpha", bitmaps=BITMAPS, animation_streams=streams, sounds=sounds),
            "beta": fx.build_map("beta", category=1, sounds=stereo)})
        cls.out = root / "content"
        cc.run(staging, None, ["textures", "animations", "sounds"], "gx-baseline-v1", cls.out)
        cls.cases = root / "cases"
        cls.summary = lc.build(cls.out, cls.cases)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def host_check(self, listing):
        return subprocess.run([str(self.exe), str(listing), str(self.cases.as_posix()) + "/"], capture_output=True,
                              text=True)

    def test_every_case_passes_on_the_host_build(self):
        result = self.host_check(self.cases / "cases-host.txt")
        lines = result.stdout.splitlines()
        failed = [line for line in lines if line.startswith("CASE ") and not line.endswith("result=pass")]
        self.assertEqual((result.returncode, failed), (0, []), result.stdout[-3000:])
        self.assertEqual(lines[-1], "SUMMARY cases=%d passed=%d" % (self.summary["cases"], self.summary["cases"]))
        kinds = {line.split()[2] for line in lines if line.startswith("CASE ") and "expect=ok" in line}
        self.assertEqual(kinds, {"kind=texture", "kind=animation", "kind=sound"})
        rejected = {(line.split()[3], line.split()[4]) for line in lines if " expect=ok " not in line
                    and line.startswith("CASE ")}
        self.assertTrue(all(expect[7:] == got[4:] for expect, got in rejected))
        for error in ("truncated", "length", "magic", "version", "enum", "dimensions", "count", "reserved",
                      "event", "delta", "value", "identity"):
            self.assertIn("expect=" + error, result.stdout, error)

    def test_samples_cover_each_gx_format_and_both_codecs(self):
        public = (self.cases / "cases-public.json").read_text(encoding="utf-8")
        for gx in ("CMPR", "RGBA8", "I8", "IA8", "RGB565", "RGB5A3"):
            self.assertIn('"gx": "%s"' % gx, public, gx)
        for kind in ('"type": "cube"', '"type": "3d"', '"version": 1', '"version": 4', '"channels": 2'):
            self.assertIn(kind, public, kind)
        self.assertNotIn('"map"', public)

    def test_a_changed_texel_or_sample_fails_its_case(self):
        listing = self.cases / "cases-host.txt"
        rows = [line for line in listing.read_text(encoding="ascii").splitlines() if line.startswith("case ")]
        for kind in ("texture", "sound"):
            row = next(r for r in rows if r.split()[2] == kind and r.split()[3] == "ok")
            path = Path(row.split()[4])
            data = bytearray(path.read_bytes())
            data[65 if kind == "texture" else 40] ^= 0x01   # a texel of image 0; a nibble of block 0
            changed = self.cases / ("changed-" + path.name)
            changed.write_bytes(bytes(data))
            parts = row.split()
            parts[4], parts[6] = changed.as_posix(), lc.sha(data)
            one = self.cases / ("one-%s.txt" % kind)
            one.write_text(" ".join(parts) + "\n", encoding="ascii")
            result = self.host_check(one)
            self.assertEqual(result.returncode, 1, kind)
            self.assertIn("decoded_match=0", result.stdout, kind)

    def test_malformed_case_list_is_refused(self):
        bad = self.cases / "bad.txt"
        bad.write_text("case x texture ok /elsewhere/x.hwt 10 %s %s\n" % ("0" * 64, "0" * 64), encoding="ascii")
        self.assertIn("CASES ok=0", self.host_check(bad).stdout)


if __name__ == "__main__":
    unittest.main()
