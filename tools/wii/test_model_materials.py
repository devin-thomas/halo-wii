"""HWI-086: the shader_model base map material of the models.

- port/wii/render/render_model_material.c and hwm_texcoord_decode_le, built
  with the host compiler under the strict flags of the Wii build and run
  (model_material_check.c): the state rules of _rasterizer_model_draw (alpha
  test unless not_alpha_tested or an alpha-blended decal, reference 0x7F;
  decals blended without depth writes; two_sided both faces, not
  nocull_two_sided), the NORMSHORT2 texcoord (s / 32767) through the static
  texture rows and the GX matrix that folds the normalisation in, and the
  bounded bitmap residency of the models: keys, the table bound, lookups,
  loads per frame and bytes, no eviction of a texture wanted last frame, the
  GPU wait before reused bytes, missing, oversize, invalid and backed-off
  files, generations. Skipped when no host C compiler is installed.
- render_stage.py selects the model base maps (every bitmap of each
  shader_model base map a model names) from authored records.
- The GX and engine sides use the evidenced layouts and states, and the
  reserve stays a bounded block of the BSP store, separate from the
  environment's cache and given back after GX is done with it.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RENDER = ROOT / "port" / "wii" / "render"
CONTENT = ROOT / "port" / "wii" / "content"
sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_stage as rs  # noqa: E402
import engine_build  # noqa: E402

SOURCES = (ROOT / "tools" / "wii" / "model_material_check.c", RENDER / "render_model_material.c",
           RENDER / "render_texture_cache.c", CONTENT / "hwm_model.c", CONTENT / "content_common.c",
           CONTENT / "content_sections.c")


def compiler():
    for name in ("gcc", "clang", "cc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def function_body(text, signature):
    start = text.index(signature)
    open_brace = text.index("{", start)
    end = text.index("\n}\n", open_brace)
    return text[open_brace:end]


@unittest.skipUnless(compiler(), "no host C compiler")
class MaterialRulesOnTheHost(unittest.TestCase):
    def test_check_program(self):
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / ("model_material_check" + (".exe" if sys.platform == "win32" else ""))
            command = [compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-ffp-contract=off",
                       f"-I{RENDER}", f"-I{CONTENT}", *map(str, SOURCES), "-o", str(program)]
            if sys.platform != "win32":
                command.append("-lm")
            subprocess.run(command, check=True, capture_output=True)
            result = subprocess.run([str(program)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("MODEL_MATERIAL_CHECK ok", result.stdout)


class StagingTests(unittest.TestCase):
    def test_layout_offsets(self):
        # rasterizer_xbox_models.c asserts base_map.index at 0xB0; the model's
        # shaders block follows its geometries (content_geometry.py)
        self.assertEqual(rs.SHADER_MODEL_BASE_MAP_INDEX, 0xB0)
        self.assertEqual(rs.MODEL_SHADERS, 220)
        self.assertEqual(rs.MODEL_SHADER_REFERENCE_BYTES, 0x20)
        self.assertEqual(rs.SHADER_MODEL, 4)

    def test_selects_every_bitmap_of_model_base_maps(self):
        shaders = {0xE0000020: (4, 0xA0000028), 0xE0000021: (4, 0xA0000029), 0xE0000022: (3, 0xA000002A),
                   0xE0000023: (4, rs.NONE), 0xE0000024: (5, rs.NONE), 0xE0000025: (4, 0xA000002B)}
        references = [0xE0000020, 0xE0000021, 0xE0000020, 0xE0000022, 0xE0000023, 0xE0000024, 0xE0000025]
        counts = {40: 1, 41: 3, 43: 0}
        files = rs.select_models("map", references, shaders, counts)
        self.assertEqual(files, [
            ("render/map/textures/00040-000.hwt", "model_base", 2),
            ("render/map/textures/00041-000.hwt", "model_base", 1),
            ("render/map/textures/00041-001.hwt", "model_base", 1),
            ("render/map/textures/00041-002.hwt", "model_base", 1)])


class SourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gx = (RENDER / "render_models_gx.c").read_text(encoding="utf-8")
        cls.engine = (RENDER / "render_objects_engine.c").read_text(encoding="utf-8")
        cls.render_gx = (RENDER / "render_gx.c").read_text(encoding="utf-8")

    def test_engine_reads_the_asserted_shader_model_layout(self):
        for field, offset in (("flags", "0x28"), ("map_u_scale", "0x9C"), ("base_map.index", "0xB0"),
                              ("multipurpose_map.index", "0xC8"), ("detail_map.index", "0xE8"),
                              ("texture_animation", "0xFC"), ("reflection_cube_map.index", "0x170")):
            self.assertRegex(self.engine, r"offsetof\(struct render_objects_shader_model, " + re.escape(field) +
                             r"\) == " + offset)
        material = function_body(self.engine, "static void render_objects_part_material(\n")
        self.assertIn("shader_permutation_index % bitmap_count", material)
        self.assertIn("bitmap->type != RENDER_OBJECTS_BITMAP_TYPE_2D", material)
        self.assertIn("model->base_map_scale.i * shader_model->map_u_scale", material)
        self.assertIn("model->base_map_scale.j * shader_model->map_v_scale", material)
        parts = function_body(self.engine, "long wii_render_model_parts(\n")
        self.assertIn("forced_shader_permutation_index ? forced_shader_permutation_index : "
                      "reference->permutation_index", parts)
        self.assertIn("object->object.forced_shader_permutation_index", self.engine)

    def test_gx_samples_and_tests_as_the_xbox(self):
        load = function_body(self.gx, "static int texture_load(")
        self.assertEqual(load.count("GX_REPEAT, GX_REPEAT"), 2)
        self.assertEqual(load.count("GX_LIN_MIP_LIN, GX_LINEAR"), 2)
        self.assertIn("header.type != HWT_TYPE_2D", load)
        self.assertIn("(int)header.width != slot->width", load)
        base = function_body(self.gx, "static void apply_base_map(")
        self.assertIn("GX_SetAlphaCompare(GX_GREATER, (u8)WII_MODEL_ALPHA_REFERENCE", base)
        self.assertIn("GX_SetZCompLoc(GX_FALSE)", base)
        self.assertIn("GX_BL_SRCALPHA, GX_BL_INVSRCALPHA", base)
        self.assertIn("GX_TEXMTX1", base)
        self.assertNotIn("GX_TEXMTX0", base)   # the lightmap matrix of the environment
        self.assertIn("GX_CA_TEXA", base)
        draw = function_body(self.gx, "unsigned long wii_gx_models_draw(void)")
        self.assertLess(draw.index("fence_wait();"), draw.index("textures_update();"))
        self.assertLess(draw.index("textures_update();"), draw.index("models.proc(pass);"))
        self.assertIn("GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0)", draw)
        self.assertIn("models.fenced_sequence = models.sequence;", draw)
        fence = function_body(self.gx, "static void fence_wait(void)")
        self.assertIn("wii_model_textures_gpu_done(&models.textures.cache, models.fenced_sequence);", fence)

    def test_reserve_is_bounded_separate_and_released_after_gx(self):
        self.assertNotIn("gx.cache", function_body(self.gx, "void wii_gx_model_textures_bind("))
        begin = function_body(self.render_gx, "int wii_gx_map_begin(")
        self.assertLess(begin.index("wii_gx_models_bind(gx.model_skin"), begin.index("wii_gx_model_textures_bytes()"))
        end = function_body(self.render_gx, "void wii_gx_map_end(void)")
        self.assertLess(end.index("wii_gx_models_unbind();"),
                        end.index("store_give(gx.model_textures, gx.model_textures_taken);"))
        unbind = function_body(self.gx, "static void textures_unbind(void)")
        self.assertLess(unbind.index("fence_wait();"), unbind.index("wii_model_textures_reset(c);"))
        self.assertIn("textures_unbind();", function_body(self.gx, "void wii_gx_models_unbind(void)"))

    def test_units_are_listed(self):
        self.assertIn(Path("port/wii/render/render_model_material.c"), engine_build.RENDER_GX)
        self.assertIn(Path("port/wii/render/render_model_material.h"), engine_build.RENDER_HEADERS)


if __name__ == "__main__":
    unittest.main()
