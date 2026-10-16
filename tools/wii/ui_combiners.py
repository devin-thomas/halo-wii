#!/usr/bin/env python3
"""The interface's combiners: the Xbox's reference and the Wii's TEV program (HWI-033).

The menus' plasma effect (ui_widget.c: two animated plasma maps under the
widget's own bitmap, doing_plasma_effect) is an NV2A register-combiner
program that _rasterizer_psuedo_dynamic_screen_quad_draw
(source/rasterizer/xbox/rasterizer_xbox_dynavobgeom.c) builds from its
parameters. This module holds:

- an evaluator of NV2A general combiner stages and the final combiner with
  the semantics of the Linux port's translator (port/linux/src/nv2a_psh.c):
  input mappings, [-1, 1] register clamps, the mux on r0.a >= 0.5, unique
  per-stage constants, r0 starting as (0, 0, 0, t0.a);
- the plasma program's register words, as the rasterizer writes them for
  map0_to_1 = min (5) and map1_to_2 = alpha blend (0) with the plasma bit;
- the Wii's TEV program for the same thing (port/wii/render/render_ui_gx.c,
  plasma_tev), as an integer model of the TEV's arithmetic (8-bit inputs,
  the lerp's c + (c >> 7) weight and rounding bias, scale, clamp, compares);
- min and max framebuffer blends, which GX has no blend mode for: the Wii
  copies the destination to a texture and compares per channel in the TEV,
  which is exact on an 8-bit framebuffer;
- a checker for the boot mode's self-test lines (BOOT_UI_SELFTEST), which
  draw authored inputs through GX and peek the EFB.

    python tools/wii/ui_combiners.py check-log <engine.log>

The TEV program follows the reference exactly in its structure; its error is
the 8-bit fixed point of the TEV against the combiners' wider precision,
bounded here exhaustively over the differences that drive it
(test_ui_combiners.py).
"""
import argparse
import json
import re
import sys

# ---------------------------------------------------------------- NV2A


def _clamp(value, low=-1.0, high=1.0):
    return max(low, min(high, value))


def _mapping(value, mapping):
    if mapping == 0x00:
        return max(value, 0.0)
    if mapping == 0x20:
        return 1.0 - _clamp(value, 0.0, 1.0)
    if mapping == 0x40:
        return 2.0 * max(value, 0.0) - 1.0
    if mapping == 0x60:
        return 1.0 - 2.0 * max(value, 0.0)
    if mapping == 0x80:
        return max(value, 0.0) - 0.5
    if mapping == 0xA0:
        return 0.5 - max(value, 0.0)
    if mapping == 0xC0:
        return value
    return -value


_REGISTERS = {0: "zero", 1: "c0", 2: "c1", 3: "fog", 4: "v0", 5: "v1", 8: "t0", 9: "t1", 10: "t2", 11: "t3",
              12: "r0", 13: "r1"}


def _input(registers, byte, alpha_portion, constants):
    name = _REGISTERS.get(byte & 0x0F, "zero")
    vector = constants[name] if name in ("c0", "c1") else registers.get(name, (0.0, 0.0, 0.0, 0.0))
    alpha_channel = (byte & 0x10) != 0
    mapping = byte & 0xE0
    if alpha_portion:
        return _mapping(vector[3] if alpha_channel else vector[2], mapping)
    if alpha_channel:
        return tuple(_mapping(vector[3], mapping) for _ in range(3))
    return tuple(_mapping(vector[index], mapping) for index in range(3))


def _scale(value, flags):
    kind = flags & 0x38
    if kind == 0x08:
        return value - 0.5
    if kind == 0x10:
        return value * 2.0
    if kind == 0x18:
        return (value - 0.5) * 2.0
    if kind == 0x20:
        return value * 4.0
    if kind == 0x30:
        return value * 0.5
    return value


def _quantize(value):
    return round(value * 255.0) / 255.0


def combiner_stage(registers, rgb_inputs, rgb_outputs, alpha_inputs, alpha_outputs, constants, mux_msb=True,
                   quantize=False):
    """one general combiner stage, in place (both portions read before either writes); quantize rounds each
    register written to the combiners' 1/255 steps (their registers hold 8 bits and a sign)"""
    writes = []
    for alpha, inputs, outputs in ((False, rgb_inputs, rgb_outputs), (True, alpha_inputs, alpha_outputs)):
        flags = outputs >> 12
        a, b, c, d = (_input(registers, (inputs >> shift) & 0xFF, alpha, constants) for shift in (24, 16, 8, 0))
        if alpha:
            ab, cd = a * b, c * d
        else:
            ab = (sum(x * y for x, y in zip(a, b)),) * 3 if flags & 0x02 else tuple(x * y for x, y in zip(a, b))
            cd = (sum(x * y for x, y in zip(c, d)),) * 3 if flags & 0x01 else tuple(x * y for x, y in zip(c, d))
        if flags & 0x04:
            select_cd = (registers["r0"][3] >= 0.5) if mux_msb else (int(registers["r0"][3] * 255.0 + 0.5) & 1)
            total = cd if select_cd else ab
        else:
            total = ab + cd if alpha else tuple(x + y for x, y in zip(ab, cd))
        if alpha:
            ab, cd, total = (_clamp(_scale(v, flags)) for v in (ab, cd, total))
        else:
            ab, cd, total = (tuple(_clamp(_scale(x, flags)) for x in v) for v in (ab, cd, total))
        for value, shift in ((ab, 4), (cd, 0), (total, 8)):
            name = _REGISTERS.get((outputs >> shift) & 0x0F)
            if name in ("v0", "v1", "t0", "t1", "t2", "t3", "r0", "r1"):
                writes.append((name, alpha, value))
    for name, alpha, value in writes:
        if quantize:
            value = _quantize(value) if alpha else tuple(_quantize(x) for x in value)
        old = list(registers[name])
        if alpha:
            old[3] = value
        else:
            old[0:3] = value
        registers[name] = tuple(old)


def plasma_words(alpha_weighted=False):
    """the stages' words for the plasma effect (rasterizer_xbox_dynavobgeom.c, the map0_to_1 min case and the
    map1_to_2 alpha blend with the plasma bit), as (rgb_in, rgb_out, alpha_in, alpha_out, constant index) a stage;
    the constants: 0 map0's colour, 1 map1's, 2 map2's, 4-7 the plasma fade"""
    stages = [
        (0x08010902, 0x00000089, 0x18111912, 0x00000089, (0, 1)),
        (0x0A010804, 0x000000AC, 0x1A111814, 0x000000AC, (2, None)),
        (0x1920B820, 0x00000C00, 0x0820A920, 0x00000C00, (None, None)),
        (0x00000000, 0x00000000, 0x1C1C0C0C, 0x00024C00, (None, None)),
        (0x00000000, 0x00000000, 0x00005C5C, 0x00004D00, (None, None)),
        (0x1CA01DA0, 0x00000C00, 0x00000000, 0x00000C00, (None, None)),
        (0x0C010A04, 0x00000C00, 0x1C201A20, 0x00000C00, ("fade", None)),
    ]
    final_abcd = 0x0C1C0000 if alpha_weighted else 0x0000000C
    return stages, final_abcd, 0x00001C00


def nv2a_plasma(t0, t1, t2, v0, map_colours, plasma_fade, quantize=False):
    """the Xbox's result (rgba in [0, 1]) for texels t0..t2 (rgba), the vertex colour v0, the maps' colours
    (tint rgb, fade a: three rgba) and the plasma fade (rgba)"""
    registers = {"t0": tuple(t0), "t1": tuple(t1), "t2": tuple(t2), "v0": tuple(v0), "v1": (0.0,) * 4,
                 "r0": (0.0, 0.0, 0.0, t0[3]), "r1": (0.0,) * 4, "fog": (0.0, 0.0, 0.0, 1.0)}
    stages, final_abcd, final_efg = plasma_words()
    for rgb_in, rgb_out, alpha_in, alpha_out, (k0, k1) in stages:
        zero = (0.0,) * 4
        constants = {"c0": plasma_fade if k0 == "fade" else (map_colours[k0] if k0 is not None else zero),
                     "c1": map_colours[k1] if k1 is not None else zero}
        combiner_stage(registers, rgb_in, rgb_out, alpha_in, alpha_out, constants, quantize=quantize)
    assert final_abcd == 0x0000000C and final_efg == 0x00001C00
    r0 = registers["r0"]
    return tuple(_clamp(x, 0.0, 1.0) for x in r0[:3]) + (_clamp(r0[3], 0.0, 1.0),)


# ---------------------------------------------------------------- TEV


def _wrap(value, clamp):
    return max(0, min(255, value)) if clamp else max(-1024, min(1023, value))


def tev_regular(a, b, c, d, sub=False, bias=0, scale=1, clamp=True):
    """one TEV channel: ((d + bias) op lerp(a, b, c)) * scale; a, b, c are 8-bit; bias 0, 128 or -128"""
    a, b, c = a & 0xFF, b & 0xFF, c & 0xFF
    shift = {1: 0, 2: 1, 4: 2, 0.5: 0}[scale]
    lerp_bias = 0 if scale == 0.5 else (127 if sub else 128)
    lerp = ((((a << 8) + (b - a) * (c + (c >> 7))) << shift) + lerp_bias) >> 8
    value = ((d + bias) << shift) + (-lerp if sub else lerp)
    if scale == 0.5:
        value >>= 1
    return _wrap(value, clamp)


def tev_compare_gt(a, b, c, d, clamp=True):
    """compare stage, one channel (R8 or RGB8 on that channel): d + (a > b ? c : 0)"""
    return _wrap(d + ((c & 0xFF) if (a & 0xFF) > (b & 0xFF) else 0), clamp)


def tev_compare_eq(a, b, c, d, clamp=True):
    return _wrap(d + ((c & 0xFF) if (a & 0xFF) == (b & 0xFF) else 0), clamp)


def to_byte(value):
    return int(max(0.0, min(1.0, value)) * 255.0 + 0.5)


def tev_plasma(t0, t1, t2, v0, k0, k_map1, k3, plasma_fade):
    """the Wii's TEV program (render_ui_gx.c plasma_tev), 8-bit integers throughout:
    t0, t1, t2: texels (r, g, b, a); v0: the vertex colour (r, g, b, a);
    k0: map0's (fade, tint blue), k_map1: map1's (fade, tint blue); k3: map2's (tint r, g, b, fade);
    plasma_fade: (r, g, b). Returns (r, g, b, a)."""
    # S0/S1: each plasma map with its texture's channels swapped to (a, a, a, b): its faded alpha in the
    # colour, its tinted blue in the alpha
    a0, b0 = tev_regular(0, t0[3], k0[0], 0), tev_regular(0, t0[2], k0[1], 0)
    a1, b1 = tev_regular(0, t1[3], k_map1[0], 0), tev_regular(0, t1[2], k_map1[1], 0)
    # S2: x = a1 - a0 + 1/2 (colour), y = b0 - b1 + 1/2 (alpha), clamped to [0, 1] (the combiners clamp to
    # [-1, 1], and what follows reads only max(x, 0), max(y, 0) and whether y >= 1/2)
    x = tev_regular(a0, 0, 0, a1, sub=True, bias=128)
    y = tev_regular(b1, 0, 0, b0, sub=True, bias=128)
    # S3: 4x^2 and 4y^2, clamped
    x2 = tev_regular(0, x, x, 0, scale=4)
    y2 = tev_regular(0, y, y, 0, scale=4)
    # S4/S5: the mux on y >= 1/2: (y > 127 ? 4x^2 : 0) + (128 > y ? 4y^2 : 0)
    r = tev_compare_gt(y, 127, x2, 0)
    r = tev_compare_gt(128, y, y2, r)
    # S6/S7: u^2, u = clamp(2r - 1)
    u = tev_regular(0, 0, 0, r, bias=-128, scale=2)
    u2 = tev_regular(0, u, u, 0)
    # S8: P = (r + u^2) / 2
    p = tev_regular(0, u2, 255, r, scale=0.5)
    # S9: map2 tinted; S10: P times the plasma fade; S11: plus map2 times the vertex colour
    t2c = [tev_regular(0, t2[i], k3[i], 0) for i in range(3)]
    t2a = tev_regular(0, t2[3], k3[3], 0)
    pf = [tev_regular(0, p, plasma_fade[i], 0) for i in range(3)]
    rgb = [tev_regular(0, v0[i], t2c[i], pf[i]) for i in range(3)]
    return rgb[0], rgb[1], rgb[2], t2a


def plasma_case(t0, t1, t2, v0, map_colours, plasma_fade, quantize=False):
    """(reference bytes, TEV bytes) for one case given in [0, 1] floats; the inputs are rounded to bytes first,
    as the Wii's textures and constants hold them"""
    def quant(vector):
        return tuple(to_byte(x) / 255.0 for x in vector)
    t0, t1, t2, v0 = (quant(v) for v in (t0, t1, t2, v0))
    map_colours = [quant(c) for c in map_colours]
    plasma_fade = quant(plasma_fade)
    reference = tuple(to_byte(x) for x in nv2a_plasma(t0, t1, t2, v0, map_colours, plasma_fade, quantize))
    b = [tuple(to_byte(x) for x in v) for v in (t0, t1, t2, v0)]
    k = [tuple(to_byte(x) for x in c) for c in map_colours]
    tev = tev_plasma(b[0], b[1], b[2], b[3], (k[0][3], k[0][2]), (k[1][3], k[1][2]), k[2],
                     tuple(to_byte(x) for x in plasma_fade[:3]))
    return reference, tev


# ---------------------------------------------------------------- HUD meters (HWI-087)


def meter_words(flash_negative=False):
    """the meter program's stages (rasterizer_xbox_dynavobgeom.c, meter_parameters), as
    (rgb_in, rgb_out, alpha_in, alpha_out, (c0, c1)) a stage, and the final combiner's words"""
    stages = [
        (0x1120E820, 0x00020C00, 0x12081208, 0x00020C00, ("gradient_min", "gain_max")),
        (0x3C011C02, 0x00000C00, 0x6C200000, 0x000000C0, ("gradient_min", "gradient_max")),
        (0x0C201C00 | (0xE2 if flash_negative else 0x02), 0x00000C00, 0x0820B220, 0x00000C00,
         ("gradient_min", "flash")),
        (0x0C200120, 0x00004C00, 0x12201120, 0x00004C00, ("background", "tint")),
    ]
    return stages, 0x0C180000, 0x00001C00


def _argb_floats(word):
    """a pixel32 (ARGB) as (r, g, b, a) in [0, 1]"""
    return tuple(((word >> shift) & 0xFF) / 255.0 for shift in (16, 8, 0, 24))


def _argb_bytes(word):
    return tuple((word >> shift) & 0xFF for shift in (16, 8, 0, 24))


def gx_blend_one_srcalpha(source, alpha, destination):
    """GX_BM_BLEND (one, source alpha), one 8-bit channel"""
    return max(0, min(255, source + (destination * (alpha + (alpha >> 7)) >> 8)))


def nv2a_meter(texel, meter, destination):
    """the Xbox's meter for one texel (r, g, b, a bytes) over a destination (r, g, b bytes): the combiner
    stages, the final combiner (r0.rgb t0.a, r0.a), the alpha kill and the blend of the tint (constant
    colour) and the source alpha; meter: gradient_min, gradient_max, flash, background, tint (pixel32),
    gain (the first stage's constant alpha byte), flash_negative. Returns the destination's bytes."""
    t0 = tuple(x / 255.0 for x in texel)
    if texel[3] == 0:
        return tuple(destination)
    constants = {name: _argb_floats(meter[name]) for name in ("gradient_min", "gradient_max", "flash", "background",
                                                             "tint")}
    constants["gain_max"] = constants["gradient_max"][:3] + (meter["gain"] / 255.0,)
    registers = {"t0": t0, "r0": (0.0, 0.0, 0.0, t0[3]), "r1": (0.0,) * 4, "v0": (1.0,) * 4, "v1": (0.0,) * 4,
                 "fog": (0.0, 0.0, 0.0, 1.0)}
    stages, final_abcd, final_efg = meter_words(meter.get("flash_negative", False))
    for rgb_in, rgb_out, alpha_in, alpha_out, (k0, k1) in stages:
        combiner_stage(registers, rgb_in, rgb_out, alpha_in, alpha_out, {"c0": constants[k0], "c1": constants[k1]})
    assert final_abcd == 0x0C180000 and final_efg == 0x00001C00
    r0 = registers["r0"]
    rgb = [_clamp(x, 0.0, 1.0) * t0[3] for x in r0[:3]]
    alpha = to_byte(_clamp(r0[3], 0.0, 1.0))
    tint = constants["tint"]
    return tuple(gx_blend_one_srcalpha(to_byte(rgb[i] * tint[i]), alpha, destination[i]) for i in range(3))


def tev_meter(texel, meter, destination):
    """the Wii's TEV program for the meter (render_ui_gx.c wii_ui_gx_meter), 8-bit throughout, and its
    blend (one, source alpha); the same arguments as nv2a_meter"""
    u, coverage = texel[2], texel[3]
    mn, mx, fl, bg, tint = (_argb_bytes(meter[name]) for name in ("gradient_min", "gradient_max", "flash",
                                                                  "background", "tint"))
    k, negative = meter["gain"], meter.get("flash_negative", False)
    # S0 (texture swap b, b, b, b; raster k, k, k, 1): 4 u k; 4 (m - u)
    c0 = tev_regular(0, u, k, 0, scale=4)
    a0 = tev_regular(u, 0, 0, mn[3], sub=True, scale=4)
    # S1: g = 2 C0; w = 2 (1/2 - A0)
    c0 = tev_regular(0, 0, 0, c0, scale=2)
    a1 = tev_regular(a0, 0, 0, 0, sub=True, bias=128, scale=2)
    # S2: lerp(mn, mx, g); S3: +- fl w
    colour = [tev_regular(mn[i], mx[i], c0, 0) for i in range(3)]
    colour = [tev_regular(0, fl[i], a1, colour[i], sub=negative) for i in range(3)]
    # S4: M > u (colour and alpha)
    mask = tev_compare_gt(fl[3], u, 255, 0)
    mask_alpha = tev_compare_gt(fl[3], u, 255, 0)
    # S5: the background where u >= M
    colour = [tev_regular(bg[i], colour[i], mask, 0) for i in range(3)]
    alpha = tev_regular(bg[3], tint[3], mask_alpha, 0)
    # S6: times the coverage; a killed texel's alpha is 1 (it keeps the destination)
    colour = [tev_regular(0, colour[i], coverage, 0) for i in range(3)]
    alpha = tev_compare_eq(coverage, 0, 255, alpha)
    # S7: times the tint
    colour = [tev_regular(0, colour[i], tint[i], 0) for i in range(3)]
    return tuple(gx_blend_one_srcalpha(colour[i], alpha, destination[i]) for i in range(3))


def tev_min_max(source, destination, maximum):
    """the Wii's min/max: three compare stages per channel on the copied destination"""
    result = []
    for s, d in zip(source, destination):
        if maximum:
            value = tev_compare_gt(s, d, s, 0)
            value = tev_compare_gt(d, s, d, value)
        else:
            value = tev_compare_gt(s, d, d, 0)
            value = tev_compare_gt(d, s, s, value)
        value = tev_compare_eq(s, d, s, value)
        result.append(value)
    return tuple(result)


# ---------------------------------------------------------------- self-test lines

SELFTEST = re.compile(r"^BOOT_UI_SELFTEST (\w+) case=(\d+) (.*)$")


def _values(text):
    return {key: tuple(int(x, 16) for x in value.split(",")) for key, value in re.findall(r"(\w+)=([0-9a-f,]+)", text)}


def check_log(text):
    """the self-test cases of a boot launch's log: min/max against the exact per-channel result, the plasma
    against the TEV model (the EFB must hold exactly its result: the GX program is the modelled one) and,
    reported apart, against the combiners' reference. Returns a dict of counts"""
    result = {"cases": 0, "failures": 0, "minmax": 0, "plasma": 0, "plasma_reference_errors": {}}
    for line in text.splitlines():
        match = SELFTEST.match(line)
        if not match:
            continue
        kind, _, rest = match.groups()
        values = _values(rest)
        if kind in ("min", "max"):
            exact = tuple(min(s, d) if kind == "min" else max(s, d) for s, d in zip(values["src"], values["dst"]))
            failed = values["efb"] != exact or tev_min_max(values["src"], values["dst"], kind == "max") != exact
            result["minmax"] += 1
        elif kind == "plasma":
            m0, m1, m2 = values["m0"], values["m1"], values["m2"]
            tev = tev_plasma(values["t0"], values["t1"], values["t2"], values["v0"], (m0[3], m0[2]), (m1[3], m1[2]),
                             m2, values["pf"])
            failed = tuple(values["efb"]) != tev[:3]
            t = [tuple(x / 255.0 for x in values[name]) for name in ("t0", "t1", "t2", "v0")]
            colours = [tuple(x / 255.0 for x in values[name]) for name in ("m0", "m1", "m2")]
            reference = nv2a_plasma(*t, colours, tuple(x / 255.0 for x in values["pf"]) + (1.0,))
            error = max(abs(x - to_byte(y)) for x, y in zip(values["efb"], reference[:3]))
            errors = result["plasma_reference_errors"]
            errors[error] = errors.get(error, 0) + 1
            result["plasma"] += 1
        else:
            continue
        result["cases"] += 1
        result["failures"] += bool(failed)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check-log", help="compare a boot launch's BOOT_UI_SELFTEST lines with the reference")
    check.add_argument("log")
    args = parser.parse_args(argv)
    with open(args.log, encoding="ascii", errors="replace") as stream:
        result = check_log(stream.read())
    result["plasma_reference_errors"] = dict(sorted(result["plasma_reference_errors"].items()))
    print(json.dumps(result))
    return 0 if result["cases"] and not result["failures"] else 1


if __name__ == "__main__":
    sys.exit(main())
