"""Focused tests of the Wii rasterizer's first-person weapon (HWI-084).

- The weapon's GX projection (port/wii/render/render_first_person_projection.c),
  built with the host compiler under the Wii build's strict flags and called
  through ctypes: x and y are the frame's, the first-person near plane is
  depth 0 and its far plane depth `slice`, depth grows with distance, the
  world (its own near plane) lies behind the whole slice but within a hair
  of that plane, and the weapon keeps depth precision in 24 bits. Skipped
  when no host C compiler is installed.
- The state boundary of the engine seam (source/interface/first_person_weapons.c
  first_person_weapon_render_pose): the only game state it writes is the view
  model's node_orientations, under store_orientations; it calls none of the
  render update's side effects (visibility, effects, sounds, the forget path,
  the lighting cache, local random numbers). The renderer stores a pose on
  drawn frames only (pose checks and diagnostic cameras store nothing).
- The build lists the new units.
"""
from pathlib import Path
import ctypes
import math
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RENDER = ROOT / "port" / "wii" / "render"
FIRST_PERSON_WEAPONS = ROOT / "source" / "interface" / "first_person_weapons.c"

sys.path.insert(0, str(ROOT))
from tools.wii import engine_build  # noqa: E402

Matrix = ctypes.c_float * 4 * 4
SLICE = 1.0 / 32.0                     # WII_GX_FIRST_PERSON_DEPTH_SLICE
NEAR, FAR = 0.01171875, 1024.0         # rasterizer_global_defaults' first-person clip distances
WORLD_NEAR = 0.0625                    # and the world's near clip distance


def compiler():
    for name in ("clang", "gcc", "cc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def gx_perspective(near, far, x_scale=1.2, y_scale=1.6, x_centre=0.01, y_centre=-0.02):
    """the frame's projection as render_engine_visibility builds it"""
    m = Matrix()
    m[0][0], m[0][2] = x_scale, x_centre
    m[1][1], m[1][2] = y_scale, y_centre
    m[2][2], m[2][3] = -near / (far - near), -(far * near) / (far - near)
    m[3][2] = -1.0
    return m


def depth(m, z_view):
    """GX's depth (0 near .. 1 far) of a view-space point on the axis at z_view (< 0)"""
    z = m[2][2] * z_view + m[2][3]
    w = m[3][2] * z_view + m[3][3]
    return z / w + 1.0


@unittest.skipUnless(compiler(), "no host C compiler")
class FirstPersonProjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        suffix = ".dll" if sys.platform == "win32" else ".so"
        library = Path(cls.directory.name) / ("first_person_projection" + suffix)
        command = [compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-shared", "-fPIC",
                   f"-I{RENDER}", str(RENDER / "render_first_person_projection.c"), "-o", str(library)]
        if sys.platform != "win32":
            command.append("-lm")
        subprocess.run(command, check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.wii_gx_first_person_projection.restype = ctypes.c_int
        cls.lib.wii_gx_first_person_projection.argtypes = [ctypes.POINTER(Matrix), ctypes.c_float, ctypes.c_float,
                                                            ctypes.c_float, ctypes.POINTER(Matrix)]

    @classmethod
    def tearDownClass(cls):
        cls.lib = None
        try:
            cls.directory.cleanup()
        except OSError:
            pass  # (Windows keeps a loaded library's file)

    def project(self, frame, near=NEAR, far=FAR, slice_=SLICE):
        out = Matrix()
        ok = self.lib.wii_gx_first_person_projection(ctypes.byref(frame), near, far, slice_, ctypes.byref(out))
        return ok, out

    def test_unusable_distances_and_slices_are_refused(self):
        frame = gx_perspective(WORLD_NEAR, FAR)
        for near, far, slice_ in ((0.0, FAR, SLICE), (-1.0, FAR, SLICE), (1.0, 1.0, SLICE), (2.0, 1.0, SLICE),
                                  (NEAR, FAR, 0.0), (NEAR, FAR, 1.5), (math.nan, FAR, SLICE),
                                  (NEAR, math.inf, SLICE), (NEAR, FAR, math.nan)):
            ok, _ = self.project(frame, near, far, slice_)
            self.assertEqual(ok, 0, (near, far, slice_))

    def test_x_and_y_are_the_frames(self):
        frame = gx_perspective(WORLD_NEAR, FAR)
        ok, out = self.project(frame)
        self.assertEqual(ok, 1)
        for row in (0, 1):
            self.assertEqual(list(out[row]), list(frame[row]))
        self.assertEqual(list(out[3]), [0.0, 0.0, -1.0, 0.0])

    def test_near_plane_is_depth_zero_and_far_plane_the_slice(self):
        _, out = self.project(gx_perspective(WORLD_NEAR, FAR))
        self.assertAlmostEqual(depth(out, -NEAR), 0.0, places=6)
        self.assertAlmostEqual(depth(out, -FAR), SLICE, places=6)
        previous = -1.0
        for distance in (NEAR, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 10.0, FAR):
            d = depth(out, -distance)
            self.assertGreater(d, previous)
            self.assertLessEqual(d, SLICE + 1e-6)
            previous = d

    def test_a_whole_slice_is_rasterizer_set_frustum_z(self):
        # slice 1: the first-person clip distances' own perspective
        _, out = self.project(gx_perspective(WORLD_NEAR, FAR), slice_=1.0)
        reference = gx_perspective(NEAR, FAR)
        self.assertAlmostEqual(out[2][2], reference[2][2], places=6)
        self.assertAlmostEqual(out[2][3], reference[2][3], places=5)

    def test_the_world_is_behind_the_weapon_but_at_its_near_plane(self):
        frame = gx_perspective(WORLD_NEAR, FAR)
        _, out = self.project(frame)
        weapon_far = depth(out, -FAR)
        # the world's depth is below the slice only this close to its own near plane
        threshold = WORLD_NEAR / (1.0 - SLICE * (FAR - WORLD_NEAR) / FAR)
        self.assertLess(threshold, WORLD_NEAR * 1.04)
        self.assertGreaterEqual(depth(frame, -threshold * 1.0001), weapon_far)
        for distance in (0.07, 0.1, 0.5, 1.0, 100.0):
            self.assertGreater(depth(frame, -distance), depth(out, -distance))

    def test_the_weapon_keeps_depth_precision(self):
        _, out = self.project(gx_perspective(WORLD_NEAR, FAR))
        # a hundredth of a world unit apart at a gun's distances, in 24 bits
        for distance in (0.05, 0.1, 0.2, 0.4):
            step = (depth(out, -(distance + 0.01)) - depth(out, -distance)) * (1 << 24)
            self.assertGreater(step, 8.0, distance)


def function_body(text, signature):
    """the body of the definition (not a prototype) that starts with signature"""
    match = re.search(re.escape(signature) + r"[^;{]*\)\n\{", text)
    if not match:
        raise AssertionError(f"no definition of {signature!r}")
    open_brace = match.end() - 1
    end = text.index("\n}\n", open_brace)
    return text[open_brace:end + 2]


class FirstPersonStateBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = FIRST_PERSON_WEAPONS.read_text(encoding="utf-8")
        cls.pose = function_body(cls.source, "short first_person_weapon_render_pose(\n")
        cls.orientations = function_body(cls.source, "static void first_person_weapon_build_node_orientations(\n")

    def test_only_the_orientations_are_written_and_only_when_stored(self):
        writes = re.findall(r"first_person_weapon->(\w+)(?:\[[^\]]*\])?\s*=(?!=)", self.pose)
        self.assertEqual(writes, [])
        copies = re.findall(r"csmemcpy\(\s*([^,]+),", self.pose)
        state_copies = [target.strip() for target in copies if "first_person_weapon->" in target]
        self.assertEqual(state_copies, ["first_person_weapon->node_orientations"])
        store = self.pose.index("if (store_orientations)")
        self.assertGreater(self.pose.index("csmemcpy(\n\t\t\tfirst_person_weapon->node_orientations"), store)

    def test_no_side_effects_of_the_render_update(self):
        for name in ("first_person_weapon_set_visibility", "first_person_weapon_forget_weapon", "effects_",
                     "particles_", "sound_", "object_get_cached_render_lighting", "real_local_random", "error(",
                     "first_person_weapon_build_node_matrices"):
            self.assertNotIn(name, self.pose, name)
        for name in ("first_person_weapon->node_orientations", "first_person_weapon->node_matrices",
                     "first_person_weapon->rendered", "first_person_weapon->visible", "sound_", "effects_"):
            self.assertNotIn(name, self.orientations, name)

    def test_the_render_update_still_builds_through_the_shared_orientations(self):
        build = function_body(self.source, "static void first_person_weapon_build_node_matrices(\n\tshort")
        self.assertIn("first_person_weapon_build_node_orientations(", build)
        self.assertIn("first_person_weapon->node_orientations);", build)

    def test_drawn_frames_store_and_checks_do_not(self):
        text = (RENDER / "render_engine.c").read_text(encoding="utf-8")
        frame = function_body(text, "static void render_engine_frame(\n")
        capture = function_body(text, "static int render_engine_capture(\n")
        boot = function_body(text, "int wii_render_environment_draw(\n")
        self.assertIn("wii_render_first_person_collect(&render_engine.view, TRUE);", frame)
        self.assertIn("wii_render_first_person_collect(&render_engine.view, TRUE);", boot)
        self.assertIn("wii_render_first_person_collect(&render_engine.view, FALSE);", capture)
        self.assertNotIn("TRUE);", capture[capture.index("wii_render_first_person_collect"):][:80])
        self.assertIn("wii_render_first_person_forget();", capture)


class FirstPersonBuildTests(unittest.TestCase):
    def test_units_are_listed(self):
        self.assertIn(Path("port/wii/render/render_first_person_engine.c"), engine_build.RENDER_ENGINE)
        self.assertIn(Path("port/wii/render/render_first_person_projection.c"), engine_build.RENDER_GX)
        self.assertIn(Path("port/wii/render/render_models_engine.h"), engine_build.RENDER_HEADERS)
        for path in (*engine_build.RENDER_ENGINE, *engine_build.RENDER_GX, *engine_build.RENDER_HEADERS):
            self.assertTrue((ROOT / path).is_file(), path)


if __name__ == "__main__":
    unittest.main()
