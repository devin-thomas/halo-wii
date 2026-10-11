"""Tests for the boot mode's staging and build wiring (HWI-016D). No game data."""
import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import boot_stage  # noqa: E402
import engine_build  # noqa: E402
import run_dolphin  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class ManifestTests(unittest.TestCase):
    def test_manifest_text_lines(self):
        text = boot_stage.manifest_text("a" * 64, "0123456789abcdef", "b" * 64,
                                        [("ui", 10, "c" * 64), ("prisoner", 20, "d" * 64)], [("ui", 3, 30)])
        lines = text.splitlines()
        self.assertEqual(lines[0], "# " + boot_stage.MANIFEST_FORMAT)
        self.assertIn("source " + "a" * 64, lines)
        self.assertIn("generation 0123456789abcdef", lines)
        self.assertIn("map ui 10 " + "c" * 64, lines)
        self.assertIn("map prisoner 20 " + "d" * 64, lines)
        self.assertIn("textures ui 3 30", lines)
        self.assertTrue(text.endswith("\n"))

    def test_manifest_rejects_odd_map_names(self):
        with self.assertRaises(boot_stage.StageError):
            boot_stage.manifest_text("a" * 64, "g", "b" * 64, [("../x", 1, "c" * 64)], [])

    def test_source_identity_needs_the_image_hash(self):
        with self.assertRaises(boot_stage.StageError):
            boot_stage.source_identity({"source_chain": {}})
        self.assertEqual(boot_stage.source_identity({"source_chain": {"image_sha256": "e" * 64}}), "e" * 64)


class StageTests(unittest.TestCase):
    def test_stage_copies_maps_and_ui_textures_and_checks_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            generation = base / "gen" / "0011223344556677"
            outputs = [dict(path="textures/ui/00001-000.hwt", **write(generation / "textures/ui/00001-000.hwt", b"t1")),
                       dict(path="textures/other/00002-000.hwt",
                            **write(generation / "textures/other/00002-000.hwt", b"t2"))]
            manifest = {"format": "halo-wii-content-manifest", "source_chain": {"image_sha256": "f" * 64},
                        "outputs": outputs}
            (generation / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            stage_dir = base / "stage"
            write(stage_dir / "ui.wmap", b"HWIMAP01 ui")
            write(stage_dir / "prisoner.wmap", b"HWIMAP01 prisoner")
            out = base / "out"
            record = boot_stage.stage(generation, stage_dir, "ui", ["prisoner"], {}, out)
            self.assertEqual(record["ui_textures"], {"files": 1, "bytes": 2})
            self.assertTrue((out / "data/render/ui/textures/00001-000.hwt").is_file())
            self.assertFalse((out / "data/render/other").exists())
            text = (out / "manifest.txt").read_text(encoding="ascii")
            self.assertIn("map prisoner %d %s" % (len(b"HWIMAP01 prisoner"),
                                                   hashlib.sha256(b"HWIMAP01 prisoner").hexdigest()), text)
            # a generation file that differs from its manifest is refused
            (generation / "textures/ui/00001-000.hwt").write_bytes(b"tX")
            with self.assertRaises(boot_stage.StageError):
                boot_stage.stage(generation, stage_dir, "ui", [], {}, base / "out2")

    def test_missing_staged_map_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            generation = base / "g"
            generation.mkdir()
            (generation / "manifest.json").write_text(json.dumps(
                {"format": "halo-wii-content-manifest", "source_chain": {"image_sha256": "f" * 64}, "outputs": []}),
                encoding="utf-8")
            with self.assertRaises(boot_stage.StageError):
                boot_stage.stage(generation, base / "nothing", "ui", [], {}, base / "out")


class BuildWiringTests(unittest.TestCase):
    def setUp(self):
        self.previous = os.getcwd()
        os.chdir(ROOT)

    def tearDown(self):
        os.chdir(self.previous)

    def test_boot_units_and_wraps(self):
        self.assertIn(Path("port/wii/engine/boot_hooks.c"), engine_build.ENGINE_AUTHORED)
        names = {path.name for path in engine_build.WII_PLATFORM}
        self.assertTrue({"wii_xinput.c", "wii_loopback_net.c"} <= names)
        self.assertIn(Path("port/linux/src/xnet.c"), engine_build.REUSED_PLATFORM)
        self.assertIn(Path("port/wii/render/render_ui.c"), engine_build.RENDER_ENGINE)
        self.assertIn(Path("port/wii/render/render_ui_gx.c"), engine_build.RENDER_GX)
        for unit in engine_build.BOOT_DRIVER:
            self.assertTrue((ROOT / unit).is_file(), unit)
            self.assertNotIn(unit, engine_build.WII_PLATFORM)
        for name in ("_rasterizer_initialize", "render_frame", "render_frame_present", "rasterizer_text_draw_character",
                     "main_set_map_name", "system_milliseconds", "Sleep", "cache_file_open",
                     "cache_files_precache_map_begin", "game_precache_new_map", "sound_initialize"):
            self.assertIn(f"-Wl,--wrap={name}", engine_build.LINK_FLAGS)
        # every boot wrap is defined in the unit both builds compile
        text = (ROOT / "port/wii/engine/boot_hooks.c").read_text(encoding="utf-8")
        for name in engine_build.WRAPPED[engine_build.WRAPPED.index("_rasterizer_initialize"):]:
            self.assertIn(f"__wrap_{name}(", text, name)


class DolphinProfileTests(unittest.TestCase):
    def test_safe_texture_cache_option(self):
        files = run_dolphin.profile_files("D3D", False, False, True)
        self.assertIn("SafeTextureCacheColorSamples = 0", files["GFX.ini"])
        self.assertNotIn("SafeTextureCacheColorSamples", run_dolphin.profile_files("D3D")["GFX.ini"])


class HostReplayTests(unittest.TestCase):
    def test_pad_and_arena_lines(self):
        import run_boot_host
        log = "\n".join([
            "ARENA open offset=4096 low=0x90002000 high_before=0x933db7e0 high_during=0x9",
            "BOOT_PAD scan=0 clock_ms=0 connected=1 buttons=0000 stick=0,0 cstick=0,0 triggers=0,0",
            "BOOT_PAD scan=498 clock_ms=16401 connected=1 buttons=0104 stick=-3,7 cstick=0,0 triggers=0,255"])
        self.assertEqual(run_boot_host.pad_lines(log), ["0 0 1 0000 0 0 0 0 0 0", "0 498 1 0104 -3 7 0 0 0 255"])
        self.assertEqual(run_boot_host.arena_of(log), ("0x90002000", "0x933db7e0", "4096"))


class TwoPortTests(unittest.TestCase):
    def test_two_ports_interleave_per_poll(self):
        script = {"port": 1, "ports": 2, "steps": [{"polls": 2, "buttons": ["A"], "buttons2": ["START"]}]}
        body = run_dolphin.pad_entries(script)
        self.assertEqual(len(body), 4 * 8)
        one, two = body[0:8], body[8:16]
        self.assertEqual(one[0:2], (run_dolphin.PAD_CONNECTED | run_dolphin.PAD_BUTTONS["A"]).to_bytes(2, "little"))
        self.assertEqual(two[0:2], (run_dolphin.PAD_CONNECTED | run_dolphin.PAD_BUTTONS["START"]).to_bytes(2, "little"))
        self.assertEqual(body[16:32], body[0:16])
        movie = run_dolphin.author_dtm(b"ID-eng", body, ports=2)
        self.assertEqual(movie[run_dolphin.DTM_CONTROLLERS], 0x03)
        self.assertIn("SIDevice1 = 6", run_dolphin.profile_files("D3D", gc_ports=2)["Dolphin.ini"])
        self.assertIn("SIDevice1 = 0", run_dolphin.profile_files("D3D")["Dolphin.ini"])
        with self.assertRaises(ValueError):
            run_dolphin.pad_entries({"ports": 3, "steps": [{"polls": 1}]})


if __name__ == "__main__":
    unittest.main()
