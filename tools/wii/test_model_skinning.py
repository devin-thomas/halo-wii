"""Host tests of the renderer's compressed model vertex profile (HWI-082).

port/wii/content/hwm_model.c's hwm_skin_decode_le and hwm_skin_apply, built
with the host compiler under the Wii build's strict flags and called through
ctypes on authored vertices: node bytes are the model's node times 3, the
weight is NORMSHORT1 (s16 / 32767) with the second node's weight 1 minus it,
a full weight leaves the second node byte unread, and anything outside the
profile is refused by reason, never mapped to another node. Skipped when no
host C compiler is installed.
"""
from pathlib import Path
import ctypes
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

CONTENT = Path(__file__).resolve().parents[2] / "port" / "wii" / "content"
SOURCES = ("hwm_model.c", "content_common.c", "content_sections.c")
OK, NODE_STRIDE, NODE_RANGE, WEIGHT_RANGE, POSITION = range(5)


def compiler():
    for name in ("clang", "gcc", "cc"):
        path = shutil.which(name)
        if path:
            return path
    return None


class SkinVertex(ctypes.Structure):
    _fields_ = [("position", ctypes.c_float * 3), ("normal", ctypes.c_float * 3), ("node", ctypes.c_uint8 * 2),
                ("weight", ctypes.c_float * 2)]


Palette = ctypes.c_float * 4 * 3 * 64


def vertex(position=(1.0, 2.0, 3.0), normal=0, nodes=(0, 0), weight=32767):
    """one little-endian Xbox compressed model vertex"""
    return struct.pack("<3f3I2h2Bh", *position, normal, 0, 0, 0, 0, nodes[0], nodes[1], weight)


def pack_normal(i, j, k):
    """11:11:10 as rasterizer_geometry.c packs it (two's complement fields)"""
    return (i & 0x7FF) | (j & 0x7FF) << 11 | (k & 0x3FF) << 22


def transform(m, p, w=1.0):
    return [m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + w * m[r][3] for r in range(3)]


@unittest.skipUnless(compiler(), "no host C compiler")
class SkinProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        suffix = ".dll" if sys.platform == "win32" else ".so"
        library = Path(cls.directory.name) / ("hwm_skin" + suffix)
        command = [compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-shared", "-fPIC",
                   *[str(CONTENT / name) for name in SOURCES], "-o", str(library)]
        if sys.platform != "win32":
            command.append("-lm")
        subprocess.run(command, check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.hwm_skin_decode_le.restype = ctypes.c_int
        cls.lib.hwm_skin_decode_le.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.POINTER(SkinVertex)]
        cls.lib.hwm_skin_apply.restype = None
        cls.lib.hwm_skin_apply.argtypes = [ctypes.POINTER(SkinVertex), ctypes.POINTER(Palette),
                                           ctypes.POINTER(ctypes.c_float * 3), ctypes.POINTER(ctypes.c_float * 3)]
        cls.lib.hwm_skin_result_name.restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.lib = None
        try:
            cls.directory.cleanup()
        except OSError:
            pass  # (Windows keeps a loaded library's file)

    def decode(self, data, node_count):
        out = SkinVertex()
        return self.lib.hwm_skin_decode_le(data, node_count, ctypes.byref(out)), out

    def apply(self, decoded, palette):
        position, normal = (ctypes.c_float * 3)(), (ctypes.c_float * 3)()
        self.lib.hwm_skin_apply(ctypes.byref(decoded), ctypes.byref(palette), ctypes.byref(position),
                                ctypes.byref(normal))
        return list(position), list(normal)

    def palette(self, matrices):
        palette = Palette()
        for node, rows in matrices.items():
            for r in range(3):
                for c in range(4):
                    palette[node][r][c] = rows[r][c]
        return palette

    def test_full_weight_reads_one_node_and_not_the_second_byte(self):
        # retail maps leave arbitrary second node bytes under a full weight
        for garbage in (1, 100, 253):
            result, out = self.decode(vertex(nodes=(9, garbage), weight=32767), 4)
            self.assertEqual(result, OK)
            self.assertEqual(list(out.node), [3, 3])
            self.assertEqual(list(out.weight), [1.0, 0.0])
        scale_translate = [[2, 0, 0, 1], [0, 2, 0, 2], [0, 0, 2, 3]]
        position, _ = self.apply(out, self.palette({3: scale_translate}))
        self.assertEqual(position, [3.0, 6.0, 9.0])

    def test_partial_weight_is_normshort_and_blends_two_nodes(self):
        result, out = self.decode(vertex(position=(1.0, 0.0, 0.0), nodes=(3, 6), weight=16384), 3)
        self.assertEqual(result, OK)
        self.assertEqual(list(out.node), [1, 2])
        w0 = struct.unpack("<f", struct.pack("<f", 16384 * (1.0 / 32767.0)))[0]
        self.assertAlmostEqual(out.weight[0], w0, places=7)
        self.assertAlmostEqual(out.weight[0] + out.weight[1], 1.0, places=7)
        a = [[1, 0, 0, 10], [0, 1, 0, 0], [0, 0, 1, 0]]
        b = [[0, -1, 0, 0], [1, 0, 0, 20], [0, 0, 1, 0]]
        position, _ = self.apply(out, self.palette({1: a, 2: b}))
        expected = [out.weight[0] * x + out.weight[1] * y
                    for x, y in zip(transform(a, (1, 0, 0)), transform(b, (1, 0, 0)))]
        for got, want in zip(position, expected):
            self.assertAlmostEqual(got, want, places=4)

    def test_weight_is_not_the_byte_over_255_decode(self):
        # (byte)32767 / 255 would be 1.0 as well; 1560, retail's smallest, would be 0.094
        result, out = self.decode(vertex(nodes=(0, 3), weight=1560), 2)
        self.assertEqual(result, OK)
        self.assertAlmostEqual(out.weight[0], 1560 / 32767.0, places=6)

    def test_refusals_by_reason(self):
        cases = [(vertex(nodes=(4, 0)), 4, NODE_STRIDE),            # first node byte not a triple
                 (vertex(nodes=(12, 0)), 4, NODE_RANGE),            # node 4 of a model of 4
                 (vertex(nodes=(3, 100), weight=20000), 4, NODE_STRIDE),  # a consumed second byte
                 (vertex(nodes=(3, 12), weight=20000), 4, NODE_RANGE),
                 (vertex(nodes=(3, 3), weight=-1), 4, WEIGHT_RANGE),
                 (vertex(position=(float("nan"), 0.0, 0.0)), 4, POSITION),
                 (vertex(position=(float("inf"), 0.0, 0.0)), 4, POSITION)]
        for data, nodes, want in cases:
            result, _ = self.decode(data, nodes)
            self.assertEqual(result, want, self.lib.hwm_skin_result_name(result))

    def test_normal_through_the_node_rotation(self):
        data = vertex(nodes=(0, 0))[:12] + struct.pack("<I", pack_normal(1023, 0, 0)) + vertex()[16:]
        result, out = self.decode(data, 1)
        self.assertEqual(result, OK)
        self.assertAlmostEqual(out.normal[0], 2047 / 2047.0, places=6)
        rotate = [[0, -3, 0, 5], [3, 0, 0, 5], [0, 0, 3, 5]]   # scale 3, a quarter turn about z
        _, normal = self.apply(out, self.palette({0: rotate}))
        length = math.sqrt(sum(c * c for c in normal))
        self.assertAlmostEqual(length, 1.0, places=5)
        self.assertGreater(normal[1], 0.99)


if __name__ == "__main__":
    unittest.main()
