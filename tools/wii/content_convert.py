"""Deterministic Halo (Xbox build 01.10.12.2276) to Wii content conversion.

Reads the private staged maps named by a staging manifest (staging.json:
file name, size and SHA-256 for each map), never modifies them, and publishes
one validated content generation under an output root (content_publish.py):

  textures/<map>/<tag>-<bitmap>.hwt   GX textures, every hardware mip level
  animations/<map>/<index>.hra        canonical big-endian recorded animations
  sounds/<map>/<tag>-<range>-<perm>.hws  sound permutations, codec retained

The manifest records the converter version and the hashes of its own
modules, the profile, every source map hash and every output hash, and no
timestamps or host paths, so identical input and profile give a
byte-identical manifest. Every planned item is validated before any of its
map's outputs are written; any invalid item fails the run and nothing is
published. Run facts (timings, verification counts, faults) go to a separate
run record outside the generation.

Exit status: 0 published; 1 invalid input or conversion failure; 2 usage;
3 insufficient destination space; 4 I/O failure (e.g. disk full). In every
non-zero case the previously published generation is left as it was.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_publish as publish  # noqa: E402
import gx_texture as gt  # noqa: E402
import halo_cache as hc  # noqa: E402
import media_formats as mf  # noqa: E402
import recorded_animation as ra  # noqa: E402

CONVERTER = "halo-wii-content-convert"
CONVERTER_VERSION = "1.0.0"
CHECKOUT = Path(__file__).resolve().parents[2]
MODULES = ("content_convert.py", "content_publish.py", "gx_texture.py", "halo_cache.py", "media_formats.py",
           "recorded_animation.py")
CATEGORIES = ("textures", "animations", "sounds")
MAP_NAME = re.compile(r"^[a-z0-9_]{1,32}$")
MAX_BITMAPS_PER_GROUP = 2048
MAX_PITCH_RANGES = 8
MAX_PERMUTATIONS = 256
MAX_RECORDED_ANIMATIONS = 1024
SCENARIO_RECORDED_ANIMATIONS = 0x36C
SCENARIO_BYTES = 0x5B0
VERIFY_ADPCM_BLOCKS = 64

PROFILES = {
    "gx-baseline-v1": {
        "name": "gx-baseline-v1",
        "textures": dict(gt.PROFILE_GX_BASELINE, fidelity=gt.FIDELITY, container="HWT1"),
        "animations": {"container": "HRA1", "byte_order": "big-endian, explicit field decode (ADR-018)"},
        "sounds": {"container": "HWS1", "xbox_adpcm": "payload retained verbatim (decoded at runtime)",
                   "pcm16le": "converted to big-endian PCM16"},
    },
}
HWS_MAGIC = b"HWS1"
HWS_CODECS = {"xbox_adpcm": 1, "pcm16be": 2}


class ConversionError(ValueError):
    """An invalid item; the message names map, tag ordinal and reason only."""


def code_identity():
    """SHA-256 of each converter module with line endings normalized to LF."""
    here = Path(__file__).resolve().parent
    return {name: hashlib.sha256((here / name).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            for name in MODULES}


def external(path):
    resolved = Path(path).resolve()
    if resolved == CHECKOUT or CHECKOUT in resolved.parents:
        raise ConversionError("inputs and outputs must be outside the checkout")
    return resolved


def load_staging(staging_dir):
    staging_dir = external(staging_dir)
    raw = (staging_dir / "staging.json").read_bytes()
    record = json.loads(raw)
    maps = {}
    for entry in record.get("files", []):
        name = entry.get("path", "")
        if not name.endswith(".map") or not MAP_NAME.match(name[:-4]) or "/" in name or "\\" in name:
            raise ConversionError("staging entry name not allowed")
        if not isinstance(entry.get("size"), int) or not re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", "")):
            raise ConversionError("staging entry identity malformed")
        maps[name[:-4]] = entry
    if not maps:
        raise ConversionError("staging manifest lists no maps")
    return staging_dir, maps, {"staging_manifest_sha256": hashlib.sha256(raw).hexdigest(),
                               "image_sha256": record.get("image_sha256")}


# ---- planning (validation before any write) ----------------------------------
def plan_textures(cache, map_name, profile):
    items = []
    for instance in cache.by_group("bitm"):
        root = cache.root_offset(instance, 0x6C)
        where = "%s tag %d" % (map_name, instance["ordinal"])
        if root is None:
            raise ConversionError(where + ": bitmap root outside tag data")
        try:
            elements = cache.block(root + 0x60, 0x30, MAX_BITMAPS_PER_GROUP)
        except hc.CacheError as error:
            raise ConversionError("%s: %s" % (where, error)) from None
        for index, element in enumerate(elements):
            w, h, d, type_id, format_id, flags = struct.unpack_from("<hhhhhH", cache.tags, element + 4)
            fields = {"width": w, "height": h, "depth": d, "type": type_id, "format": format_id, "flags": flags,
                      "mipmap_count": cache.s16(element + 0x14)}
            pixels_offset, pixels_size = cache.s32(element + 0x18), cache.s32(element + 0x1C)
            try:
                layout = gt.describe(w, h, d, type_id, format_id, flags, fields["mipmap_count"])
                gt.gx_target(layout["format"], profile["textures"])
                if layout["bytes"] > pixels_size:
                    raise gt.TextureError("hardware layout larger than the stored pixel size")
                cache.file_span(pixels_offset, layout["bytes"])
            except (gt.TextureError, hc.CacheError) as error:
                raise ConversionError("%s bitmap %d: %s" % (where, index, error)) from None
            gx = gt.gx_target(layout["format"], profile["textures"])
            estimate = gt.HWT_HEADER_BYTES + sum(
                gt.gx_level_bytes(gx, lv["w"], lv["h"]) * lv["d"] for lv in layout["levels"]) * layout["faces"]
            items.append({"kind": "texture", "path": "textures/%s/%05d-%03d.hwt" % (map_name, instance["ordinal"], index),
                          "tag": instance["ordinal"], "index": index, "fields": fields, "pixels_offset": pixels_offset,
                          "layout_bytes": layout["bytes"], "estimate": estimate})
    return items


def plan_animations(cache, map_name):
    scenario = cache.instances[cache.scenario_ordinal]
    root = cache.root_offset(scenario, SCENARIO_BYTES)
    if root is None:
        raise ConversionError("%s: scenario root outside tag data" % map_name)
    items = []
    try:
        elements = cache.block(root + SCENARIO_RECORDED_ANIMATIONS, 0x40, MAX_RECORDED_ANIMATIONS)
    except hc.CacheError as error:
        raise ConversionError("%s recorded animations: %s" % (map_name, error)) from None
    for index, element in enumerate(elements):
        try:
            stream = cache.tag_data_bytes(element + 44, ra.MAX_STREAM_BYTES)
            version, unit_control, ticks = cache.tags[element + 32], cache.tags[element + 34], cache.s16(element + 36)
            canonical, facts = ra.convert(stream, version, unit_control, ticks)
        except (hc.CacheError, ra.AnimationError) as error:
            raise ConversionError("%s recorded animation %d: %s" % (map_name, index, error)) from None
        items.append({"kind": "animation", "path": "animations/%s/%04d.hra" % (map_name, index), "index": index,
                      "data": canonical, "estimate": len(canonical),
                      "source": {"version": version, "unit_control_version": unit_control, "length_in_ticks": ticks,
                                 "stream_bytes": len(stream), "stream_sha256": hashlib.sha256(stream).hexdigest(),
                                 "length_equals_delta_sum_plus_1": ticks == facts["total_ticks"] + 1},
                      "facts": facts})
    return items


def plan_sounds(cache, map_name):
    items = []
    for instance in cache.by_group("snd!"):
        where = "%s tag %d" % (map_name, instance["ordinal"])
        root = cache.root_offset(instance, 0xA4)
        if root is None:
            raise ConversionError(where + ": sound root outside tag data")
        rate_id, encoding, compression = cache.s16(root + 6), cache.s16(root + 0x6C), cache.s16(root + 0x6E)
        try:
            if rate_id not in mf.SAMPLE_RATES or encoding not in mf.ENCODINGS:
                raise mf.MediaError("sample rate or encoding outside the engine's enums")
            channels = mf.ENCODINGS[encoding]
            for r_index, pitch_range in enumerate(cache.block(root + 0x98, 0x48, MAX_PITCH_RANGES)):
                for p_index, permutation in enumerate(cache.block(pitch_range + 0x3C, 0x7C, MAX_PERMUTATIONS)):
                    p_compression = cache.s16(permutation + 0x28)
                    samples = cache.tag_data(permutation + 0x40)
                    if p_compression != compression:
                        raise mf.MediaError("permutation compression differs from its sound")
                    if samples["flags"] & 1:
                        raise mf.MediaError("samples in an external resource file")
                    cache.file_span(samples["file_offset"], samples["size"])
                    codec = mf.COMPRESSIONS.get(p_compression)
                    if codec == "xbox_adpcm":
                        frames = mf.adpcm_frames(samples["size"], channels)
                    elif codec == "pcm16le":
                        if samples["size"] % (2 * channels):
                            raise mf.MediaError("PCM payload is not a whole number of frames")
                        frames = samples["size"] // (2 * channels)
                    else:
                        raise mf.MediaError("sound compression not handled by this profile")
                    items.append({"kind": "sound", "path": "sounds/%s/%05d-%d-%03d.hws" % (
                        map_name, instance["ordinal"], r_index, p_index), "tag": instance["ordinal"],
                        "codec": codec, "channels": channels, "rate": mf.SAMPLE_RATES[rate_id], "frames": frames,
                        "file_offset": samples["file_offset"], "size": samples["size"],
                        "estimate": 32 + samples["size"]})
        except (hc.CacheError, mf.MediaError) as error:
            raise ConversionError("%s: %s" % (where, error)) from None
    return items


def pack_hws(codec, rate, channels, frames, payload):
    header = struct.pack(">4sHHIHHII", HWS_MAGIC, 1, HWS_CODECS[codec], rate, channels, 0, frames, len(payload))
    return header + bytes(32 - len(header)) + payload


# ---- conversion ---------------------------------------------------------------
def convert_map(cache, map_name, categories, profile, generation, verify_per_kind, record):
    planned = []
    if "textures" in categories:
        planned += plan_textures(cache, map_name, profile)
    if "animations" in categories:
        planned += plan_animations(cache, map_name)
    if "sounds" in categories:
        planned += plan_sounds(cache, map_name)
    generation.require_space(sum(item["estimate"] for item in planned))
    outputs = []
    verified = {}
    for item in planned:
        if item["kind"] == "texture":
            pixels = cache.file_span(item["pixels_offset"], item["layout_bytes"])
            header, data, images, layout = gt.convert(item["fields"], pixels, profile["textures"])
            key = "texture:%s:%s" % (layout["format"], layout["type"])
            if verified.setdefault(key, 0) < verify_per_kind:
                verified[key] += 1
                result = gt.verify(item["fields"], pixels, data, images, layout)
                summary = record["verification"].setdefault(key, {"textures": 0, "texels_checked": 0,
                                                                  "texels_exact": 0, "max_channel_error": [0] * 4})
                summary["textures"] += 1
                summary["texels_checked"] += result["texels_checked"]
                summary["texels_exact"] += result["texels_exact"]
                summary["max_channel_error"] = [max(a, b) for a, b in zip(summary["max_channel_error"],
                                                                         result["max_channel_error"])]
            blob = gt.pack_hwt(header, data)
            entry = generation.write(item["path"], blob)
            entry.update(kind="texture", source={"map": map_name, "tag": item["tag"], "bitmap": item["index"],
                                                 "format": layout["format"], "type": layout["type"],
                                                 "width": header["width"], "height": header["height"],
                                                 "depth": header["depth"], "levels": header["levels"],
                                                 "source_bytes": layout["bytes"],
                                                 "source_sha256": hashlib.sha256(pixels).hexdigest()},
                         gx={"format": header["gx_format_name"], "images": header["images"],
                             "loadable_dimensions": header["gx_loadable_dimensions"]})
        elif item["kind"] == "animation":
            entry = generation.write(item["path"], item["data"])
            entry.update(kind="animation", source=dict(item["source"], map=map_name, index=item["index"]),
                         events=item["facts"]["events"], round_trip=item["facts"]["round_trip"])
        else:
            payload = bytes(cache.file_span(item["file_offset"], item["size"]))
            codec = item["codec"]
            if codec == "pcm16le":
                payload, codec = mf.pcm16le_to_be(payload, item["channels"]), "pcm16be"
            key = "sound:%s:%d:%d" % (item["codec"], item["channels"], item["rate"])
            if codec == "xbox_adpcm" and verified.setdefault(key, 0) < verify_per_kind:
                verified[key] += 1
                head = payload[:VERIFY_ADPCM_BLOCKS * mf.ADPCM_BLOCK_BYTES * item["channels"]]
                pcm = mf.decode_xbox_adpcm(head, item["channels"])
                summary = record["verification"].setdefault(key, {"permutations": 0, "frames_decoded": 0})
                summary["permutations"] += 1
                summary["frames_decoded"] += len(pcm) // item["channels"]
            blob = pack_hws(codec, item["rate"], item["channels"], item["frames"], payload)
            entry = generation.write(item["path"], blob)
            entry.update(kind="sound", source={"map": map_name, "tag": item["tag"], "codec": item["codec"],
                                               "channels": item["channels"], "rate": item["rate"],
                                               "frames": item["frames"],
                                               "source_sha256": hashlib.sha256(
                                                   cache.file_span(item["file_offset"], item["size"])).hexdigest()})
        outputs.append(entry)
    return outputs


def verify_output(entry, data):
    """Structural re-read of a published file (used by content_publish.verify_tree)."""
    kind = entry.get("kind")
    if kind == "texture":
        header, body = gt.unpack_hwt(data)
        if gt.GX_NAMES[header["gx_format"]] != entry["gx"]["format"] or header["images"] != entry["gx"]["images"]:
            raise publish.PublishError("texture header disagrees with its manifest entry")
    elif kind == "animation":
        ra.unpack_canonical(data)
    elif kind == "sound":
        if data[:4] != HWS_MAGIC or struct.unpack_from(">I", data, 20)[0] != len(data) - 32:
            raise publish.PublishError("sound container header inconsistent")


def run(staging_dir, map_names, categories, profile_name, output_root, verify_per_kind=2, fault=None,
        disk_usage=None):
    if profile_name not in PROFILES:
        raise ConversionError("unknown profile")
    profile = PROFILES[profile_name]
    staging_dir, staged, chain = load_staging(staging_dir)
    output_root = external(output_root)
    if staging_dir == output_root or staging_dir in output_root.parents or output_root in staging_dir.parents:
        raise ConversionError("output root must be separate from the source staging directory")
    names = sorted(map_names or staged)
    for name in names:
        if name not in staged:
            raise ConversionError("map not listed in the staging manifest")
    if not categories or any(c not in CATEGORIES for c in categories):
        raise ConversionError("unknown category")
    record = {"maps": {}, "verification": {}, "source_unchanged": {}}
    kwargs = {"fault": fault} if disk_usage is None else {"fault": fault, "disk_usage": disk_usage}
    generation = publish.Generation(output_root, **kwargs)
    started = time.perf_counter()
    try:
        sources = []
        outputs = []
        for name in names:
            path = staging_dir / "maps" / (name + ".map")
            before = os.stat(path)
            t0 = time.perf_counter()
            try:
                cache = hc.open_map(path, staged[name]["sha256"])
            except hc.CacheError as error:
                raise ConversionError("%s: %s" % (name, error)) from None
            if cache.header["name"] != name:
                raise ConversionError("%s: header name differs from the staged file name" % name)
            sources.append({"map": name, "sha256": cache.facts["source_sha256"],
                            "stored_bytes": cache.facts["stored_bytes"],
                            "declared_bytes": cache.header["declared_bytes"], "build": cache.header["build"],
                            "category": cache.header["category"],
                            "inflated_sha256": hashlib.sha256(cache.body).hexdigest()})
            outputs += convert_map(cache, name, categories, profile, generation, verify_per_kind, record)
            del cache
            after = os.stat(path)
            record["maps"][name] = {"seconds": round(time.perf_counter() - t0, 3)}
            record["source_unchanged"][name] = ((before.st_size, before.st_mtime_ns) ==
                                                (after.st_size, after.st_mtime_ns))
        manifest = {
            "format": "halo-wii-content-manifest", "format_version": 1,
            "converter": {"name": CONVERTER, "version": CONVERTER_VERSION, "modules_sha256": code_identity()},
            "profile": profile, "profile_sha256": hashlib.sha256(
                json.dumps(profile, sort_keys=True).encode("ascii")).hexdigest(),
            "source_chain": chain, "sources": sources, "categories": sorted(categories),
            "totals": {kind: {"files": sum(1 for o in outputs if o["kind"] == kind),
                              "bytes": sum(o["bytes"] for o in outputs if o["kind"] == kind)}
                       for kind in ("texture", "animation", "sound")},
            "outputs": outputs,
        }
        result = generation.finish(manifest, verify_output)
    except BaseException:
        generation.abandon()
        raise
    record["source_sha256_matches_after"] = {}
    for name in names:  # sources re-hashed after publication: never modified
        after = hc.sha256_file(staging_dir / "maps" / (name + ".map")) == staged[name]["sha256"]
        record["source_sha256_matches_after"][name] = after
        if not after or not record["source_unchanged"][name]:
            raise ConversionError("%s: source changed during conversion" % name)
    record.update(result, seconds=round(time.perf_counter() - started, 3), totals=manifest["totals"],
                  outputs=len(manifest["outputs"]), sources_rehashed_after=True)
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staging", required=True, type=Path, help="private directory with staging.json and maps/")
    parser.add_argument("--maps", nargs="*", help="map names (default: every staged map)")
    parser.add_argument("--categories", nargs="+", default=list(CATEGORIES), choices=CATEGORIES)
    parser.add_argument("--profile", default="gx-baseline-v1", choices=sorted(PROFILES))
    parser.add_argument("--output-root", required=True, type=Path, help="private publication root")
    parser.add_argument("--verify-per-kind", type=int, default=2,
                        help="items per format/type per map re-decoded and compared (default 2)")
    parser.add_argument("--record", type=Path, help="new private run-record JSON file")
    parser.add_argument("--fault-after-bytes", type=int, help="simulate a full disk after this many bytes")
    parser.add_argument("--fault-stage", choices=("before-manifest", "before-validate", "before-promote",
                                                  "before-current"), help="simulate an interruption")
    args = parser.parse_args(argv)
    fault = publish.FaultInjector(args.fault_after_bytes, args.fault_stage)
    status, record = 0, None
    try:
        if args.record is not None and external(args.record).exists():
            print("Content conversion failed: run record already exists", file=sys.stderr)
            return 1
        record = run(args.staging, args.maps, args.categories, args.profile, args.output_root,
                     args.verify_per_kind, fault)
        print("Published generation %s: %d outputs (%s)." % (record["generation"], record["outputs"], ", ".join(
            "%s %d" % (k, v["files"]) for k, v in record["totals"].items())))
    except publish.PublishError as error:
        status = 3 if "insufficient destination space" in str(error) else 1
        print("Content conversion failed: %s" % error, file=sys.stderr)
    except (ConversionError, hc.CacheError, gt.TextureError, ra.AnimationError, mf.MediaError) as error:
        status = 1
        print("Content conversion failed: %s" % error, file=sys.stderr)
    except InterruptedError as error:
        status = 4
        print("Content conversion interrupted: %s" % error, file=sys.stderr)
    except OSError as error:
        status = 4
        print("Content conversion I/O failure (errno %s); previous generation unchanged." % error.errno,
              file=sys.stderr)
    if args.record is not None:
        payload = record if record is not None else {"status": status}
        payload = dict(payload, status=status, fault={"after_bytes": args.fault_after_bytes,
                                                      "stage": args.fault_stage, "bytes_written": fault.written})
        with open(external(args.record), "x", encoding="ascii") as stream:
            json.dump(payload, stream, indent=1, sort_keys=True, default=str)
            stream.write("\n")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
