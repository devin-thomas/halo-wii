"""HWI-083: the environment renderer's bounded base map residency.

The cache (port/wii/render/render_texture_cache.c) is the unit render_gx.c
links. texture_cache_check.c drives it on the host: memory limits, eviction,
visibility, duplicate references, failed and stale loads, and the GPU fence
(no byte handed out while a draw that has not completed may read it). The
rest pins render_gx.c's use of it: the safe default, the completion
boundaries and the draw path's residency check.
"""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import engine_build

ROOT = Path(__file__).resolve().parents[2]
RENDER = ROOT / "port" / "wii" / "render"
RENDER_GX = RENDER / "render_gx.c"


def compiler():
    for name in ("gcc", "cc", "clang"):
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
class CacheOnTheHost(unittest.TestCase):
    def test_pinned_lightmaps_do_not_alias_evictable_base_maps(self):
        text = RENDER_GX.read_text(encoding="utf-8")
        signature = "static int texture_find(long tag, long bitmap, int lightmap)"
        function = signature + "\n" + function_body(text, signature) + "\n}\n"
        # The same bitmap may be referenced in both roles. Evicting its base
        # must never clear the pinned lightmap's object or change its wrap mode.
        source = r'''
#include <assert.h>
#include <stddef.h>
struct texture { long tag, bitmap; int lightmap; };
static struct texture entries[] = {{7, 2, 0}, {7, 2, 1}, {8, 0, 0}};
static struct { struct texture *textures; int texture_count; } gx = {entries, 3};
''' + function + r'''
int main(void) {
    assert(texture_find(7, 2, 0) == 0);
    assert(texture_find(7, 2, 1) == 1);
    assert(texture_find(8, 0, 1) == -1);
    assert(texture_find(8, 0, 0) == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "role.c"
            exe = Path(temp) / ("role.exe" if sys.platform == "win32" else "role")
            path.write_text(source, encoding="utf-8")
            build = subprocess.run([compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o",
                                    str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)

    def test_the_production_cache_keeps_every_rule(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / ("texture_cache_check.exe" if sys.platform == "win32" else "texture_cache_check")
            build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{RENDER}",
                                    "-o", str(exe), str(ROOT / "tools" / "wii" / "texture_cache_check.c"),
                                    str(RENDER / "render_texture_cache.c")], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-4000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            match = re.fullmatch(r"ok (\d+) cases\s*", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            self.assertGreaterEqual(int(match.group(1)), 40)


class RendererUsesTheCache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = RENDER_GX.read_text(encoding="utf-8")

    def test_streaming_is_off_by_default_and_documented(self):
        defaults = function_body(self.text, "static void config_defaults(void)")
        self.assertIn("gx.texture_stream = 0;", defaults)
        self.assertIn('"texture_stream="', self.text)
        readme = (RENDER / "README.md").read_text(encoding="utf-8")
        for option in ("texture_stream=", "stream_loads=", "stream_frame_bytes=", "stream_keep_frames="):
            self.assertIn(option, readme)

    def test_the_unit_is_built_with_the_renderer(self):
        self.assertIn(Path("port/wii/render/render_texture_cache.c"), engine_build.RENDER_GX)
        self.assertIn(Path("port/wii/render/render_texture_cache.h"), engine_build.RENDER_HEADERS)

    def test_every_draw_completion_is_seen(self):
        # after the configuration's first copy, the draw path waits for the GP
        # only through draw_done, which tells the cache the sequence completed
        draw_path = self.text[self.text.index("static void present(int vsyncs)"):]
        self.assertNotIn("GX_DrawDone()", draw_path)
        self.assertIn("draw_done();", draw_path)
        done = function_body(self.text, "static void draw_done(void)")
        self.assertIn("wii_texture_cache_gpu_done", done)
        end = function_body(self.text, "void wii_gx_map_end(void)")
        self.assertLess(end.index("draw_done()"), end.index("release_textures()"))

    def test_draws_check_residency_and_fence_what_they_bind(self):
        draw = function_body(self.text, "static unsigned long draw_materials(")
        self.assertIn("++gx.gpu_submitted", draw)
        self.assertIn("wii_texture_cache_used(", draw)
        self.assertIn("base_of(m)", draw)
        self.assertNotIn("&gx.textures[m->base_texture]", draw)
        base_of = function_body(self.text, "static const struct texture *base_of(")
        self.assertIn("wii_texture_cache_resident", base_of)

    def test_streaming_runs_before_each_frames_draws(self):
        for signature in ("void wii_gx_frame(", "void wii_gx_environment_draw(", "void wii_gx_pose_capture("):
            body = function_body(self.text, signature)
            self.assertLess(body.index("stream_update("), body.index("draw_materials("), signature)

    def test_a_load_is_checked_before_it_is_published(self):
        load = function_body(self.text, "static int stream_load(")
        self.assertLess(load.index("texture_prepare("), load.index("wii_texture_cache_publish("))
        update = function_body(self.text, "static void stream_update(")
        self.assertIn("GX_InvalidateTexAll()", update)


if __name__ == "__main__":
    unittest.main()
