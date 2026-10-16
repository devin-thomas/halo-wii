"""Tests of the Wii's gameplay HUD (HWI-087): the meter program's models and the presentation seams.

The meter: the model's register words are the rasterizer's, the TEV form follows the NV2A reference, the
kill and the threshold are exact. The seams: the boot mode's HUD pass calls only the presentation entry
points, which swap their module's game-state pointer for a copy and restore it; nothing the original
hud_draw_screen ticks, plays or updates is called from the frame. These are static checks of the code the
frame reaches; the game state's digests across a drawn HUD are the runtime qualification (the root's).
"""
import random
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_combiners as uc  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def _text(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def _function(text, name):
    """the text of a C function definition (from its name at the start of a line to its closing brace)"""
    match = re.search(r"^(?:static )?[A-Za-z_][^\n;]*\b%s\(\n?[^;{]*\)\s*\{" % re.escape(name), text, re.M)
    assert match, name
    depth, index = 0, match.end() - 1
    while True:
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
        if depth == 0:
            return text[match.start():index]


def _meter(rnd, gain):
    minimum = rnd.randrange(256)
    maximum = max(minimum, rnd.randrange(256))
    return {"gradient_min": (minimum << 24) | rnd.randrange(1 << 24), "gradient_max": rnd.randrange(1 << 24),
            "flash": (maximum << 24) | rnd.choice([0, rnd.randrange(1 << 24)]),
            "background": rnd.randrange(1 << 32),
            "tint": (rnd.randrange(256) << 24) | (0x010101 * rnd.randrange(256)), "gain": gain,
            "flash_negative": rnd.random() < 0.1}


class MeterWordsTests(unittest.TestCase):
    def test_meter_words_are_the_rasterizers(self):
        text = _text("source/rasterizer/xbox/rasterizer_xbox_dynavobgeom.c")
        words = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})", text)}
        stages, final_abcd, final_efg = uc.meter_words()
        for rgb_in, rgb_out, alpha_in, alpha_out, _ in stages:
            for word in (rgb_in, rgb_out, alpha_in, alpha_out):
                self.assertIn(word, words | {0x0C201C02}, hex(word))
        self.assertIn("0x0C201C00 | (meter->flash_color_is_negative ? 0x000000E2 : 0x00000002)", text)
        self.assertIn(final_abcd, words)
        self.assertIn(final_efg, words)
        for needle in ("D3DTALPHAKILL_ENABLE", "D3DBLEND_CONSTANTCOLOR", "D3DBLEND_SRCALPHA",
                       "meter->tint_mode_2", "meter->gradient==1.0f"):
            self.assertIn(needle, text)

    def test_gx_side_holds_the_program(self):
        body = _function(_text("port/wii/render/render_ui_gx.c"), "wii_ui_gx_meter")
        for needle in ("GX_BL_ONE, GX_BL_SRCALPHA", "GX_TEV_COMP_R8_GT", "GX_TEV_COMP_A8_GT", "GX_TEV_COMP_A8_EQ",
                       "GX_TB_ADDHALF, GX_CS_SCALE_2", "GX_SetNumTevStages(8)", "tev_swaps_reset(8)",
                       "GX_SetBlendMode(GX_BM_NONE"):
            self.assertIn(needle, body)
        # the swap table borrowed is put back to its default afterwards
        borrowed = body.index("GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE")
        restored = body.index("GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA")
        self.assertLess(borrowed, restored)
        self.assertGreater(restored, body.index("GX_End()"))


class MeterModelTests(unittest.TestCase):
    def test_tev_follows_the_reference(self):
        """within 2/255 everywhere, and within 1/255 almost everywhere, for either packing of 1/8"""
        rnd = random.Random(87)
        histogram = {}
        for _ in range(20000):
            meter = _meter(rnd, rnd.choice([31, 32]))
            texel = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256),
                     rnd.choice([0, 255, rnd.randrange(256)]))
            destination = tuple(rnd.randrange(256) for _ in range(3))
            a = uc.nv2a_meter(texel, meter, destination)
            b = uc.tev_meter(texel, meter, destination)
            difference = max(abs(x - y) for x, y in zip(a, b))
            histogram[difference] = histogram.get(difference, 0) + 1
        self.assertLessEqual(max(histogram), 2, histogram)
        self.assertGreater(histogram.get(0, 0) + histogram.get(1, 0), 0.99 * 20000, histogram)

    def test_kill_keeps_the_destination(self):
        rnd = random.Random(1)
        for _ in range(500):
            meter = _meter(rnd, 32)
            destination = tuple(rnd.randrange(256) for _ in range(3))
            texel = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 0)
            self.assertEqual(uc.nv2a_meter(texel, meter, destination), destination)
            self.assertEqual(uc.tev_meter(texel, meter, destination), destination)

    def test_threshold_is_exact(self):
        """the texels at or past the maximum value's alpha are the background's, those below are filled"""
        meter = {"gradient_min": 0x80FF0000, "gradient_max": 0x0000FF00, "flash": 0xA0000000,
                 "background": 0x400000FF, "tint": 0xFFFFFFFF, "gain": 32}
        for u in range(256):
            texel = (0, 0, u, 255)
            for model in (uc.nv2a_meter, uc.tev_meter):
                out = model(texel, meter, (0, 0, 0))
                if u >= 0xA0:
                    self.assertEqual(out, (0, 0, 255), (u, model.__name__))
                else:
                    self.assertEqual(out[2], 0, (u, model.__name__))
                    self.assertGreater(out[0] + out[1], 0, (u, model.__name__))

    def test_flash_only_while_fading(self):
        """hud_draw_meter's flash colour is the tag's times the fade: with none, nothing is added"""
        meter = {"gradient_min": 0x80202020, "gradient_max": 0x00202020, "flash": 0xC0000000,
                 "background": 0x00000000, "tint": 0xFFFFFFFF, "gain": 32}
        flashing = dict(meter, flash=0xC0FFFFFF)
        texel = (0, 0, 0x90, 255)                  # between the value and the maximum: the recent damage
        for model in (uc.nv2a_meter, uc.tev_meter):
            self.assertEqual(model(texel, meter, (0, 0, 0)), (32, 32, 32), model.__name__)
            self.assertEqual(model(texel, flashing, (0, 0, 0)), (255, 255, 255), model.__name__)


class PresentationSeamTests(unittest.TestCase):
    UPDATES = ("hud_draw_screen", "interface_draw_screen", "motion_sensor_tick", "hud_play_unit_sounds",
               "hud_show_action_response", "hud_messaging_update", "hud_update(", "hud_update_unit",
               "hud_update_weapon", "hud_render_damage_indicators", "player_effect_clear_damage_indicators",
               "hud_render_unit_interface(", "hud_render_weapon_interface(")

    def test_frame_calls_only_the_presentation_entry_points(self):
        text = _text("port/wii/render/render_ui.c")
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for name in self.UPDATES:
            self.assertNotIn(name, code, name)
        body = _function(text, "boot_ui_hud")
        weapon = body.index("hud_render_weapon_interface_presentation(player)")
        unit = body.index("hud_render_unit_interface_presentation(player)")
        self.assertLess(weapon, unit)                                   # hud_draw_screen's order
        self.assertLess(body.index("boot_ui.hud_pass = TRUE"), weapon)
        self.assertGreater(body.index("boot_ui.hud_pass = FALSE"), unit)
        for needle in ("hud_scripted_globals->show_hud", "_director_perspective_neutral",
                       "_director_perspective_scripted", "player->unit_index == NONE", "boot_ui.config.hud",
                       "local_player_get_player_index(local_player_index)", "boot_ui_hud_reset()"):
            self.assertIn(needle, body)
        # drawn where render_window draws interface_draw_screen: before the widgets
        interface = _function(text, "boot_ui_interface")
        self.assertLess(interface.index("boot_ui_hud(local_player_index)"), interface.index("render_ui_widgets("))

    def test_presentation_draws_on_a_copy_and_restores(self):
        for relative, name, pointer in (
                ("source/interface/hud_unit.c", "hud_render_unit_interface_presentation", "unit_hud_globals"),
                ("source/interface/hud_weapon.c", "hud_render_weapon_interface_presentation", "weapon_hud_globals")):
            body = _function(_text(relative), name)
            swap = body.index("%s = &scratch;" % pointer)
            restore = body.index("%s = authoritative;" % pointer)
            draw = body.index(name.replace("_presentation", "") + "(player);")
            self.assertLess(body.index("scratch = *authoritative;"), swap)
            self.assertLess(swap, draw)
            self.assertLess(draw, restore)
            # nothing between the swap and the restore but the draw and the active flag
            between = [line.strip() for line in body[swap:restore].splitlines()[1:] if line.strip()]
            self.assertTrue(all("presentation.active" in line or line.endswith("(player);") for line in between),
                            between)
            # the game state's globals are not written through their pointer here
            self.assertNotRegex(body, r"\b%s\s*->" % pointer)

    def test_unit_presentation_refuses_motion_sensor_and_tag_write(self):
        text = _text("source/interface/hud_unit.c")
        self.assertIn("game_engine_hud_draw_motion_sensor(player_index) &&\n\t\t\t\t\t"
                      "unit_hud_presentation_draws_motion_sensor())", text)
        self.assertRegex(text, r"if \(unit_hud_presentation\.active\)\s*\{\s*team_element = overlay->static_element;")

    def test_hud_bitmaps_answered_only_while_the_hud_draws(self):
        body = _function(_text("port/wii/render/render_ui.c"), "boot_ui_bitmap_hardware_format")
        self.assertLess(body.index("if (boot_ui.hud_pass)"), body.index("if (!boot_ui_in_menu_map())"))

    def test_off_switch(self):
        main = _text("port/wii/engine/boot_main.c")
        self.assertIn('!strncmp(line, "hud=", 4)', main)
        self.assertIn("boot.hud = 1;", main)
        self.assertIn("config.hud = boot.hud;", main)


if __name__ == "__main__":
    unittest.main()
