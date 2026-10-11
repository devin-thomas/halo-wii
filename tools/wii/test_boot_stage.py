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
        text = boot_stage.manifest_text("a" * 64, "0123456789abcdef", "b" * 64, [("ui", 3, 30)])
        lines = text.splitlines()
        self.assertEqual(lines[0], "# " + boot_stage.MANIFEST_FORMAT)
        self.assertIn("source " + "a" * 64, lines)
        self.assertIn("generation 0123456789abcdef", lines)
        self.assertIn("textures ui 3 30", lines)
        self.assertFalse(any(line.startswith("map ") for line in lines))
        self.assertTrue(text.endswith("\n"))

    def test_odd_map_names_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(boot_stage.StageError):
                boot_stage.copy_map(Path(temporary), Path(temporary) / "out", "../x")

    def test_source_identity_needs_the_image_hash(self):
        with self.assertRaises(boot_stage.StageError):
            boot_stage.source_identity({"source_chain": {}})
        self.assertEqual(boot_stage.source_identity({"source_chain": {"image_sha256": "e" * 64}}), "e" * 64)


def maps_manifest_row(name, data):
    return f"{name} 2 {len(data)} 0000abcd {hashlib.sha256(data).hexdigest()}"


class StageTests(unittest.TestCase):
    def generation(self, base):
        generation = base / "gen" / "0011223344556677"
        outputs = [dict(path="textures/ui/00001-000.hwt", **write(generation / "textures/ui/00001-000.hwt", b"t1")),
                   dict(path="textures/other/00002-000.hwt",
                        **write(generation / "textures/other/00002-000.hwt", b"t2"))]
        manifest = {"format": "halo-wii-content-manifest", "source_chain": {"image_sha256": "f" * 64},
                    "outputs": outputs}
        (generation / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return generation

    def stage_dir(self, base, maps, listed=None):
        stage_dir = base / "stage"
        stage_dir.mkdir()
        for name, data in maps.items():
            write(stage_dir / f"{name}.wmap", data)
        rows = ["HWIMAP-MANIFEST 2"] + [maps_manifest_row(name, maps[name]) for name in (listed or maps)]
        (stage_dir / "manifest.txt").write_text("\n".join(rows) + "\n", encoding="ascii")
        return stage_dir

    def test_stage_copies_maps_their_manifest_and_ui_textures(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            generation = self.generation(base)
            maps = {"ui": b"HWIMAP02 ui", "prisoner": b"HWIMAP02 prisoner", "other": b"HWIMAP02 other"}
            stage_dir = self.stage_dir(base, maps)
            out = base / "out"
            record = boot_stage.stage(generation, stage_dir, "ui", ["prisoner"], {}, out)
            self.assertEqual(record["ui_textures"], {"files": 1, "bytes": 2})
            self.assertTrue((out / "data/render/ui/textures/00001-000.hwt").is_file())
            self.assertFalse((out / "data/render/other").exists())
            self.assertFalse((out / "data/maps/other.wmap").exists())
            # the maps manifest: map_stage.py's lines for the maps copied, unchanged
            rows = (out / "data/maps/manifest.txt").read_text(encoding="ascii").splitlines()
            self.assertEqual(rows, ["HWIMAP-MANIFEST 2", maps_manifest_row("ui", maps["ui"]),
                                    maps_manifest_row("prisoner", maps["prisoner"])])
            self.assertIn({"name": "prisoner", "bytes": len(maps["prisoner"]),
                           "sha256": hashlib.sha256(maps["prisoner"]).hexdigest()}, record["maps"])
            # a generation file that differs from its manifest is refused
            (generation / "textures/ui/00001-000.hwt").write_bytes(b"tX")
            with self.assertRaises(boot_stage.StageError):
                boot_stage.stage(generation, stage_dir, "ui", [], {}, base / "out2")

    def test_a_map_the_manifest_does_not_list_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            generation = self.generation(base)
            stage_dir = self.stage_dir(base, {"ui": b"HWIMAP02 ui", "prisoner": b"HWIMAP02 p"}, listed=["ui"])
            with self.assertRaises(boot_stage.StageError):
                boot_stage.stage(generation, stage_dir, "ui", ["prisoner"], {}, base / "out")
            self.assertFalse((base / "out").exists())
            (stage_dir / "manifest.txt").unlink()
            with self.assertRaises(boot_stage.StageError):
                boot_stage.stage(generation, stage_dir, "ui", [], {}, base / "out")

    def test_missing_staged_map_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            generation = self.generation(base)
            stage_dir = self.stage_dir(base, {"ui": b"HWIMAP02 ui"})
            (stage_dir / "ui.wmap").unlink()
            with self.assertRaises(boot_stage.StageError):
                boot_stage.stage(generation, stage_dir, "ui", [], {}, base / "out")


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

    def test_maps_manifest_from_the_little_endian_lines(self):
        import run_boot_host
        with tempfile.TemporaryDirectory() as temporary:
            stage_dir = Path(temporary)
            (stage_dir / "manifest.le.txt").write_text(
                "HWIMAP-MANIFEST 2\nui 2 5 00000001 " + "a" * 64 + "\nprisoner 2 7 00000002 " + "b" * 64 + "\n",
                encoding="ascii")
            text = run_boot_host.maps_manifest([stage_dir / "ui.le.wmap", stage_dir / "prisoner.le.wmap"])
            self.assertEqual(text.splitlines(), ["HWIMAP-MANIFEST 2", "prisoner 2 7 00000002 " + "b" * 64,
                                                 "ui 2 5 00000001 " + "a" * 64])
            with self.assertRaises(SystemExit):
                run_boot_host.maps_manifest([stage_dir / "beavercreek.le.wmap"])


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
