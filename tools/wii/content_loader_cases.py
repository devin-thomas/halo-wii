"""Stage converted content for the Wii loader self-test (HWI-008C).

Picks a deterministic sample of a published content generation (every
texture format and type, sound codec class and recorded-animation version),
computes the SHA-256 of each file's decoded contents with the host decoders,
derives malformed and truncated variants, and writes:

  <out>/sd/halo-wii-content/cases.txt   case list for the guest (sd: paths)
  <out>/sd/halo-wii-content/c/<id>.*    the sampled and malformed files
  <out>/cases-host.txt                  the same list with local paths (host_check)
  <out>/stage.txt                       LOCAL=sd:/path lines for run_dolphin.py --stage
  <out>/cases-private.json              sample identity (map, tag) - private
  <out>/cases-public.json               formats, sizes and expectations only

Decoded digests:
  texture    RGBA8 of every texel of every image (face, level, slice; rows), GX decode rules
  animation  the rebuilt Xbox event stream (equals the manifest's source stream hash)
  sound      decoded samples as big-endian PCM16, interleaved

The output directory must be new and outside the checkout; nothing here is
published. Exit status 0 on success, 1 on any failure.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gx_texture as gt  # noqa: E402
import media_formats as mf  # noqa: E402
import recorded_animation as ra  # noqa: E402

CHECKOUT = Path(__file__).resolve().parents[2]
SD_DIR = "halo-wii-content"
MAX_FILE_BYTES = 8 * 1024 * 1024
TEXTURE_TEXEL_BUDGET = 400_000
SOUND_BYTE_BUDGET = 400_000
EXTENSIONS = {"texture": "hwt", "animation": "hra", "sound": "hws"}


class CaseError(ValueError):
    pass


def external(path):
    resolved = Path(path).resolve()
    if resolved == CHECKOUT or CHECKOUT in resolved.parents:
        raise CaseError("output must be outside the checkout")
    return resolved


def sha(data):
    return hashlib.sha256(bytes(data)).hexdigest()


# ---- decoded digests --------------------------------------------------------
def hwt_images(header):
    """(face, level, slice, width, height, offset, bytes) in container order."""
    images, offset = [], gt.HWT_HEADER_BYTES
    gx = header["gx_format"]
    for face in range(header["faces"]):
        for level in range(header["levels"]):
            w, h = max(header["width"] >> level, 1), max(header["height"] >> level, 1)
            size = gt.gx_level_bytes(gx, w, h)
            for z in range(max(header["depth"] >> level, 1)):
                images.append((face, level, z, w, h, offset, size))
                offset += size
    return images


def texture_digest(blob):
    header, _ = gt.unpack_hwt(blob)
    digest = hashlib.sha256()
    for _, _, _, w, h, offset, size in hwt_images(header):
        image = bytes(blob[offset:offset + size])
        row = bytearray()
        for y in range(h):
            for x in range(w):
                row += bytes(gt.gx_decode_texel(header["gx_format"], image, w, x, y))
            digest.update(row)
            row.clear()
    return digest.hexdigest()


def texels(header):
    return sum(w * h for _, _, _, w, h, _, _ in hwt_images(header))


def sound_digest(blob):
    codec, channels = struct.unpack_from(">H", blob, 6)[0], struct.unpack_from(">H", blob, 12)[0]
    payload = bytes(blob[32:])
    if codec == 2:
        return sha(payload)
    pcm = mf.decode_xbox_adpcm(payload, channels)
    return sha(struct.pack(">%dh" % len(pcm), *pcm))


def animation_digest(blob):
    return sha(ra.encode_xbox(ra.unpack_canonical(bytes(blob))))


DIGESTS = {"texture": texture_digest, "animation": animation_digest, "sound": sound_digest}


# ---- sample selection ---------------------------------------------------------
def select(outputs, generation_dir):
    """Deterministic sample: (kind, entry) pairs."""
    chosen = []

    def pick(group, budget_key):
        within = [e for e in group if e["bytes"] <= MAX_FILE_BYTES and budget_key(e)]
        within.sort(key=lambda e: (e["bytes"], e["path"]))
        if not within:
            return []
        picks = [within[0]]
        if len(within) > 2:
            picks.append(within[len(within) // 2])
        return picks

    textures = {}
    for e in outputs:
        if e["kind"] == "texture":
            textures.setdefault((e["source"]["format"], e["source"]["type"]), []).append(e)

    def texture_budget(e):
        s = e["source"]
        total = sum(max(s["width"] >> l, 1) * max(s["height"] >> l, 1) * max(s["depth"] >> l, 1)
                    for l in range(s["levels"]))
        return total * (6 if s["type"] == "cube" else 1) <= TEXTURE_TEXEL_BUDGET

    for key in sorted(textures):
        group = textures[key]
        mipped = [e for e in group if e["source"]["levels"] > 1]
        chosen += [("texture", e) for e in (pick(mipped, texture_budget) or pick(group, texture_budget))]
    oversize = [e for e in outputs if e["kind"] == "texture" and not e["gx"]["loadable_dimensions"]
                and e["bytes"] <= MAX_FILE_BYTES]
    if oversize:
        chosen.append(("texture", min(oversize, key=lambda e: (e["bytes"], e["path"]))))
    sounds = {}
    for e in outputs:
        if e["kind"] == "sound":
            s = e["source"]
            sounds.setdefault((s["codec"], s["channels"], s["rate"]), []).append(e)
    for key in sorted(sounds):
        chosen += [("sound", e) for e in pick(sounds[key], lambda e: e["bytes"] <= SOUND_BYTE_BUDGET)]
    animations = {}
    for e in outputs:
        if e["kind"] == "animation":
            s = e["source"]
            animations.setdefault((s["version"], s["unit_control_version"]), []).append(e)
    for key in sorted(animations):
        group = sorted(animations[key], key=lambda e: (e["bytes"], e["path"]))
        chosen.append(("animation", group[0]))
        if len(group) > 1:
            chosen.append(("animation", group[-1]))
    seen, unique = set(), []
    for kind, e in chosen:
        if e["path"] not in seen:
            seen.add(e["path"])
            unique.append((kind, e))
    return unique


# ---- malformed variants ---------------------------------------------------------
def put16(blob, offset, value):
    struct.pack_into(">H", blob, offset, value & 0xFFFF)


def put32(blob, offset, value):
    struct.pack_into(">I", blob, offset, value & 0xFFFFFFFF)


def texture_variants(blob):
    """(name, bytes, expected error) for one valid 2D texture with mip levels."""
    b = bytes(blob)
    header, _ = gt.unpack_hwt(b)
    out = [("header_truncated", b[:40], "truncated"), ("data_truncated", b[:-32], "length"),
           ("trailing_bytes", b + bytes(32), "length")]

    def edit(name, error, fn):
        m = bytearray(b)
        fn(m)
        out.append((name, bytes(m), error))
    edit("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HWX1"))
    edit("bad_version", "version", lambda m: put16(m, 4, 2))
    edit("gx_format_unknown", "enum", lambda m: put16(m, 6, 7))
    wrong = gt.GX_RGBA8 if header["gx_format"] != gt.GX_RGBA8 else gt.GX_I8
    edit("gx_format_against_profile", "enum", lambda m: put16(m, 6, wrong))
    edit("width_zero", "dimensions", lambda m: put16(m, 8, 0))
    edit("levels_above_chain", "dimensions", lambda m: put16(m, 16, 20))
    edit("levels_short", "count", lambda m: put16(m, 16, header["levels"] - 1))
    edit("images_extra", "count", lambda m: put16(m, 18, header["images"] + 1))
    edit("data_bytes_extra", "length", lambda m: put32(m, 28, len(b) - 64 + 32))
    edit("reserved_nonzero", "reserved", lambda m: m.__setitem__(40, 1))
    edit("type_unknown", "enum", lambda m: put16(m, 22, 7))
    return out


def animation_variants(blob):
    b = bytes(blob)
    decoded = ra.unpack_canonical(b)
    fields, events = len(decoded["unit_control"]), len(decoded["events"])
    events_at = ra.HRA_HEADER.size + 4 * (fields + len(decoded["state"]))
    out = [("header_truncated", b[:20], "truncated"), ("events_truncated", b[:-16 - len(decoded["tail"])],
                                                                        "truncated"),
           ("trailing_bytes", b + bytes(4), "length")]

    def edit(name, error, fn):
        m = bytearray(b)
        fn(m)
        out.append((name, bytes(m), error))
    edit("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HRX1"))
    edit("bad_animation_version", "version", lambda m: m.__setitem__(6, 9))
    edit("field_count_extra", "count", lambda m: put16(m, 10, fields + 1))
    edit("total_ticks_off", "count", lambda m: put32(m, 20, decoded["total_ticks"] + 1))
    edit("unknown_event_type", "event", lambda m: m.__setitem__(events_at, 63))
    edit("end_event_not_last", "event", lambda m: m.__setitem__(events_at + 16 * (events - 1), 0))
    current = decoded["codec"] == ra.CODEC_CURRENT
    for index, (event_type, kind, delta, payload) in enumerate(decoded["events"]):
        codes = (ra.CURRENT_PAYLOADS if current else ra.V1_PAYLOADS)[event_type]
        at = events_at + 16 * index
        if len(codes) < 3:
            edit("unused_payload_word", "reserved", lambda m, at=at, n=len(codes): put32(m, at + 4 + 4 * n, 1))
            break
    for index, (event_type, kind, delta, payload) in enumerate(decoded["events"]):
        codes = (ra.CURRENT_PAYLOADS if current else ra.V1_PAYLOADS)[event_type]
        if codes and codes[0] in ("u8", "s8", "s16", "u16"):
            at = events_at + 16 * index
            edit("payload_outside_width", "value", lambda m, at=at: put32(m, at + 4, 0x12345))
            break
    if current:
        for index, (event_type, kind, delta, payload) in enumerate(decoded["events"]):
            if kind == 1:
                at = events_at + 16 * index
                edit("delta_against_kind", "delta", lambda m, at=at: put16(m, at + 2, 7))
                break
    # A float unit-control word changed: still a valid record, but the
    # rebuilt stream no longer matches the recorded source tag.
    float_index = next(i for i, (code, _) in enumerate(ra.unit_control_fields(decoded["unit_control_version"]))
                       if code == "f32")
    edit("source_tag_mismatch", "identity",
         lambda m: put32(m, ra.HRA_HEADER.size + 4 * float_index,
                         struct.unpack_from(">I", m, ra.HRA_HEADER.size + 4 * float_index)[0] ^ 1))
    return out


def sound_variants(blob):
    b = bytes(blob)
    channels = struct.unpack_from(">H", b, 12)[0]
    frames = struct.unpack_from(">I", b, 16)[0]
    out = [("header_truncated", b[:24], "truncated"), ("payload_truncated", b[:-1], "length"),
           ("trailing_bytes", b + bytes(36 * channels), "length")]

    def edit(name, error, fn, base=b):
        m = bytearray(base)
        fn(m)
        out.append((name, bytes(m), error))
    edit("bad_magic", "magic", lambda m: m.__setitem__(slice(0, 4), b"HWX1"))
    edit("bad_version", "version", lambda m: put16(m, 4, 0))
    edit("codec_unknown", "enum", lambda m: put16(m, 6, 3))
    edit("rate_unknown", "enum", lambda m: put32(m, 8, 32000))
    edit("channels_three", "enum", lambda m: put16(m, 12, 3))
    edit("reserved_nonzero", "reserved", lambda m: put16(m, 14, 1))
    edit("frames_extra", "count", lambda m: put32(m, 16, frames + 1))
    edit("partial_block", "count", lambda m: put32(m, 20, len(b) - 32 - 1), base=b[:-1])
    return out


VARIANTS = {"texture": texture_variants, "animation": animation_variants, "sound": sound_variants}


def build(root, out, prefix="sd:/" + SD_DIR + "/"):
    root, out = Path(root).resolve(), external(out)
    if out.exists():
        raise CaseError("output directory must be new")
    generation, manifest_sha = (root / "CURRENT").read_text(encoding="ascii").split()
    gen_dir = root / "generations" / generation
    raw = (gen_dir / "manifest.json").read_bytes()
    if sha(raw) != manifest_sha:
        raise CaseError("manifest hash differs from CURRENT")
    manifest = json.loads(raw)
    files_dir = out / "sd" / SD_DIR / "c"
    files_dir.mkdir(parents=True)
    rows, private, public = [], [], []
    counters = {"texture": 0, "animation": 0, "sound": 0}
    first_valid = {}
    for kind, entry in select(manifest["outputs"], gen_dir):
        blob = (gen_dir / entry["path"]).read_bytes()
        if sha(blob) != entry["sha256"] or len(blob) != entry["bytes"]:
            raise CaseError("published file differs from its manifest entry")
        case_id = "%s%02d" % (kind[0], counters[kind])
        counters[kind] += 1
        decoded = DIGESTS[kind](blob)
        if kind == "animation" and decoded != entry["source"]["stream_sha256"]:
            raise CaseError("rebuilt animation stream differs from the manifest source hash")
        name = "%s.%s" % (case_id, EXTENSIONS[kind])
        (files_dir / name).write_bytes(blob)
        rows.append((case_id, kind, "ok", name, len(blob), entry["sha256"], decoded))
        detail = {k: v for k, v in entry["source"].items() if k not in ("map", "tag", "bitmap", "index",
                                                                         "source_sha256", "stream_sha256")}
        if kind == "texture":
            detail.update(gx=entry["gx"]["format"], images=entry["gx"]["images"],
                          gx_loadable=entry["gx"]["loadable_dimensions"])
            header, _ = gt.unpack_hwt(blob)
            detail["texels"] = texels(header)
            if header["type"] == "2d" and header["levels"] > 1 and "texture" not in first_valid \
                    and entry["gx"]["loadable_dimensions"]:
                first_valid["texture"] = blob
        elif kind not in first_valid and (kind != "animation" or entry["source"]["version"] == 4):
            first_valid[kind] = blob
        public.append(dict(detail, id=case_id, kind=kind, bytes=len(blob), expect="ok"))
        private.append({"id": case_id, "path": entry["path"], "map": entry["source"]["map"],
                        "sha256": entry["sha256"], "decoded_sha256": decoded})
    for kind in ("texture", "animation", "sound"):
        if kind not in first_valid:
            raise CaseError("no valid %s to derive malformed variants from" % kind)
        for index, (variant, data, error) in enumerate(VARIANTS[kind](first_valid[kind])):
            case_id = "m%s%02d" % (kind[0], index)
            name = "%s.%s" % (case_id, EXTENSIONS[kind])
            (files_dir / name).write_bytes(data)
            rows.append((case_id, kind, error, name, len(data), sha(data), "-"))
            public.append({"id": case_id, "kind": kind, "variant": variant, "bytes": len(data), "expect": error})
    sd_lines, host_lines, stage = [], [], []
    for case_id, kind, expect, name, size, file_sha, decoded in rows:
        sd_lines.append("case %s %s %s %sc/%s %d %s %s" % (case_id, kind, expect, prefix, name, size, file_sha,
                                                             decoded))
        host_lines.append("case %s %s %s %s %d %s %s" % (case_id, kind, expect, (files_dir / name).as_posix(),
                                                           size, file_sha, decoded))
        stage.append("%s=%sc/%s" % ((files_dir / name).as_posix(), prefix, name))
    header = "# HWI-008C content loader cases: id kind expect path bytes file_sha256 decoded_sha256\n"
    (out / "sd" / SD_DIR / "cases.txt").write_text(header + "\n".join(sd_lines) + "\n", encoding="ascii",
                                                   newline="\n")
    stage.append("%s=%scases.txt" % ((out / "sd" / SD_DIR / "cases.txt").as_posix(), prefix))
    (out / "cases-host.txt").write_text(header + "\n".join(host_lines) + "\n", encoding="ascii", newline="\n")
    (out / "stage.txt").write_text("\n".join(stage) + "\n", encoding="ascii", newline="\n")
    summary = {"generation": generation, "manifest_sha256": manifest_sha, "cases": len(rows),
               "valid": sum(1 for r in rows if r[2] == "ok"), "malformed": sum(1 for r in rows if r[2] != "ok"),
               "bytes": sum(r[4] for r in rows)}
    (out / "cases-private.json").write_text(json.dumps(dict(summary, samples=private), indent=1, sort_keys=True)
                                            + "\n", encoding="utf-8")
    (out / "cases-public.json").write_text(json.dumps(dict(summary, cases=public), indent=1, sort_keys=True) + "\n",
                                           encoding="utf-8")
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", required=True, type=Path, help="content_convert.py output root (private)")
    parser.add_argument("--out", required=True, type=Path, help="new private output directory")
    args = parser.parse_args(argv)
    try:
        summary = build(args.root, args.out)
    except (CaseError, gt.TextureError, ra.AnimationError, mf.MediaError, OSError, ValueError) as error:
        print("content loader cases failed: %s" % error, file=sys.stderr)
        return 1
    print("wrote %d cases (%d valid, %d malformed, %d bytes)" % (summary["cases"], summary["valid"],
                                                                 summary["malformed"], summary["bytes"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
