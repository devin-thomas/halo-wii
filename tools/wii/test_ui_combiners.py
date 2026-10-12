"""Tests of the interface's combiner models (HWI-033): the Xbox's plasma program, its TEV form, min/max."""
import random
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_combiners as uc  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


class SourceWordsTests(unittest.TestCase):
    def test_plasma_words_are_the_rasterizers(self):
        """the model's register words are the ones rasterizer_xbox_dynavobgeom.c writes for the plasma case"""
        text = (ROOT / "source/rasterizer/xbox/rasterizer_xbox_dynavobgeom.c").read_text(encoding="utf-8")
        words = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})", text)}
        stages, _, final_efg = uc.plasma_words()
        for rgb_in, rgb_out, alpha_in, alpha_out, _ in stages:
            for word in (rgb_in, rgb_out, alpha_in, alpha_out):
                if word:
                    self.assertIn(word, words | {0x0C010A04}, hex(word))
        # the plasma bit's operand: 0x0C010A00 | 0x00000004
        self.assertIn("0x0C010A00", text)
        self.assertIn("doing_plasma_effect ? 0x00000004", text)
        self.assertIn(final_efg, words)

    def test_tev_program_is_the_one_drawn(self):
        """the GX side holds the stages the model does (compare thresholds, swap, divide)"""
        text = (ROOT / "port/wii/render/render_ui_gx.c").read_text(encoding="utf-8")
        for needle in ("GX_TEV_COMP_R8_GT", "GX_TEV_KCSEL_1_2", "(GXColor){127, 127, 127, 127}",
                       "GX_CH_ALPHA, GX_CH_ALPHA, GX_CH_ALPHA, GX_CH_BLUE", "GX_CS_DIVIDE_2", "GX_CS_SCALE_4",
                       "GX_TB_SUBHALF, GX_CS_SCALE_2", "GX_TEV_COMP_RGB8_EQ"):
            self.assertIn(needle, text)


class CombinerTests(unittest.TestCase):
    def test_mappings_and_mux(self):
        registers = {"r0": (0.0, 0.0, 0.0, 0.6), "t0": (0.25, 0.5, 0.75, 1.0)}
        constants = {"c0": (0.0,) * 4, "c1": (0.0,) * 4}
        # alpha: mux(AB = t0.b * 1, CD = t0.a * 1) on r0.a >= 0.5 -> CD -> r1.a
        uc.combiner_stage(registers | {"r1": (0.0,) * 4}, 0, 0, 0x08201820, 0x00004D00, constants)
        state = {"r0": (0.0, 0.0, 0.0, 0.6), "t0": (0.25, 0.5, 0.75, 1.0), "r1": (0.0,) * 4}
        uc.combiner_stage(state, 0, 0, 0x08201820, 0x00004D00, constants)
        self.assertEqual(state["r1"][3], 1.0)
        state["r0"] = (0.0, 0.0, 0.0, 0.4)
        uc.combiner_stage(state, 0, 0, 0x08201820, 0x00004D00, constants)
        self.assertEqual(state["r1"][3], 0.75)

    def test_plasma_reference_shape(self):
        # equal plasma maps: x = y = 1/2 -> 4 * (1/2)^2 = 1 -> u = 1 -> P = 1: the plasma fade itself
        t = (0.2, 0.4, 0.6, 0.8)
        result = uc.nv2a_plasma(t, t, (0.0, 0.0, 0.0, 0.5), (1.0,) * 4, [(1.0,) * 4] * 3, (0.5, 0.25, 0.125, 1.0))
        self.assertAlmostEqual(result[0], 0.5)
        self.assertAlmostEqual(result[1], 0.25)
        self.assertAlmostEqual(result[2], 0.125)
        self.assertAlmostEqual(result[3], 0.5)


class TevPlasmaTests(unittest.TestCase):
    def test_tev_follows_the_reference(self):
        """the TEV program's 8-bit result against the combiners' (float) result over random cases: the
        documented bound (HWI-033 evidence)"""
        rng = random.Random(33)
        errors = []
        for _ in range(6000):
            def rnd():
                return tuple(rng.random() for _ in range(4))
            t0, t1, t2, v0 = rnd(), rnd(), rnd(), rnd()
            m0 = tuple(rng.random() for _ in range(3)) + (rng.random(),)
            reference, tev = uc.plasma_case(t0, t1, t2, v0, [m0, m0, rnd()], rnd())
            errors.append(max(abs(a - b) for a, b in zip(reference, tev)))
        def share(limit):
            return sum(e <= limit for e in errors) / len(errors)
        self.assertGreaterEqual(share(2), 0.85)
        self.assertGreaterEqual(share(4), 0.95)
        self.assertGreaterEqual(share(8), 0.99)

    def test_tev_exact_cases(self):
        # equal maps: the plasma fade plus map 2 times the vertex colour
        t = (10, 20, 30, 40)
        rgba = uc.tev_plasma(t, t, (255, 0, 0, 128), (255, 255, 255, 255), (255, 255), (255, 255),
                             (255, 255, 255, 255), (100, 50, 25))
        self.assertEqual(rgba, (255, 50, 25, 128))

    def test_tev_arithmetic(self):
        self.assertEqual(uc.tev_regular(0, 200, 255, 0), 200)          # times 1.0 is exact
        self.assertEqual(uc.tev_regular(0, 0, 0, 100, bias=128), 228)
        self.assertEqual(uc.tev_regular(50, 0, 0, 20, sub=True, bias=128), 98)
        self.assertEqual(uc.tev_regular(0, 128, 128, 0, scale=4), 255)  # clamped
        self.assertEqual(uc.tev_regular(0, 7, 255, 9, scale=0.5), 8)


class MinMaxTests(unittest.TestCase):
    def test_compare_stages_are_exact(self):
        for s in range(256):
            for d in range(0, 256, 3):
                self.assertEqual(uc.tev_min_max((s,), (d,), False), (min(s, d),))
                self.assertEqual(uc.tev_min_max((s,), (d,), True), (max(s, d),))


class LogCheckTests(unittest.TestCase):
    def test_check_log(self):
        t0, t1, t2, v0 = (1, 2, 3, 4), (5, 6, 7, 8), (9, 10, 11, 12), (255, 255, 255, 255)
        tev = uc.tev_plasma(t0, t1, t2, v0, (255, 255), (255, 255), (255, 255, 255, 255), (128, 128, 128))
        efb = ",".join("%02x" % x for x in tev[:3])
        log = chr(10).join([
            "BOOT_UI_SELFTEST min case=0 src=10,80,ff dst=20,80,00 efb=10,80,00",
            "BOOT_UI_SELFTEST max case=1 src=10,80,ff dst=20,80,00 efb=20,80,ff",
            "BOOT_UI_SELFTEST plasma case=2 t0=01,02,03,04 t1=05,06,07,08 t2=09,0a,0b,0c v0=ff,ff,ff,ff "
            "m0=ff,ff,ff,ff m1=ff,ff,ff,ff m2=ff,ff,ff,ff pf=80,80,80 efb=" + efb,
            "BOOT_UI_SELFTEST_END cases=3"])
        result = uc.check_log(log)
        self.assertEqual((result["cases"], result["failures"], result["minmax"], result["plasma"]), (3, 0, 2, 1))
        self.assertEqual(uc.check_log(log.replace("efb=10,80,00", "efb=10,80,01"))["failures"], 1)


if __name__ == "__main__":
    unittest.main()
