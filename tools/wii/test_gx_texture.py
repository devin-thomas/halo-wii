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


SPLIT = gt.PROFILE_GX_SPLITALPHA


def dxt_block(name, colour, alpha):
    """A 16-byte DXT3/DXT5 block: alpha (8 bytes) then colour (c0, c1, indices)."""
    return bytes(alpha) + struct.pack("<HHI", *colour)


def one_block_texture(name, block, size=4):
    """A size x size 2D texture (no mips) whose blocks all equal block."""
    fields = {"width": size, "height": size, "depth": 1, "type": 0, "format": gt.FORMAT_IDS[name],
              "flags": gt.FLAG_POW2 | gt.FLAG_COMPRESSED, "mipmap_count": 0}
    layout = gt.describe(size, size, 1, 0, fields["format"], fields["flags"], 0)
    pixels = block * ((size // 4) ** 2)
    return fields, pixels + bytes(layout["bytes"] - len(pixels))   # the face pads to 128 bytes


def split_texels(header, data, images, image=0):
    """Row-major RGBA of one converted split-alpha image, by the independent decoder."""
    alpha = header["alpha"]
    colour_bytes = header["colour_bytes"]
    info = images[image]
    colour = data[info["offset"]:info["offset"] + info["bytes"]]
    out = []
    for y in range(info["height"]):
        for x in range(info["width"]):
            rgba = gt.gx_decode_texel(gt.GX_CMPR, colour, info["width"], x, y)
            if alpha["kind"] == "plane":
                a = alpha["images"][image]
                plane = data[colour_bytes + a["offset"]:colour_bytes + a["offset"] + a["bytes"]]
                rgba = rgba[:3] + (gt.gx_decode_texel(alpha["gx_format"], plane, info["width"], x, y)[3],)
            out.append(rgba)
    return out


class SplitAlphaColourTests(unittest.TestCase):
    """gx-splitalpha-v1 colour: every DXT colour block case on authored blocks."""

    def test_interpolation_rule_is_bounded_exhaustively(self):
        # every 5- and 6-bit endpoint pair: GX's 5/8+3/8 against DXT's 2/3+1/3
        self.assertEqual(gt.interpolation_error_table(), {5: (11, 0), 6: (11, 0)})

    def check_block(self, colour, expected_case):
        indices = colour[2]
        for name in ("dxt3", "dxt5"):
            block = dxt_block(name, colour, b"\xff" * 8 if name == "dxt3" else bytes([255, 255]) + bytes(6))
            fixed, case = gt.four_colour_block(block[8:])
            self.assertEqual(case, expected_case)
            fields, pixels = one_block_texture(name, block, 8)
            header, data, images, layout = gt.convert(fields, pixels, SPLIT)
            self.assertEqual(header["alpha"]["colour_blocks"][expected_case], 4)
            # the CMPR block is the fixed DXT1-form block, endpoints big-endian
            c0, c1, idx = struct.unpack("<HHI", fixed)
            self.assertEqual(struct.unpack_from(">HH", data, 0), (c0, c1))
            reference = gt.decode_dxt_block(name, block)
            got = split_texels(header, data, images)
            for y in range(4):
                for x in range(4):
                    ref, gx = reference[y * 4 + x], got[y * 8 + x]
                    i = (indices >> (2 * (y * 4 + x))) & 3
                    bound = gt.tolerance(name, ref, block[8:], split=True)
                    self.assertTrue(all(abs(a - b) <= t for a, b, t in zip(gx, ref, bound)), (gx, ref, bound))
                    if i < 2 or expected_case == gt.BLOCK_UNIFORM:
                        self.assertEqual(gx, ref)          # endpoints (and uniform blocks) are exact
                    self.assertEqual(gx[3], 255)
            result = gt.verify(fields, pixels, data, images, layout, alpha=header["alpha"])
            self.assertLessEqual(max(result["max_channel_error"]), 11)
        return fixed

    def test_c0_greater_than_c1_is_kept(self):
        colour = (0xF800, 0x001F, 0b11100100 * 0x01010101)   # red > blue, indices 0,1,2,3 per row
        fixed = self.check_block(colour, gt.BLOCK_KEPT)
        self.assertEqual(fixed, struct.pack("<HHI", *colour))

    def test_c0_less_than_c1_swaps_endpoints_and_indices(self):
        colour = (0x001F, 0xF800, 0b11100100 * 0x01010101)   # blue < red: DXT1 would use 3 colours
        fixed = self.check_block(colour, gt.BLOCK_SWAPPED)
        self.assertEqual(fixed, struct.pack("<HHI", 0xF800, 0x001F, 0b10110001 * 0x01010101))  # 0<->1, 2<->3
        # the DXT1 rule on the raw block would have made index 3 transparent black
        self.assertEqual(gt.decode_dxt_block("dxt1", struct.pack("<HHI", *colour))[3], (0, 0, 0, 0))

    def test_c0_equal_c1_clears_the_indices(self):
        colour = (0x7BEF, 0x7BEF, 0xFFFFFFFF)                # every index 3: transparent under DXT1/CMPR rules
        fixed = self.check_block(colour, gt.BLOCK_UNIFORM)
        self.assertEqual(fixed, struct.pack("<HHI", 0x7BEF, 0x7BEF, 0))

    def test_exhaustive_index_permutation_on_swapped_blocks(self):
        # every endpoint swap keeps the palette: DXT(c0, c1)[i] == DXT(c1, c0)[i ^ 1]
        for c0, c1 in ((0, 1), (0x0001, 0xFFFF), (0x8410, 0x841F), (0x07E0, 0xF81F)):
            a = gt._dxt_colours(struct.pack("<HH", c0, c1), True)
            b = gt._dxt_colours(struct.pack("<HH", c1, c0), True)
            self.assertEqual([a[i] for i in range(4)], [b[i ^ 1] for i in range(4)])


class SplitAlphaPlaneTests(unittest.TestCase):
    """gx-splitalpha-v1 alpha: exact planes, edge values, omission."""

    def plane_of(self, name, alpha_bytes, colour=(0xF800, 0x001F, 0)):
        fields, pixels = one_block_texture(name, dxt_block(name, colour, alpha_bytes), 8)
        header, data, images, layout = gt.convert(fields, pixels, SPLIT)
        result = gt.verify(fields, pixels, data, images, layout, alpha=header["alpha"])
        self.assertEqual((result["alpha_exact"], result["max_channel_error"][3]), (result["texels_checked"], 0))
        return header, data, images, [t[3] for t in split_texels(header, data, images)]

    def test_dxt3_every_nibble_is_exact_in_i4(self):
        alpha = struct.pack("<Q", 0xFEDCBA9876543210)          # nibbles 0..15, texel i holds i
        header, data, images, got = self.plane_of("dxt3", alpha)
        self.assertEqual((header["alpha"]["kind"], header["alpha"]["gx_format"]), ("plane", gt.GX_I4))
        self.assertEqual(got[:4] + got[8:12], [0, 17, 34, 51, 68, 85, 102, 119])
        # I4 tile: high nibble first; row 0 of the 8x8 tile is texels 0..3 of two blocks
        plane = data[header["colour_bytes"]:]
        self.assertEqual(plane[:4], bytes([0x01, 0x23, 0x01, 0x23]))
        self.assertEqual(len(plane), 32)

    def test_dxt3_extremes_and_opaque_omission(self):
        _, _, _, got = self.plane_of("dxt3", bytes(8))
        self.assertEqual(set(got), {0})
        header, data, _, got = self.plane_of("dxt3", b"\xff" * 8)
        self.assertEqual((header["alpha"]["kind"], header["alpha"]["bytes"], set(got)), ("omitted", 0, {255}))
        self.assertEqual(len(data), header["colour_bytes"])

    def test_dxt5_eight_and_six_value_modes_are_exact_in_i8(self):
        bits = sum(i % 8 << (3 * i) for i in range(16)).to_bytes(6, "little")
        header, _, _, got = self.plane_of("dxt5", bytes([200, 100]) + bits)
        self.assertEqual(header["alpha"]["gx_format"], gt.GX_I8)
        self.assertEqual(got[:4] + got[8:12], [200, 100, 185, 171, 157, 142, 128, 114])
        header, _, _, got = self.plane_of("dxt5", bytes([100, 200]) + bits)
        self.assertEqual(got[:4] + got[8:12], [100, 200, 120, 140, 160, 180, 0, 255])
        # the reference rounds down: (6 * 1 + 1 * 0) // 7 = 0 where round-to-nearest would give 1
        header, _, _, got = self.plane_of("dxt5", bytes([1, 0]) + bits)
        self.assertEqual(got[:4] + got[8:12], [1, 0, 0, 0, 0, 0, 0, 0])

    def test_dxt5_multiples_of_17_use_i4_and_constants_keep_a_plane(self):
        bits = sum(i % 8 << (3 * i) for i in range(16)).to_bytes(6, "little")
        header, _, _, got = self.plane_of("dxt5", bytes([0, 255]) + bits)
        self.assertEqual((header["alpha"]["gx_format"], sorted(set(got))), (gt.GX_I4, [0, 51, 102, 153, 204, 255]))
        header, _, _, got = self.plane_of("dxt5", bytes([128, 128]) + bytes(6))   # constant, but not 255
        self.assertEqual((header["alpha"]["kind"], header["alpha"]["gx_format"], set(got)), ("plane", gt.GX_I8, {128}))
        header, _, _, got = self.plane_of("dxt5", bytes([255, 255]) + bytes(6))
        self.assertEqual((header["alpha"]["kind"], set(got)), ("omitted", {255}))
        # a0 == a1 == 255 is the six-value mode: index 6 is 0, so the plane stays
        header, _, _, got = self.plane_of("dxt5", bytes([255, 255]) + (6).to_bytes(6, "little"))
        self.assertEqual((header["alpha"]["kind"], got[0], got[1]), ("plane", 0, 255))


class SplitAlphaLayoutTests(unittest.TestCase):
    """Mips, cube maps, volumes, dimension flags and the version 2 container."""

    def convert(self, name, w, h, d=1, kind="2d", mips=0, alpha=None, seed=3):
        fields, pixels, layout = fx.source_pixels(name, w, h, d, kind, mips, seed=seed, alpha=alpha)
        header, data, images, layout = gt.convert(fields, pixels, SPLIT)
        result = gt.verify(fields, pixels, data, images, layout, alpha=header["alpha"])
        self.assertEqual(result["texels_checked"],
                         layout["faces"] * sum(lv["w"] * lv["h"] * lv["d"] for lv in layout["levels"]))
        self.assertEqual(result["alpha_exact"], result["texels_checked"])
        self.assertLessEqual(max(result["max_channel_error"]), 11)
        return header, data, images, result

    def test_mips_cube_and_volume(self):
        header, data, images, _ = self.convert("dxt3", 64, 16, mips=6, alpha="edges")
        self.assertEqual((header["levels"], len(images), len(header["alpha"]["images"])), (5, 5, 5))  # stops at 4
        self.assertEqual([i["width"] for i in images], [64, 32, 16, 8, 4])
        header, _, images, _ = self.convert("dxt5", 16, 16, kind="cube", mips=2, alpha="edges")
        self.assertEqual((header["faces"], len(images), header["alpha"]["gx_format"]), (6, 18, gt.GX_I8))
        header, _, images, _ = self.convert("dxt5", 8, 8, d=4, kind="3d", mips=1, alpha="steps17")
        self.assertEqual(([i["slice"] for i in images], header["alpha"]["gx_format"]), ([0, 1, 2, 3, 0, 1], gt.GX_I4))
        header, _, _, _ = self.convert("dxt3", 8, 8, kind="cube", alpha="opaque")
        self.assertEqual(header["alpha"]["kind"], "omitted")

    def test_oversize_flag_and_non_power_of_two(self):
        header, _, _, _ = self.convert("dxt3", 2048, 4)
        self.assertFalse(header["gx_loadable_dimensions"])
        header, _, _, _ = self.convert("dxt5", 1024, 8)
        self.assertTrue(header["gx_loadable_dimensions"])
        for w, h in ((12, 8), (8, 6)):   # block formats need power-of-two dimensions
            with self.subTest(w=w, h=h), self.assertRaises(gt.TextureError):
                gt.describe(w, h, 1, 0, gt.FORMAT_IDS["dxt3"], gt.FLAG_COMPRESSED, 0)

    def test_sizes_against_rgba8(self):
        for name, plane, ratio in (("dxt3", gt.GX_I4, 4), ("dxt5", gt.GX_I8, 8 / 3)):
            fields, pixels, layout = fx.source_pixels(name, 64, 64, mipmap_count=4, seed=8)
            split = gt.pack_hwt(*gt.convert(fields, pixels, SPLIT)[:2])
            base = gt.pack_hwt(*gt.convert(fields, pixels)[:2])
            # every level is whole GX tiles: CMPR plus I4 is 1 byte a texel, plus I8 1.5
            self.assertEqual(len(split) - 64, sum(gt.gx_level_bytes(gt.GX_CMPR, lv["w"], lv["h"]) +
                                                  gt.gx_level_bytes(plane, lv["w"], lv["h"]) for lv in layout["levels"]))
            self.assertEqual(gt.gx_level_bytes(gt.GX_CMPR, 64, 64) + gt.gx_level_bytes(plane, 64, 64),
                             64 * 64 * 4 / ratio)   # the top level: exactly 1/4 (I4) or 3/8 (I8) of RGBA8
            self.assertLessEqual(len(split) - 64, (len(base) - 64) / ratio + 2 * 64 * len(layout["levels"]))

    def test_other_formats_are_identical_to_the_baseline(self):
        for name in ("dxt1", "a8r8g8b8", "a4r4g4b4", "p8_bump", "ay8"):
            fields, pixels, _ = fx.source_pixels(name, 16, 8, mipmap_count=2, seed=5)
            with self.subTest(name=name):
                self.assertEqual(gt.pack_hwt(*gt.convert(fields, pixels, SPLIT)[:2]),
                                 gt.pack_hwt(*gt.convert(fields, pixels)[:2]))

    def test_version_2_container_round_trip_and_rejections(self):
        fields, pixels, _ = fx.source_pixels("dxt5", 16, 16, mipmap_count=2, seed=6)
        header, data, _, _ = gt.convert(fields, pixels, SPLIT)
        blob = gt.pack_hwt(header, data)
        parsed, colour = gt.unpack_hwt(blob)
        self.assertEqual((parsed["version"], parsed["gx_format"], parsed["alpha"]["kind"]), (2, gt.GX_CMPR, "plane"))
        self.assertEqual(bytes(colour) + bytes(parsed["alpha"]["data"]), data)
        self.assertEqual(struct.unpack_from(">HHHHII", blob, 32),
                         (1, gt.HWT_ALPHA_PLANE, gt.GX_I8, 0, 64 + header["colour_bytes"], header["alpha"]["bytes"]))
        self.assertEqual(gt.unpack_hwt(gt.pack_hwt(*gt.convert(fields, pixels)[:2]))[0]["alpha"], None)  # version 1

        def edited(offset, fmt, value):
            m = bytearray(blob)
            struct.pack_into(fmt, m, offset, value)
            return bytes(m)
        for broken in (blob[:-1], blob + b"\0", edited(32, ">H", 2), edited(34, ">H", 3), edited(36, ">H", 3),
                       edited(38, ">H", 1), edited(40, ">I", 0), edited(50, ">B", 1), edited(20, ">H", 14),
                       edited(4, ">H", 3)):
            with self.subTest(size=len(broken)), self.assertRaises(gt.TextureError):
                gt.unpack_hwt(broken)
        fields, pixels, _ = fx.source_pixels("dxt3", 8, 8, alpha="opaque", seed=6)
        header, data, _, _ = gt.convert(fields, pixels, SPLIT)
        blob = gt.pack_hwt(header, data)
        self.assertEqual(struct.unpack_from(">HHHHII", blob, 32), (1, gt.HWT_ALPHA_OMITTED, 0, 0, 0, 0))
        self.assertEqual(gt.unpack_hwt(blob)[0]["alpha"]["kind"], "omitted")

    def test_split_conversion_is_deterministic(self):
        fields, pixels, _ = fx.source_pixels("dxt5", 32, 16, mipmap_count=3, seed=12, alpha="edges")
        first = gt.convert(fields, pixels, SPLIT)
        second = gt.convert(fields, bytes(pixels), SPLIT)
        self.assertEqual(gt.pack_hwt(*first[:2]), gt.pack_hwt(*second[:2]))


if __name__ == "__main__":
    unittest.main()
