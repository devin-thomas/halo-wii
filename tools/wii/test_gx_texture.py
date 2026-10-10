"""Authored-fixture tests for Xbox bitmap decoding and GX texture encoding."""
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_fixtures as fx  # noqa: E402
import gx_texture as gt  # noqa: E402

CHECKOUT = Path(__file__).resolve().parents[2]


# Python transliterations of port/wii/gx_materials/main.c, the layout that
# Dolphin verified (texel_offset, convert_dxt1_block, dxt1_to_cmpr mode 0).
def gx_materials_texel_offset(fmt, w, x, y):
    tw, th, tb = {gt.GX_I8: (8, 4, 32), gt.GX_RGB565: (4, 4, 32), gt.GX_IA8: (4, 4, 32),
                  gt.GX_RGB5A3: (4, 4, 32)}[fmt]
    bpp = 1 if fmt == gt.GX_I8 else 2
    return ((y // th) * (w // tw) + x // tw) * tb + ((y % th) * tw + x % tw) * bpp


def gx_materials_dxt1_to_cmpr(src, w, h):
    def block(s):
        d = bytearray([s[1], s[0], s[3], s[2]])
        for row in range(4):
            v = s[4 + row]
            d.append(((v & 3) << 6) | (((v >> 2) & 3) << 4) | (((v >> 4) & 3) << 2) | ((v >> 6) & 3))
        return bytes(d)
    out = bytearray()
    blocks_x = w // 4
    for ty in range(h // 8):
        for tx in range(w // 8):
            for sub in range(4):
                bx, by = tx * 2 + (sub & 1), ty * 2 + (sub >> 1)
                i = (by * blocks_x + bx) * 8
                out += block(src[i:i + 8])
    return bytes(out)


class SourceLayoutTests(unittest.TestCase):
    def test_morton_order_square_and_rectangular(self):
        level = {"w": 4, "h": 4, "d": 1}
        got = [gt.source_texel_index(level, x, y, 0, True) for y in range(2) for x in range(4)]
        self.assertEqual(got, [0, 1, 4, 5, 2, 3, 6, 7])
        wide = {"w": 8, "h": 2, "d": 1}
        self.assertEqual([gt.source_texel_index(wide, x, 1, 0, True) for x in range(8)],
                         [2, 3, 6, 7, 10, 11, 14, 15])
        volume = {"w": 2, "h": 2, "d": 2}
        self.assertEqual(gt.source_texel_index(volume, 0, 0, 1, True), 4)
        self.assertEqual(gt.source_texel_index(volume, 1, 1, 1, True), 7)

    def test_compressed_chain_stops_at_four_by_four_and_rounds(self):
        layout = gt.describe(256, 64, 1, 0, gt.FORMAT_IDS["dxt1"], gt.FLAG_POW2 | gt.FLAG_COMPRESSED, 8)
        self.assertEqual(gt.describe(4, 4, 1, 0, gt.FORMAT_IDS["dxt1"], gt.FLAG_POW2 | gt.FLAG_COMPRESSED, 2)
                         ["levels"][-1]["w"], 4)
        self.assertEqual(len(layout["levels"]), 7)
        last = layout["levels"][-1]
        self.assertEqual((last["w"], last["h"], last["stored_w"], last["stored_h"], last["bytes"]), (4, 1, 4, 4, 8))
        self.assertEqual(layout["levels"][5]["stored_h"], 4)

    def test_cube_faces_are_padded_to_128_bytes(self):
        layout = gt.describe(4, 4, 1, 2, gt.FORMAT_IDS["a8"], gt.FLAG_POW2 | gt.FLAG_SWIZZLED, 2)
        self.assertEqual(sum(level["bytes"] for level in layout["levels"]), 21)
        self.assertEqual((layout["face_bytes"], layout["bytes"]), (128, 768))

    def test_linear_rows_are_64_byte_aligned(self):
        layout = gt.describe(20, 3, 1, 0, gt.FORMAT_IDS["a8r8g8b8"], gt.FLAG_LINEAR, 0)
        self.assertEqual(gt.describe(32, 4, 1, 0, gt.FORMAT_IDS["a8"], gt.FLAG_LINEAR | gt.FLAG_POW2, 2)
                         ["levels"][0]["pitch"], 64)
        self.assertEqual(layout["levels"][0]["pitch"], 128)
        self.assertEqual(layout["bytes"], 128 * 3 + (-(128 * 3) & 127))

    def test_invalid_descriptions_reject(self):
        f = gt.FORMAT_IDS
        cases = [
            (4, 4, 1, 0, 4, 0, 0),                                   # unused format slot
            (4, 4, 1, 3, f["a8"], 0, 0),                             # unknown type
            (0, 4, 1, 0, f["a8"], 0, 0),                             # zero width
            (8192, 4, 1, 0, f["a8"], 0, 0),                          # above bound
            (4, 4, 2, 0, f["a8"], 0, 0),                             # depth on 2D
            (8, 4, 1, 2, f["a8"], 0, 0),                             # non-square cube
            (4, 4, 1, 0, f["dxt1"], 0, 0),                           # compressed flag missing
            (4, 4, 1, 0, f["a8"], gt.FLAG_COMPRESSED, 0),            # flag on uncompressed
            (4, 4, 1, 0, f["dxt1"], gt.FLAG_COMPRESSED | gt.FLAG_LINEAR, 0),
            (4, 4, 1, 0, f["a8"], gt.FLAG_POW2 | gt.FLAG_SWIZZLED, 3),  # too many mips
            (6, 4, 1, 0, f["a8"], gt.FLAG_POW2 | gt.FLAG_SWIZZLED, 1),  # pow2 flag on 6
            (6, 4, 1, 0, f["a8"], gt.FLAG_SWIZZLED, 0),              # swizzled needs power of two
            (4, 4, 1, 0, f["a8"], gt.FLAG_POW2, 0),                  # swizzled flag missing
            (4, 4, 1, 0, f["dxt1"], gt.FLAG_POW2 | gt.FLAG_COMPRESSED | gt.FLAG_SWIZZLED, 0),
            (4, 4, 1, 0, f["a8"], gt.FLAG_POW2 | gt.FLAG_SWIZZLED, -1),
        ]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(gt.TextureError):
                gt.describe(*case)

    def test_palette_matches_engine_source(self):
        text = (CHECKOUT / "source/bitmaps/bitmaps.c").read_text(encoding="latin-1")
        body = text[text.index("pixel32 global_vector_palette["):]
        body = body[body.index("{") + 1:body.index("};")]
        values = tuple(int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{8}", body))
        self.assertEqual(values, gt.GLOBAL_VECTOR_PALETTE)


class ReferenceDecodeTests(unittest.TestCase):
    def test_dxt1_four_and_three_colour_modes(self):
        block = struct.pack("<HHI", 0xF800, 0x001F, 0b11100100)  # red, blue; indices 0,1,2,3 in row 0
        texels = gt.decode_dxt_block("dxt1", block)
        self.assertEqual(texels[:4], [(255, 0, 0, 255), (0, 0, 255, 255), (170, 0, 85, 255), (85, 0, 170, 255)])
        three = struct.pack("<HHI", 0x001F, 0xF800, 0b11100100)
        texels = gt.decode_dxt_block("dxt1", three)
        self.assertEqual(texels[2:4], [(127, 0, 127, 255), (0, 0, 0, 0)])

    def test_dxt3_and_dxt5_alpha(self):
        dxt3 = struct.pack("<Q", 0xFEDCBA9876543210) + struct.pack("<HHI", 0x07E0, 0x07E0, 0)
        self.assertEqual([t[3] for t in gt.decode_dxt_block("dxt3", dxt3)], [17 * i for i in range(16)])
        # c0 == c1 in a DXT3 colour block still decodes in four-colour mode (no transparency)
        self.assertEqual(gt.decode_dxt_block("dxt3", dxt3)[0][:3], (0, 255, 0))
        bits = sum(i % 8 << (3 * i) for i in range(16))
        dxt5 = bytes([200, 100]) + bits.to_bytes(6, "little") + struct.pack("<HHI", 0, 0, 0)
        self.assertEqual([t[3] for t in gt.decode_dxt_block("dxt5", dxt5)][:8],
                         [200, 100, 185, 171, 157, 142, 128, 114])
        dxt5b = bytes([100, 200]) + bits.to_bytes(6, "little") + bytes(8)
        self.assertEqual([t[3] for t in gt.decode_dxt_block("dxt5", dxt5b)][:8], [100, 200, 120, 140, 160, 180, 0, 255])


class GxEncodingTests(unittest.TestCase):
    def convert_and_verify(self, fmt, w, h, d=1, kind="2d", mips=0, linear=False):
        fields, pixels, layout = fx.source_pixels(fmt, w, h, d, kind, mips, seed=w + h + mips, linear=linear)
        header, data, images, layout = gt.convert(fields, pixels)
        result = gt.verify(fields, pixels, data, images, layout)
        self.assertEqual(result["texels_checked"],
                         layout["faces"] * sum(lv["w"] * lv["h"] * lv["d"] for lv in layout["levels"]))
        return header, data, images, result

    def test_every_format_round_trips_through_the_gx_decoder(self):
        exact = {"a8", "y8", "ay8", "a8y8", "r5g6b5", "x8r8g8b8", "a8r8g8b8", "dxt3", "dxt5", "p8_bump"}
        for fmt in gt.FORMAT_IDS:
            with self.subTest(fmt=fmt):
                header, _, _, result = self.convert_and_verify(fmt, 16, 8, mips=2)
                self.assertEqual(header["gx_format"], gt.gx_target(fmt))
                if fmt in exact:
                    self.assertEqual(result["texels_exact"], result["texels_checked"])
                    self.assertEqual(result["max_channel_error"], [0, 0, 0, 0])

    def test_lossy_targets_stay_within_documented_bounds(self):
        _, _, _, result = self.convert_and_verify("a4r4g4b4", 32, 32, mips=1)
        self.assertLessEqual(result["max_channel_error"][3], 17)
        self.assertEqual(result["max_channel_error"][:3], [0, 0, 0])
        _, _, _, result = self.convert_and_verify("dxt1", 32, 32, mips=3)
        self.assertLessEqual(max(result["max_channel_error"]), 11)
        self.assertLess(result["texels_exact"], result["texels_checked"])  # interpolation does differ

    def test_cube_volume_linear_npot_and_small_mips(self):
        header, _, images, _ = self.convert_and_verify("r5g6b5", 8, 8, kind="cube", mips=3)
        self.assertEqual((header["faces"], header["levels"], len(images)), (6, 4, 24))
        header, _, images, _ = self.convert_and_verify("a8r8g8b8", 4, 4, d=4, kind="3d", mips=2)
        self.assertEqual([i["slice"] for i in images], [0, 1, 2, 3, 0, 1, 0])
        self.convert_and_verify("x8r8g8b8", 20, 6, linear=True)
        self.convert_and_verify("a8y8", 12, 5, linear=True)
        self.convert_and_verify("dxt5", 8, 8, mips=3)
        self.convert_and_verify("dxt1", 8, 2, mips=0)

    def test_tile_order_matches_gx_materials(self):
        for fmt, source in ((gt.GX_I8, "ay8"), (gt.GX_RGB565, "r5g6b5"), (gt.GX_IA8, "a8y8")):
            fields, pixels, layout = fx.source_pixels(source, 16, 8, seed=3)
            _, data, _, _ = gt.convert(fields, pixels)
            bpp = 1 if fmt == gt.GX_I8 else 2
            for y in range(8):
                for x in range(16):
                    src = gt.source_texel_index(layout["levels"][0], x, y, 0, True) * bpp
                    dst = gx_materials_texel_offset(fmt, 16, x, y)
                    expected = pixels[src:src + bpp] if bpp == 1 else pixels[src:src + 2][::-1]
                    self.assertEqual(data[dst:dst + bpp], expected)

    def test_rgba8_split_tiles_match_gx_materials(self):
        fields, pixels, layout = fx.source_pixels("a8r8g8b8", 8, 8, seed=4)
        _, data, _, _ = gt.convert(fields, pixels)
        for y in range(8):
            for x in range(8):
                s = gt.source_texel_index(layout["levels"][0], x, y, 0, True) * 4
                b, g, r, a = pixels[s:s + 4]
                base, within = ((y // 4) * 2 + x // 4) * 64, (y % 4) * 4 + x % 4
                self.assertEqual((data[base + 2 * within], data[base + 2 * within + 1],
                                  data[base + 32 + 2 * within], data[base + 33 + 2 * within]), (a, r, g, b))

    def test_cmpr_matches_gx_materials_converter(self):
        fields, pixels, layout = fx.source_pixels("dxt1", 16, 16, seed=9)
        _, data, _, _ = gt.convert(fields, pixels)
        self.assertEqual(data, gx_materials_dxt1_to_cmpr(pixels, 16, 16))
        self.assertNotEqual(data, pixels)  # the raw DXT1 control is different

    def test_short_pixels_and_bad_containers_reject(self):
        fields, pixels, _ = fx.source_pixels("a8", 8, 8)
        with self.assertRaises(gt.TextureError):
            gt.convert(fields, pixels[:-1])
        header, data, _, _ = gt.convert(fields, pixels)
        blob = gt.pack_hwt(header, data)
        parsed, body = gt.unpack_hwt(blob)
        self.assertEqual((parsed["width"], parsed["gx_format"], bytes(body)), (8, gt.GX_IA8, data))
        for broken in (blob[:40], b"XXXX" + blob[4:], blob + b"\0", blob[:-1]):
            with self.subTest(size=len(broken)), self.assertRaises(gt.TextureError):
                gt.unpack_hwt(broken)

    def test_conversion_is_deterministic(self):
        fields, pixels, _ = fx.source_pixels("dxt3", 32, 16, mipmap_count=3, seed=11)
        first = gt.convert(fields, pixels)[1]
        self.assertEqual(first, gt.convert(fields, bytes(pixels))[1])


if __name__ == "__main__":
    unittest.main()
