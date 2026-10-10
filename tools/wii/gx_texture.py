"""Xbox Halo bitmap layout decoding and deterministic GX texture encoding.

Source side (Xbox, as source/bitmaps and source/rasterizer/rasterizer_swizzle.c
describe it): a bitmap's pixels are its hardware texture. Uncompressed
non-linear levels are Morton-swizzled (x, y and z bits alternate until a
dimension runs out, as port/linux/src/xbox_textures.c decodes them), DXT
levels are plain row-major 4x4 blocks with dimensions rounded up to 4, a
linear bitmap has one level with 64-byte aligned rows, and each cube face
holds its whole mip chain padded to 128 bytes.

Target side (GX, as port/wii/gx_materials verified in Dolphin): big-endian
texels in 32-byte tiles (I8 8x4, IA8/RGB565/RGB5A3 4x4, RGBA8 4x4 split into
AR and GB halves) and CMPR, which is DXT1 with big-endian endpoints,
most-significant-first 2-bit indices and 2x2 blocks per 8x8 tile.

Every GX target in PROFILE_GX_BASELINE is chosen so the sampled RGBA equals
the reference decode of the source exactly, except where FIDELITY says
otherwise. A separate GX decoder (gx_decode_texel) reads converted bytes back
for verification; it shares no layout code with the encoder.
"""
from array import array
from functools import lru_cache
import struct
import sys

# ---- Halo bitmap enums (source/bitmaps/bitmaps.c) ---------------------------
FORMAT_NAMES = {0: "a8", 1: "y8", 2: "ay8", 3: "a8y8", 6: "r5g6b5", 8: "a1r5g5b5", 9: "a4r4g4b4",
                10: "x8r8g8b8", 11: "a8r8g8b8", 14: "dxt1", 15: "dxt3", 16: "dxt5", 17: "p8_bump"}
FORMAT_IDS = {name: fid for fid, name in FORMAT_NAMES.items()}
BITS = {"a8": 8, "y8": 8, "ay8": 8, "a8y8": 16, "r5g6b5": 16, "a1r5g5b5": 16, "a4r4g4b4": 16,
        "x8r8g8b8": 32, "a8r8g8b8": 32, "dxt1": 4, "dxt3": 8, "dxt5": 8, "p8_bump": 8}
TYPE_NAMES = {0: "2d", 1: "3d", 2: "cube"}
FLAG_POW2, FLAG_COMPRESSED, FLAG_PALETTIZED, FLAG_SWIZZLED, FLAG_LINEAR = 1, 2, 4, 8, 16
COMPRESSED = {"dxt1", "dxt3", "dxt5"}
MAX_SOURCE_DIMENSION = 4096
MAX_SOURCE_DEPTH = 512
CUBE_FACE_ALIGNMENT = 128
PITCH_ALIGNMENT = 64

# ---- GX formats (libogc gx.h numbering) ------------------------------------
GX_I8, GX_IA8, GX_RGB565, GX_RGB5A3, GX_RGBA8, GX_CMPR = 1, 3, 4, 5, 6, 14
GX_NAMES = {GX_I8: "I8", GX_IA8: "IA8", GX_RGB565: "RGB565", GX_RGB5A3: "RGB5A3", GX_RGBA8: "RGBA8", GX_CMPR: "CMPR"}
GX_TILE = {GX_I8: (8, 4, 32), GX_IA8: (4, 4, 32), GX_RGB565: (4, 4, 32), GX_RGB5A3: (4, 4, 32),
           GX_RGBA8: (4, 4, 64), GX_CMPR: (8, 8, 32)}
GX_MAX_DIMENSION = 1024

PROFILE_GX_BASELINE = {
    "name": "gx-baseline-v1",
    "targets": {"a8": "IA8", "y8": "IA8", "ay8": "I8", "a8y8": "IA8", "r5g6b5": "RGB565",
                "a1r5g5b5": "RGB5A3", "a4r4g4b4": "RGB5A3", "x8r8g8b8": "RGBA8", "a8r8g8b8": "RGBA8",
                "dxt1": "CMPR", "dxt3": "RGBA8", "dxt5": "RGBA8", "p8_bump": "RGBA8"},
    "dimensions": "preserved; levels above GX_MAX_DIMENSION are flagged, never resized",
    "mipmaps": "every level the Xbox hardware texture holds",
    "cube_maps": "six 2D chains in Xbox hardware face order (GX has no cube maps)",
    "volumes": "per-level depth slices as 2D images (GX has no 3D textures)",
}
FIDELITY = {
    "a8": "exact (IA8 intensity 255, alpha = A8)",
    "y8": "exact (IA8 alpha 255, intensity = L8)",
    "ay8": "exact (I8 samples I for all four channels, as D3D AL8)",
    "a8y8": "exact",
    "r5g6b5": "exact",
    "a1r5g5b5": "opaque texels exact; transparent texels keep alpha 0 but colour drops from 5 to 4 bits",
    "a4r4g4b4": "colour exact; alpha quantized 4 -> 3 bits to the nearest code (0 and 15 exact, max error 17/255)",
    "x8r8g8b8": "exact",
    "a8r8g8b8": "exact",
    "dxt1": ("block data lossless; GX interpolates the two middle colours at 5/8+3/8 where DXT1 uses "
             "2/3+1/3, so those texels differ by at most |c0-c1|/24+1 per channel; transparent texel "
             "colour is unspecified on GX (alpha compared only)"),
    "dxt3": "exact against the reference decode; 4x the source bytes (no GX equivalent)",
    "dxt5": "exact against the reference decode; 4x the source bytes (no GX equivalent)",
    "p8_bump": "exact expansion through the engine's global vector palette; 4x the source bytes",
}


class TextureError(ValueError):
    """A bounded texture validation failure; no input bytes in the message."""


def floor_log2(value):
    return value.bit_length() - 1 if value > 0 else -1


# ---- source layout ----------------------------------------------------------
def describe(width, height, depth, type_id, format_id, flags, mipmap_count):
    """Validate a bitmap_data and return its Xbox hardware layout.

    Returns {'faces', 'levels': [{w, h, d, stored_w, stored_h, pitch, bytes, offset}], 'face_bytes',
    'bytes'} where offsets are relative to the face start and every face
    has the same level table.
    """
    name = FORMAT_NAMES.get(format_id)
    if name is None:
        raise TextureError("unsupported bitmap format")
    if type_id not in TYPE_NAMES:
        raise TextureError("unsupported bitmap type")
    limit = MAX_SOURCE_DEPTH if type_id == 1 else MAX_SOURCE_DIMENSION
    if not (0 < width <= limit and 0 < height <= limit and 0 < depth <= limit):
        raise TextureError("bitmap dimensions outside bounds")
    if type_id != 1 and depth != 1:
        raise TextureError("non-volume bitmap with depth")
    if type_id == 2 and width != height:
        raise TextureError("cube map faces must be square")
    linear = bool(flags & FLAG_LINEAR)
    compressed = name in COMPRESSED
    if bool(flags & FLAG_COMPRESSED) != compressed:
        raise TextureError("compressed flag disagrees with format")
    if linear and (compressed or name == "p8_bump" or type_id != 0):
        raise TextureError("linear bitmap must be an uncompressed 2D texture")
    if mipmap_count < 0 or mipmap_count > floor_log2(max(width, height, depth)):
        raise TextureError("mipmap count outside the bitmap's chain")
    power_of_two = not any(v & (v - 1) for v in (width, height, depth))
    if bool(flags & FLAG_POW2) != power_of_two:
        raise TextureError("power-of-two flag disagrees with the dimensions")
    if not linear and not power_of_two:
        raise TextureError("a non-linear (swizzled or block) bitmap needs power-of-two dimensions")
    if bool(flags & FLAG_SWIZZLED) != (not linear and not compressed):
        raise TextureError("swizzled flag disagrees with the hardware layout")
    if not linear:
        if compressed:
            top = min(mipmap_count, floor_log2(max(width // 4, height // 4, depth)))
        else:
            top = min(mipmap_count, floor_log2(max(width, height, depth)))
    else:
        top = 0
    bits = BITS[name]
    levels = []
    offset = 0
    for level in range(top + 1):
        w, h, d = max(width >> level, 1), max(height >> level, 1), max(depth >> level, 1)
        sw, sh = ((w + 3) & ~3, (h + 3) & ~3) if compressed else (w, h)
        if linear:
            pitch = (sw * bits // 8 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1)
            size = pitch * sh
        else:
            pitch = sw * bits // 8
            size = sw * sh * d * bits // 8
        levels.append({"w": w, "h": h, "d": d, "stored_w": sw, "stored_h": sh, "pitch": pitch,
                       "bytes": size, "offset": offset})
        offset += size
    face_bytes = offset + (-offset & (CUBE_FACE_ALIGNMENT - 1))
    faces = 6 if type_id == 2 else 1
    return {"format": name, "type": TYPE_NAMES[type_id], "linear": linear, "faces": faces, "levels": levels,
            "face_bytes": face_bytes, "bytes": face_bytes * faces}


@lru_cache(maxsize=64)
def _swizzle_offsets(width, height, depth):
    """Per-axis Morton offsets (in texels) for an Xbox swizzled level."""
    masks = [0, 0, 0]
    bit = mask_bit = 1
    while True:
        done = True
        for axis, size in enumerate((width, height, depth)):
            if bit < size:
                masks[axis] |= mask_bit
                mask_bit <<= 1
                done = False
        bit <<= 1
        if done:
            break

    def spread(mask, value):
        result, b = 0, 1
        while value:
            if mask & b:
                if value & 1:
                    result |= b
                value >>= 1
            b <<= 1
        return result

    return tuple(tuple(spread(masks[axis], v) for v in range(size))
                 for axis, size in enumerate((width, height, depth)))


def source_texel_index(level, x, y, z, swizzled):
    """Texel index of (x, y, z) inside a stored uncompressed level."""
    if swizzled:
        xs, ys, zs = _swizzle_offsets(level["w"], level["h"], level["d"])
        return xs[x] | ys[y] | zs[z]
    return (z * level["h"] + y) * level["w"] + x


# ---- reference decode (Xbox/D3D semantics, per texel) -------------------------
def _e5(v):
    return (v << 3) | (v >> 2)


def _e6(v):
    return (v << 2) | (v >> 4)


def _rgb565(v):
    return (_e5(v >> 11), _e6((v >> 5) & 63), _e5(v & 31))


def _dxt_colours(block, four_colour_always):
    c0, c1 = struct.unpack_from("<HH", block, 0)
    a, b = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or four_colour_always:
        return [a + (255,), b + (255,), tuple((2 * p + q) // 3 for p, q in zip(a, b)) + (255,),
                tuple((p + 2 * q) // 3 for p, q in zip(a, b)) + (255,)]
    return [a + (255,), b + (255,), tuple((p + q) // 2 for p, q in zip(a, b)) + (255,), (0, 0, 0, 0)]


def _dxt5_alphas(a0, a1):
    if a0 > a1:
        return [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    return [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]


def decode_dxt_block(name, block):
    """Reference decode of one 4x4 DXT block to 16 RGBA tuples, row-major."""
    if name == "dxt1":
        colours = _dxt_colours(block, False)
        indices = struct.unpack_from("<I", block, 4)[0]
        return [colours[(indices >> (2 * i)) & 3] for i in range(16)]
    colours = _dxt_colours(block[8:16], True)
    indices = struct.unpack_from("<I", block, 12)[0]
    texels = [colours[(indices >> (2 * i)) & 3] for i in range(16)]
    if name == "dxt3":
        alpha_bits = struct.unpack_from("<Q", block, 0)[0]
        alphas = [((alpha_bits >> (4 * i)) & 15) * 17 for i in range(16)]
    else:
        table = _dxt5_alphas(block[0], block[1])
        alpha_bits = int.from_bytes(block[2:8], "little")
        alphas = [table[(alpha_bits >> (3 * i)) & 7] for i in range(16)]
    return [t[:3] + (a,) for t, a in zip(texels, alphas)]


def reference_texel(name, data, layout_level, x, y, z=0, swizzled=True, palette=None):
    """RGBA of texel (x, y, z) of one stored level, as D3D on the Xbox samples it."""
    level = layout_level
    if name in COMPRESSED:
        block_bytes = 8 if name == "dxt1" else 16
        blocks_x = level["stored_w"] // 4
        index = ((z * (level["stored_h"] // 4)) + y // 4) * blocks_x + x // 4
        block = bytes(data[index * block_bytes:(index + 1) * block_bytes])
        return decode_dxt_block(name, block)[(y % 4) * 4 + x % 4]
    bpp = BITS[name] // 8
    if level["pitch"] and not swizzled:
        start = y * level["pitch"] + x * bpp + z * level["pitch"] * level["h"]
    else:
        start = source_texel_index(level, x, y, z, swizzled) * bpp
    t = bytes(data[start:start + bpp])
    if name == "a8":
        return (255, 255, 255, t[0])
    if name == "y8":
        return (t[0], t[0], t[0], 255)
    if name == "ay8":
        return (t[0],) * 4
    if name == "a8y8":
        return (t[0], t[0], t[0], t[1])
    if name == "p8_bump":
        argb = (palette or GLOBAL_VECTOR_PALETTE)[t[0]]
        return ((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24)
    if bpp == 4:
        return (t[2], t[1], t[0], 255 if name == "x8r8g8b8" else t[3])
    v = t[0] | t[1] << 8
    if name == "r5g6b5":
        return _rgb565(v) + (255,)
    if name == "a1r5g5b5":
        return (_e5((v >> 10) & 31), _e5((v >> 5) & 31), _e5(v & 31), 255 if v & 0x8000 else 0)
    return (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17)


# ---- GX encoding ------------------------------------------------------------
@lru_cache(maxsize=64)
def _gx_order(width, height, tile_w, tile_h):
    """Row-major texel indices in GX tile order; -1 marks tile padding."""
    order = []
    for ty in range(0, (height + tile_h - 1) // tile_h * tile_h, tile_h):
        for tx in range(0, (width + tile_w - 1) // tile_w * tile_w, tile_w):
            for y in range(ty, ty + tile_h):
                row = y * width
                for x in range(tx, tx + tile_w):
                    order.append(row + x if x < width and y < height else -1)
    return tuple(order)


def gx_level_bytes(gx_format, width, height):
    tw, th, tb = GX_TILE[gx_format]
    return ((width + tw - 1) // tw) * ((height + th - 1) // th) * tb


def _gather(data, indices, size):
    pad = bytes(size)
    if size == 1:
        return bytes(data[i] if i >= 0 else 0 for i in indices)
    view = bytes(data)
    return b"".join(view[i * size:i * size + size] if i >= 0 else pad for i in indices)


def _swap16(data):
    """Little-endian 16-bit words to big-endian bytes, independent of the host."""
    out = bytearray(len(data))
    out[0::2], out[1::2] = data[1::2], data[0::2]
    return bytes(out)


def _table16(table, data):
    words = array("H", data)
    if sys.byteorder != "little":
        words.byteswap()
    out = array("H", (table[w] for w in words))
    if sys.byteorder == "little":
        out.byteswap()
    return out.tobytes()


@lru_cache(maxsize=None)
def _a1r5g5b5_table():
    table = []
    for v in range(65536):
        if v & 0x8000:
            table.append(v)  # RGB5A3 opaque: 1rrrrrgggggbbbbb, identical bits
        else:
            table.append((((v >> 10) & 31) >> 1) << 8 | (((v >> 5) & 31) >> 1) << 4 | ((v & 31) >> 1))
    return table


def _e3(n):
    return (n << 5) | (n << 2) | (n >> 1)


# Nearest 3-bit RGB5A3 alpha for each 4-bit alpha (ties to the lower code).
_ALPHA4_TO_3 = tuple(min(range(8), key=lambda c: (abs(_e3(c) - a * 17), c)) for a in range(16))


@lru_cache(maxsize=None)
def _a4r4g4b4_table():
    # Always the translucent RGB5A3 mode: 0aaarrrrggggbbbb keeps 4-bit colour exactly.
    return [_ALPHA4_TO_3[v >> 12] << 12 | (v & 0x0FFF) for v in range(65536)]


_REVERSE_PAIRS = bytes(((v & 3) << 6) | (((v >> 2) & 3) << 4) | (((v >> 4) & 3) << 2) | ((v >> 6) & 3)
                       for v in range(256))


def _rgba8_tiles(a, r, g, b):
    """GX RGBA8 tiles from four per-texel channel planes already in tile order."""
    n = len(a)
    ar, gb = bytearray(2 * n), bytearray(2 * n)
    ar[0::2], ar[1::2], gb[0::2], gb[1::2] = a, r, g, b
    return b"".join(bytes(ar[i:i + 32]) + bytes(gb[i:i + 32]) for i in range(0, 2 * n, 32))


def _decode_dxt_level(name, data, level):
    """Reference RGBA8 bytes (row-major, stored dimensions) of one DXT level."""
    block_bytes = 8 if name == "dxt1" else 16
    bw, bh = level["stored_w"] // 4, level["stored_h"] // 4
    out = bytearray(level["stored_w"] * level["stored_h"] * 4)
    stride = level["stored_w"] * 4
    for by in range(bh):
        for bx in range(bw):
            index = by * bw + bx
            texels = decode_dxt_block(name, bytes(data[index * block_bytes:(index + 1) * block_bytes]))
            for row in range(4):
                start = (by * 4 + row) * stride + bx * 16
                out[start:start + 16] = bytes(c for t in texels[row * 4:row * 4 + 4] for c in t)
    return bytes(out)


def _cmpr_level(data, level):
    """DXT1 level -> GX CMPR: swap endpoints, reverse index pairs, 2x2 blocks per tile."""
    raw = bytes(data)
    out = bytearray(raw)
    out[0::8], out[1::8], out[2::8], out[3::8] = raw[1::8], raw[0::8], raw[3::8], raw[2::8]
    for k in range(4, 8):
        out[k::8] = raw[k::8].translate(_REVERSE_PAIRS)
    blocks_x, blocks_y = level["stored_w"] // 4, level["stored_h"] // 4
    tiles_x, tiles_y = (level["w"] + 7) // 8, (level["h"] + 7) // 8
    pad = bytes(8)
    parts = []
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            for sub in range(4):
                bx, by = tx * 2 + (sub & 1), ty * 2 + (sub >> 1)
                if bx < blocks_x and by < blocks_y:
                    i = (by * blocks_x + bx) * 8
                    parts.append(bytes(out[i:i + 8]))
                else:
                    parts.append(pad)
    return b"".join(parts)


def encode_slice(name, gx_format, data, level, z=0, swizzled=True, palette=None):
    """Encode one 2D image (a level, or one depth slice of it) into GX bytes."""
    w, h = level["w"], level["h"]
    if gx_format == GX_CMPR:
        if name != "dxt1" or z:
            raise TextureError("CMPR is only produced from 2D DXT1 levels")
        return _cmpr_level(data, level)
    tw, th, _ = GX_TILE[gx_format]
    order = _gx_order(w, h, tw, th)
    if name in COMPRESSED:
        rgba = _decode_dxt_level(name, data, level)
        sw = level["stored_w"]
        indices = [(i // w) * sw + (i % w) if i >= 0 else -1 for i in order]
        g = _gather(rgba, indices, 4)
        return _rgba8_tiles(g[3::4], g[0::4], g[1::4], g[2::4])
    bpp = BITS[name] // 8
    if swizzled:
        xs, ys, zs = _swizzle_offsets(w, h, level["d"])
        zoff = zs[z]
        indices = [xs[i % w] | ys[i // w] | zoff if i >= 0 else -1 for i in order]
        g = _gather(data, indices, bpp)
    else:
        rows = level["pitch"] // bpp
        indices = [(i // w) * rows + (i % w) if i >= 0 else -1 for i in order]
        g = _gather(data, indices, bpp)
    n = len(order)
    if gx_format == GX_IA8 and name in ("a8", "y8"):
        out = bytearray(2 * n)
        if name == "a8":
            out[0::2], out[1::2] = g, b"\xff" * n
        else:
            out[0::2], out[1::2] = b"\xff" * n, g
        return bytes(out)
    if gx_format == GX_I8 and name == "ay8":
        return g
    if gx_format in (GX_IA8, GX_RGB565) and name in ("a8y8", "r5g6b5"):
        return _swap16(g)
    if gx_format == GX_RGB5A3 and name == "a1r5g5b5":
        return _table16(_a1r5g5b5_table(), g)
    if gx_format == GX_RGB5A3 and name == "a4r4g4b4":
        return _table16(_a4r4g4b4_table(), g)
    if gx_format == GX_RGBA8 and name in ("x8r8g8b8", "a8r8g8b8"):
        a = b"\xff" * n if name == "x8r8g8b8" else g[3::4]
        return _rgba8_tiles(a, g[2::4], g[1::4], g[0::4])
    if gx_format == GX_RGBA8 and name == "p8_bump":
        pal = palette or GLOBAL_VECTOR_PALETTE
        channel = lambda shift: bytes((p >> shift) & 255 for p in pal)  # noqa: E731
        return _rgba8_tiles(g.translate(channel(24)), g.translate(channel(16)), g.translate(channel(8)),
                            g.translate(channel(0)))
    raise TextureError("no GX encoding for this source/target pair")


def gx_target(name, profile=PROFILE_GX_BASELINE):
    target = profile["targets"].get(name)
    for fid, gname in GX_NAMES.items():
        if gname == target:
            return fid
    raise TextureError("profile has no GX target for format")


def convert(fields, pixels, profile=PROFILE_GX_BASELINE, palette=None):
    """Convert one bitmap's pixel bytes. Returns (header dict, GX bytes, image table).

    fields: width, height, depth, type, format, flags, mipmap_count.
    pixels: the bitmap's hardware texture bytes (at least the layout size).
    """
    layout = describe(fields["width"], fields["height"], fields["depth"], fields["type"], fields["format"],
                      fields["flags"], fields["mipmap_count"])
    if len(pixels) < layout["bytes"]:
        raise TextureError("pixel data shorter than the hardware layout")
    name = layout["format"]
    gx_format = gx_target(name, profile)
    swizzled = not layout["linear"] and name not in COMPRESSED
    images = []
    parts = []
    offset = 0
    for face in range(layout["faces"]):
        base = face * layout["face_bytes"]
        for index, level in enumerate(layout["levels"]):
            data = memoryview(pixels)[base + level["offset"]:base + level["offset"] + level["bytes"]]
            slice_bytes = len(data) // level["d"]
            for z in range(level["d"]):
                if name in COMPRESSED:
                    encoded = encode_slice(name, gx_format, data[z * slice_bytes:(z + 1) * slice_bytes], level)
                else:
                    encoded = encode_slice(name, gx_format, data, level, z, swizzled, palette)
                expected = gx_level_bytes(gx_format, level["w"], level["h"])
                if len(encoded) != expected:
                    raise TextureError("encoded level size mismatch")
                images.append({"face": face, "level": index, "slice": z, "width": level["w"],
                               "height": level["h"], "offset": offset, "bytes": len(encoded)})
                parts.append(encoded)
                offset += len(encoded)
    header = {"gx_format": gx_format, "gx_format_name": GX_NAMES[gx_format], "source_format": name,
              "type": layout["type"], "width": fields["width"], "height": fields["height"],
              "depth": fields["depth"], "faces": layout["faces"], "levels": len(layout["levels"]),
              "images": len(images), "source_layout_bytes": layout["bytes"],
              "gx_loadable_dimensions": max(fields["width"], fields["height"]) <= GX_MAX_DIMENSION}
    return header, b"".join(parts), images, layout


# ---- HWT1 container ---------------------------------------------------------
HWT_MAGIC = b"HWT1"
HWT_HEADER_BYTES = 64
TYPE_IDS = {"2d": 0, "3d": 1, "cube": 2}


def pack_hwt(header, data):
    """Big-endian 64-byte header followed by GX images (face, level, slice order)."""
    fixed = struct.pack(">4sHHHHHHHHHHII", HWT_MAGIC, 1, header["gx_format"], header["width"], header["height"],
                        header["depth"], header["faces"], header["levels"], header["images"],
                        FORMAT_IDS[header["source_format"]], TYPE_IDS[header["type"]],
                        HWT_HEADER_BYTES, len(data))
    return fixed + bytes(HWT_HEADER_BYTES - len(fixed)) + data


def unpack_hwt(blob):
    if len(blob) < HWT_HEADER_BYTES:
        raise TextureError("truncated HWT header")
    (magic, version, gx_format, width, height, depth, faces, levels, images, source_format, type_id,
     data_offset, data_bytes) = struct.unpack_from(">4sHHHHHHHHHHII", blob, 0)
    if magic != HWT_MAGIC or version != 1 or data_offset != HWT_HEADER_BYTES:
        raise TextureError("HWT signature/version")
    if gx_format not in GX_NAMES or source_format not in FORMAT_NAMES or type_id not in TYPE_NAMES:
        raise TextureError("HWT enum outside bounds")
    if data_bytes != len(blob) - data_offset:
        raise TextureError("HWT data length mismatch")
    return {"gx_format": gx_format, "width": width, "height": height, "depth": depth, "faces": faces,
            "levels": levels, "images": images, "source_format": FORMAT_NAMES[source_format],
            "type": TYPE_NAMES[type_id]}, memoryview(blob)[data_offset:]


# ---- independent GX decoder (verification only) -------------------------------
def gx_decode_texel(gx_format, data, width, x, y):
    """RGBA the GX sampler returns for texel (x, y) of one tiled image."""
    tw, th, tb = GX_TILE[gx_format]
    tiles_per_row = (width + tw - 1) // tw
    tile = (y // th) * tiles_per_row + (x // tw)
    base = tile * tb
    ix, iy = x % tw, y % th
    if gx_format == GX_CMPR:
        block = base + ((iy // 4) * 2 + (ix // 4)) * 8
        c0, c1 = struct.unpack_from(">HH", data, block)
        a, b = _rgb565(c0), _rgb565(c1)
        row = data[block + 4 + iy % 4]
        index = (row >> (6 - 2 * (ix % 4))) & 3
        if index == 0:
            return a + (255,)
        if index == 1:
            return b + (255,)
        if c0 > c1:
            if index == 2:
                return tuple((5 * p + 3 * q) >> 3 for p, q in zip(a, b)) + (255,)
            return tuple((3 * p + 5 * q) >> 3 for p, q in zip(a, b)) + (255,)
        if index == 2:
            return tuple((p + q) >> 1 for p, q in zip(a, b)) + (255,)
        return (0, 0, 0, 0)
    within = iy * tw + ix
    if gx_format == GX_I8:
        v = data[base + within]
        return (v, v, v, v)
    if gx_format == GX_RGBA8:
        a, r = data[base + 2 * within], data[base + 2 * within + 1]
        g, b = data[base + 32 + 2 * within], data[base + 32 + 2 * within + 1]
        return (r, g, b, a)
    hi, lo = data[base + 2 * within], data[base + 2 * within + 1]
    if gx_format == GX_IA8:
        return (lo, lo, lo, hi)
    v = hi << 8 | lo
    if gx_format == GX_RGB565:
        return _rgb565(v) + (255,)
    if v & 0x8000:
        return (_e5((v >> 10) & 31), _e5((v >> 5) & 31), _e5(v & 31), 255)
    e3 = lambda n: (n << 5) | (n << 2) | (n >> 1)  # noqa: E731
    return (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, e3((v >> 12) & 7))


def tolerance(name, expected, source_block=None):
    """Allowed per-channel error between the reference decode and the GX sample."""
    if name == "a4r4g4b4":
        return (0, 0, 0, 17)
    if name == "a1r5g5b5" and expected[3] == 0:
        return (9, 9, 9, 0)
    if name == "dxt1" and source_block is not None:
        c0, c1 = struct.unpack_from("<HH", source_block, 0)
        if c0 > c1:
            a, b = _rgb565(c0), _rgb565(c1)
            return tuple(abs(p - q) // 24 + 1 for p, q in zip(a, b)) + (0,)
    return (0, 0, 0, 0)


def verify(fields, pixels, gx_bytes, images, layout, palette=None, max_texels=None):
    """Compare every (or up to max_texels per image) GX texel with the reference decode.

    Returns counts; raises TextureError when a texel exceeds its tolerance.
    """
    name = layout["format"]
    swizzled = not layout["linear"] and name not in COMPRESSED
    gx_format = gx_target(name)
    checked = exact = 0
    worst = [0, 0, 0, 0]
    for image in images:
        level = layout["levels"][image["level"]]
        base = image["face"] * layout["face_bytes"] + level["offset"]
        data = memoryview(pixels)[base:base + level["bytes"]]
        slice_bytes = len(data) // level["d"]
        gx = memoryview(gx_bytes)[image["offset"]:image["offset"] + image["bytes"]]
        count = 0
        for y in range(level["h"]):
            for x in range(level["w"]):
                if max_texels is not None and count >= max_texels:
                    break
                count += 1
                if name in COMPRESSED:
                    src = data[image["slice"] * slice_bytes:(image["slice"] + 1) * slice_bytes]
                    expected = reference_texel(name, src, level, x, y)
                    bw = level["stored_w"] // 4
                    block_index = (y // 4) * bw + x // 4
                    block = bytes(src[block_index * 8:block_index * 8 + 8]) if name == "dxt1" else None
                else:
                    expected = reference_texel(name, data, level, x, y, image["slice"], swizzled, palette)
                    block = None
                actual = gx_decode_texel(gx_format, gx, level["w"], x, y)
                allowed = tolerance(name, expected, block)
                if name == "dxt1" and expected[3] == 0:
                    diffs = [0, 0, 0, abs(actual[3] - expected[3])]
                else:
                    diffs = [abs(p - q) for p, q in zip(actual, expected)]
                if any(d > t for d, t in zip(diffs, allowed)):
                    raise TextureError("converted texel outside documented tolerance")
                worst = [max(w, d) for w, d in zip(worst, diffs)]
                checked += 1
                exact += not any(diffs)
    return {"texels_checked": checked, "texels_exact": exact, "max_channel_error": worst}


# ---- engine palette for P8 bump maps (source/bitmaps/bitmaps.c global_vector_palette)
GLOBAL_VECTOR_PALETTE = (
    0xFF7A19CC, 0xFF7E19CC, 0xFF8019CC, 0xFF8119CC, 0xFF8519CC, 0xFF742FE2, 0xFF7A2FE2, 0xFF7E2FE2,
    0xFF802FE2, 0xFF812FE2, 0xFF852FE2, 0xFF8B2FE2, 0xFF6B42ED, 0xFF7442EE, 0xFF7A42EF, 0xFF7E42EF,
    0xFF8042EF, 0xFF8142EF, 0xFF8542EF, 0xFF8B42EE, 0xFF9442ED, 0xFF6052F2, 0xFF6B52F5, 0xFF7452F6,
    0xFF7A52F7, 0xFF7E52F7, 0xFF8052F7, 0xFF8152F7, 0xFF8552F7, 0xFF8B52F6, 0xFF9452F5, 0xFF9F52F2,
    0xFF5260F2, 0xFF6060F7, 0xFF6B60F9, 0xFF7460FB, 0xFF7A60FB, 0xFF7E60FB, 0xFF8060FB, 0xFF8160FB,
    0xFF8560FB, 0xFF8B60FB, 0xFF9460F9, 0xFF9F60F7, 0xFFAD60F2, 0xFF426BED, 0xFF526BF5, 0xFF606BF9,
    0xFF6B6BFC, 0xFF746BFD, 0xFF7A6BFD, 0xFF7E6BFD, 0xFF806BFD, 0xFF816BFD, 0xFF856BFD, 0xFF8B6BFD,
    0xFF946BFC, 0xFF9F6BF9, 0xFFAD6BF5, 0xFFBD6BED, 0xFF2F74E2, 0xFF4274EE, 0xFF5274F6, 0xFF6074FB,
    0xFF6B74FD, 0xFF7474FE, 0xFF7A74FE, 0xFF7E74FE, 0xFF8074FE, 0xFF8174FE, 0xFF8574FE, 0xFF8B74FE,
    0xFF9474FD, 0xFF9F74FB, 0xFFAD74F6, 0xFFBD74EE, 0xFFD074E2, 0xFF197ACC, 0xFF2F7AE2, 0xFF427AEF,
    0xFF527AF7, 0xFF607AFB, 0xFF6B7AFD, 0xFF747AFE, 0xFF7A7AFF, 0xFF7E7AFF, 0xFF807AFF, 0xFF817AFF,
    0xFF857AFF, 0xFF8B7AFE, 0xFF947AFD, 0xFF9F7AFB, 0xFFAD7AF7, 0xFFBD7AEF, 0xFFD07AE2, 0xFFE57ACC,
    0xFF197ECC, 0xFF2F7EE2, 0xFF427EEF, 0xFF527EF7, 0xFF607EFB, 0xFF6B7EFD, 0xFF747EFE, 0xFF7A7EFF,
    0xFF7E7EFF, 0xFF807EFF, 0xFF817EFF, 0xFF857EFF, 0xFF8B7EFE, 0xFF947EFD, 0xFF9F7EFB, 0xFFAD7EF7,
    0xFFBD7EEF, 0xFFD07EE2, 0xFFE57ECC, 0xFF1980CC, 0xFF2F80E2, 0xFF4280EF, 0xFF5280F7, 0xFF6080FB,
    0xFF6B80FD, 0xFF7480FE, 0xFF7A80FF, 0xFF7E80FF, 0xFF8080FF, 0xFF8180FF, 0xFF8580FF, 0xFF8B80FE,
    0xFF9480FD, 0xFF9F80FB, 0xFFAD80F7, 0xFFBD80EF, 0xFFD080E2, 0xFFE580CC, 0xFF1981CC, 0xFF2F81E2,
    0xFF4281EF, 0xFF5281F7, 0xFF6081FB, 0xFF6B81FD, 0xFF7481FE, 0xFF7A81FF, 0xFF7E81FF, 0xFF8081FF,
    0xFF8181FF, 0xFF8581FF, 0xFF8B81FE, 0xFF9481FD, 0xFF9F81FB, 0xFFAD81F7, 0xFFBD81EF, 0xFFD081E2,
    0xFFE581CC, 0xFF1985CC, 0xFF2F85E2, 0xFF4285EF, 0xFF5285F7, 0xFF6085FB, 0xFF6B85FD, 0xFF7485FE,
    0xFF7A85FF, 0xFF7E85FF, 0xFF8085FF, 0xFF8185FF, 0xFF8585FF, 0xFF8B85FE, 0xFF9485FD, 0xFF9F85FB,
    0xFFAD85F7, 0xFFBD85EF, 0xFFD085E2, 0xFFE585CC, 0xFF2F8BE2, 0xFF428BEE, 0xFF528BF6, 0xFF608BFB,
    0xFF6B8BFD, 0xFF748BFE, 0xFF7A8BFE, 0xFF7E8BFE, 0xFF808BFE, 0xFF818BFE, 0xFF858BFE, 0xFF8B8BFE,
    0xFF948BFD, 0xFF9F8BFB, 0xFFAD8BF6, 0xFFBD8BEE, 0xFFD08BE2, 0xFF4294ED, 0xFF5294F5, 0xFF6094F9,
    0xFF6B94FC, 0xFF7494FD, 0xFF7A94FD, 0xFF7E94FD, 0xFF8094FD, 0xFF8194FD, 0xFF8594FD, 0xFF8B94FD,
    0xFF9494FC, 0xFF9F94F9, 0xFFAD94F5, 0xFFBD94ED, 0xFF529FF2, 0xFF609FF7, 0xFF6B9FF9, 0xFF749FFB,
    0xFF7A9FFB, 0xFF7E9FFB, 0xFF809FFB, 0xFF819FFB, 0xFF859FFB, 0xFF8B9FFB, 0xFF949FF9, 0xFF9F9FF7,
    0xFFAD9FF2, 0xFF60ADF2, 0xFF6BADF5, 0xFF74ADF6, 0xFF7AADF7, 0xFF7EADF7, 0xFF80ADF7, 0xFF81ADF7,
    0xFF85ADF7, 0xFF8BADF6, 0xFF94ADF5, 0xFF9FADF2, 0xFF6BBDED, 0xFF74BDEE, 0xFF7ABDEF, 0xFF7EBDEF,
    0xFF80BDEF, 0xFF81BDEF, 0xFF85BDEF, 0xFF8BBDEE, 0xFF94BDED, 0xFF74D0E2, 0xFF7AD0E2, 0xFF7ED0E2,
    0xFF80D0E2, 0xFF81D0E2, 0xFF85D0E2, 0xFF8BD0E2, 0xFF7AE5CC, 0xFF7EE5CC, 0xFF80E5CC, 0xFF81E5CC,
    0xFF85E5CC, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x008080FF,
)
