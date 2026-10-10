"""Canonical read-only content inventory of a staged Halo Xbox dump.

Inputs (all private, outside the checkout, never modified):
  --staging DIR        staging.json plus maps/<name>.map (hash-checked)
  --image FILE         the source XISO image (optional; for movies/other files)
  --image-inventory F  the xiso_inventory.py JSON for that image

Outputs (new files only):
  --private FILE       full inventory: per-map tag groups, every bitmap,
                       sound, recorded animation and string/font tag
                       (with tag names), every non-map disc file's container
  --public FILE        sanitized summary: counts and formats only; maps are
                       named by category and ordinal, files by kind and
                       ordinal; no tag names, file names or paths

The inventory is deterministic for identical inputs (no timestamps), so it
can serve as the expected set for future completeness tests.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_convert as cc  # noqa: E402
import gx_texture as gt  # noqa: E402
import halo_cache as hc  # noqa: E402
import media_formats as mf  # noqa: E402
import recorded_animation as ra  # noqa: E402

INVENTORY_VERSION = 1
HEADER_PROBE = 4096


def _strings(cache, instance, root_bytes, block_offset):
    """Count strings/bytes of a unicode or ASCII string-list tag (tag_block of tag_data)."""
    root = cache.root_offset(instance, root_bytes)
    if root is None:
        return 0, 0
    elements = cache.block(root + block_offset, 20, 65535)
    total = 0
    for element in elements:
        data = cache.tag_data(element)
        cache.offset(data["address"], data["size"]) if data["size"] else None
        total += data["size"]
    return len(elements), total


def inventory_map(cache, profile):
    name = cache.header["name"]
    groups = cache.group_counts()
    record = {"name": name, "category": cache.header["category"], "scenario_type": cache.header["scenario_type"],
              "build": cache.header["build"], "declared_bytes": cache.header["declared_bytes"],
              "tag_bytes": cache.header["tag_bytes"], "tag_instances": len(cache.instances), "tag_groups": groups}
    # bitmaps
    bitmaps = []
    by_format = collections.Counter()
    gx_bytes = collections.Counter()
    src_bytes = collections.Counter()
    by_type = collections.Counter()
    observations = collections.Counter()
    plans = cc.plan_textures(cache, name, profile)
    for item in plans:
        fields = item["fields"]
        fmt = gt.FORMAT_NAMES[fields["format"]]
        kind = gt.TYPE_NAMES[fields["type"]]
        by_format[fmt] += 1
        by_type[kind] += 1
        src_bytes[fmt] += item["layout_bytes"]
        gx_bytes[fmt] += item["estimate"] - gt.HWT_HEADER_BYTES
        if max(fields["width"], fields["height"]) > gt.GX_MAX_DIMENSION:
            observations["above_gx_1024"] += 1
        if fields["flags"] & gt.FLAG_LINEAR:
            observations["linear"] += 1
        if not fields["flags"] & gt.FLAG_POW2:
            observations["non_power_of_two"] += 1
        bitmaps.append([cache.name(cache.instances[item["tag"]]), item["index"], fmt, kind, fields["width"],
                        fields["height"], fields["depth"], fields["mipmap_count"], fields["flags"], item["layout_bytes"]])
    group_file_offsets = collections.Counter()
    group_offset = {}
    for instance in cache.by_group("bitm"):
        root = cache.root_offset(instance, 0x6C)
        data = cache.tag_data(root + 0x30)
        group_offset[instance["ordinal"]] = data["file_offset"]
        group_file_offsets["size_0_offset_nonzero" if data["size"] == 0 and data["file_offset"] else
                           "size_0_offset_0" if data["size"] == 0 else "size_nonzero"] += 1
    # Which pixel addressing tiles the map: the bitmap's own offset, or that plus
    # the group's pixel-data file offset (source/cache/xbox_texture_cache.c adds it)?
    raw = sorted((p["pixels_offset"], p["layout_bytes"]) for p in plans)
    summed = sorted((p["pixels_offset"] + group_offset[p["tag"]], p["layout_bytes"]) for p in plans)
    overlaps = lambda spans: sum(a[0] + a[1] > b[0] for a, b in zip(spans, spans[1:]))  # noqa: E731
    group_file_offsets["overlapping_spans_bitmap_offset"] = overlaps(raw)
    group_file_offsets["overlapping_spans_plus_group_offset"] = overlaps(summed)
    record["bitmaps"] = {"groups": groups.get("bitm", 0), "bitmaps": len(bitmaps), "by_format": dict(sorted(by_format.items())),
                         "by_type": dict(sorted(by_type.items())), "source_bytes_by_format": dict(sorted(src_bytes.items())),
                         "gx_bytes_by_format": dict(sorted(gx_bytes.items())), "observations": dict(observations),
                         "group_pixel_data_fields": dict(group_file_offsets)}
    # sounds
    sounds = collections.Counter()
    sound_bytes = collections.Counter()
    sound_frames = collections.Counter()
    sound_rows = collections.defaultdict(lambda: [0, 0, 0])
    for item in cc.plan_sounds(cache, name):
        key = "%s/%dch/%dHz" % (item["codec"], item["channels"], item["rate"])
        sounds[key] += 1
        sound_bytes[key] += item["size"]
        sound_frames[key] += item["frames"]
        row = sound_rows[(item["tag"], key)]
        row[0] += 1
        row[1] += item["size"]
        row[2] += item["frames"]
    record["sounds"] = {"tags": groups.get("snd!", 0), "looping_sound_tags": groups.get("lsnd", 0),
                        "permutations_by_codec": dict(sorted(sounds.items())),
                        "bytes_by_codec": dict(sorted(sound_bytes.items())),
                        "seconds_by_codec": {k: round(sound_frames[k] / int(k.split("/")[2][:-2]), 1)
                                             for k in sorted(sound_frames)},
                        "pcm16_bytes_if_decoded": sum(sound_frames[k] * 2 * int(k.split("/")[1][0])
                                                      for k in sound_frames)}
    # recorded animations
    animations = cc.plan_animations(cache, name)
    versions = collections.Counter((a["source"]["version"], a["source"]["unit_control_version"]) for a in animations)
    record["recorded_animations"] = {
        "count": len(animations), "versions": {"v%d/ucv%d" % k: v for k, v in sorted(versions.items())},
        "stream_bytes": sum(a["source"]["stream_bytes"] for a in animations),
        "events": sum(a["facts"]["events"] for a in animations),
        "canonical_bytes": sum(len(a["data"]) for a in animations),
        "round_trip_exact": sum(a["facts"]["round_trip"] == "exact" for a in animations),
        "length_equals_delta_sum_plus_1": sum(a["source"]["length_equals_delta_sum_plus_1"] for a in animations)}
    # strings and fonts
    ustr = [_strings(cache, i, 12, 0) for i in cache.by_group("ustr")]
    strl = [_strings(cache, i, 12, 0) for i in cache.by_group("str#")]
    record["text"] = {"font_tags": groups.get("font", 0), "unicode_string_list_tags": len(ustr),
                      "unicode_strings": sum(n for n, _ in ustr), "unicode_string_bytes": sum(b for _, b in ustr),
                      "string_list_tags": len(strl), "strings": sum(n for n, _ in strl),
                      "hud_message_text_tags": groups.get("hmt ", 0)}
    record["structure_bsps"] = groups.get("sbsp", 0)
    private = dict(record, bitmap_rows=bitmaps,
                   sound_rows=[[cache.name(cache.instances[t]), k] + v for (t, k), v in sorted(sound_rows.items())],
                   animation_rows=[[a["index"], a["source"]["version"], a["source"]["unit_control_version"],
                                    a["source"]["length_in_ticks"], a["source"]["stream_bytes"], a["facts"]["events"],
                                    a["source"]["stream_sha256"]] for a in animations],
                   font_names=sorted(cache.name(i) for i in cache.by_group("font")))
    return record, private


def disc_files(image, image_inventory):
    """Container facts for every non-map file of the image, read at inventory offsets."""
    rows = []
    with open(image, "rb") as stream:
        before = os.fstat(stream.fileno())
        if before.st_size != image_inventory["image_size"]:
            raise hc.CacheError("image size differs from its inventory")
        for entry in image_inventory["entries"]:
            if entry["kind"] != "file" or entry["path"].startswith("maps/"):
                continue
            if entry["offset"] < 0 or entry["offset"] + entry["size"] > before.st_size:
                raise hc.CacheError("inventory span outside image")
            stream.seek(entry["offset"])
            head = stream.read(min(HEADER_PROBE, entry["size"]))
            facts = mf.sniff(entry["path"], head, entry["size"])
            rows.append(dict(path=entry["path"], bytes=entry["size"], **facts))
        after = os.fstat(stream.fileno())
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise hc.CacheError("image changed while it was read")
    return rows


def classify(row):
    path = row["path"]
    if path.startswith("bink/"):
        return "game-movie"
    if path.startswith("xdemos/videos/") and row["container"] == "bink":
        return "demo-launcher-video"
    if path.startswith("xdemos/"):
        return "demo-launcher-" + ("executable" if row["container"] == "xbe" else "media")
    if row["container"] == "xbe":
        return "game-executable"
    return "other"


def public_summary(maps, files, identity):
    categories = collections.defaultdict(list)
    for record in maps:
        categories[record["category"]].append(record)
    rows = []
    for category in sorted(categories):
        for ordinal, record in enumerate(sorted(categories[category], key=lambda r: r["name"]), 1):
            rows.append({"map": "%s-%02d" % (category, ordinal), **{k: v for k, v in record.items() if k != "name"}})
    totals = collections.Counter()
    formats = collections.Counter()
    gx_bytes = collections.Counter()
    src_bytes = collections.Counter()
    codecs = collections.Counter()
    codec_bytes = collections.Counter()
    groups = collections.Counter()
    for record in maps:
        groups.update(record["tag_groups"])
        totals["bitmaps"] += record["bitmaps"]["bitmaps"]
        totals["sound_tags"] += record["sounds"]["tags"]
        totals["recorded_animations"] += record["recorded_animations"]["count"]
        totals["pcm16_bytes_if_decoded"] += record["sounds"]["pcm16_bytes_if_decoded"]
        formats.update(record["bitmaps"]["by_format"])
        gx_bytes.update(record["bitmaps"]["gx_bytes_by_format"])
        src_bytes.update(record["bitmaps"]["source_bytes_by_format"])
        codecs.update(record["sounds"]["permutations_by_codec"])
        codec_bytes.update(record["sounds"]["bytes_by_codec"])
    kinds = collections.Counter()
    file_rows = []
    for row in sorted(files, key=lambda r: (classify(r), r["path"])):
        kind = classify(row)
        kinds[kind] += 1
        public = {k: v for k, v in row.items() if k != "path"}
        file_rows.append(dict(public, file="%s-%d" % (kind, kinds[kind])))
    return {"format": "halo-wii-content-inventory-public", "inventory_version": INVENTORY_VERSION,
            "identity": identity,
            "maps_by_category": {c: len(v) for c, v in sorted(categories.items())},
            "totals": dict(totals), "tag_groups_all_maps": dict(sorted(groups.items())),
            "bitmaps_by_format": dict(sorted(formats.items())),
            "bitmap_source_bytes_by_format": dict(sorted(src_bytes.items())),
            "bitmap_gx_bytes_by_format": dict(sorted(gx_bytes.items())),
            "sound_permutations_by_codec": dict(sorted(codecs.items())),
            "sound_bytes_by_codec": dict(sorted(codec_bytes.items())),
            "maps": rows, "disc_files": file_rows}


def run(staging_dir, image=None, image_inventory_path=None, profile_name="gx-baseline-v1"):
    profile = cc.PROFILES[profile_name]
    staging_dir, staged, chain = cc.load_staging(staging_dir)
    maps, private_maps = [], []
    for name in sorted(staged):
        cache = hc.open_map(staging_dir / "maps" / (name + ".map"), staged[name]["sha256"])
        if cache.header["name"] != name:
            raise hc.CacheError("map header name differs from its file name")
        record, private = inventory_map(cache, profile)
        record["source_sha256"] = cache.facts["source_sha256"]
        private["source_sha256"] = cache.facts["source_sha256"]
        maps.append(record)
        private_maps.append(private)
        del cache
    after = {name: hc.sha256_file(staging_dir / "maps" / (name + ".map")) == staged[name]["sha256"] for name in staged}
    identity = dict(chain, maps=len(staged), sources_unchanged_after=all(after.values()),
                    converter_profile=profile_name, inventory_modules_sha256=cc.code_identity())
    files = []
    if image is not None:
        image_inventory = json.loads(Path(image_inventory_path).read_bytes())
        image_sha = hc.sha256_file(image)
        if image_sha != image_inventory["image_sha256"] or image_sha != chain["image_sha256"]:
            raise hc.CacheError("image hash differs from its inventory or staging record")
        files = disc_files(image, image_inventory)
        identity["image_sha256_matches_after"] = hc.sha256_file(image) == image_sha
        identity["image_files"] = image_inventory["counts"]["files"]
    private = {"format": "halo-wii-content-inventory-private", "inventory_version": INVENTORY_VERSION,
               "identity": identity, "maps": private_maps, "disc_files": files}
    public = public_summary(maps, files, {k: v for k, v in identity.items() if k != "inventory_modules_sha256"})
    for row in public["maps"]:
        row.pop("source_sha256", None)
    public["inventory_modules_sha256"] = identity["inventory_modules_sha256"]
    return private, public


def write_new(path, record):
    path = cc.external(path)
    with open(path, "x", encoding="ascii", newline="\n") as stream:
        json.dump(record, stream, indent=1, sort_keys=True)
        stream.write("\n")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staging", required=True, type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--image-inventory", type=Path)
    parser.add_argument("--private", required=True, type=Path)
    parser.add_argument("--public", required=True, type=Path)
    args = parser.parse_args(argv)
    if (args.image is None) != (args.image_inventory is None):
        parser.error("--image and --image-inventory go together")
    try:
        for path in (args.private, args.public):
            if cc.external(path).exists():
                raise cc.ConversionError("output already exists")
        private, public = run(args.staging, args.image, args.image_inventory)
        hashes = [write_new(args.private, private), write_new(args.public, public)]
    except (cc.ConversionError, hc.CacheError, gt.TextureError, ra.AnimationError, mf.MediaError) as error:
        print("Inventory failed: %s" % error, file=sys.stderr)
        return 1
    except OSError as error:
        print("Inventory I/O failure (errno %s)." % error.errno, file=sys.stderr)
        return 1
    print("Inventory: %d maps, %d bitmaps, %d disc files; private %s, public %s." % (
        public["identity"]["maps"], public["totals"]["bitmaps"], len(public["disc_files"]), hashes[0][:16],
        hashes[1][:16]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
