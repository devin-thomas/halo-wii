#!/usr/bin/env python3
"""Re-verify every published texture of chosen formats against its Xbox source (HWI-008F).

HWI-008B's texel verification (gx_texture.verify) over a whole published
generation instead of a sample: for each texture of the chosen formats, every
texel of every image is read back from the published HWT1 bytes by the
independent GX decoder (for gx-splitalpha-v1, the CMPR colour with its alpha
plane) and compared with the reference decode of the staged source bitmap.
Any texel outside the documented tolerance fails the run. Work is spread over
worker processes by map and shard; the result is the same whatever --jobs is.

The staging directory and output root are private (never in Git); the JSON
holds counts only (formats, texels, errors), no names or paths.

    python tools/wii/verify_textures.py --staging <dir> --root <content root> \\
        [--formats dxt3 dxt5] [--jobs N] [--json <new file>]

Exit status: 0 every texel within tolerance; 1 a failure or bad input.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_convert as cc  # noqa: E402
import content_publish as publish  # noqa: E402
import gx_texture as gt  # noqa: E402
import halo_cache as hc  # noqa: E402

SHARD_TEXELS = 3_000_000


def texel_count(source):
    return sum(max(source["width"] >> l, 1) * max(source["height"] >> l, 1) * max(source["depth"] >> l, 1)
               for l in range(source["levels"])) * (6 if source["type"] == "cube" else 1)


def image_table(header, gx_format, first=0):
    """The images of a parsed HWT1 header in container order, offsets from first."""
    images, offset = [], first
    for face in range(header["faces"]):
        for level in range(header["levels"]):
            w, h = max(header["width"] >> level, 1), max(header["height"] >> level, 1)
            size = gt.gx_level_bytes(gx_format, w, h)
            for z in range(max(header["depth"] >> level, 1)):
                images.append({"face": face, "level": level, "slice": z, "width": w, "height": h, "offset": offset,
                               "bytes": size})
                offset += size
    return images


def verify_blob(fields, pixels, blob):
    """gx_texture.verify on one published file's own bytes."""
    header, colour = gt.unpack_hwt(blob)
    layout = gt.describe(fields["width"], fields["height"], fields["depth"], fields["type"], fields["format"],
                         fields["flags"], fields["mipmap_count"])
    images = image_table(header, header["gx_format"])
    alpha = header["alpha"]
    data = bytes(colour)
    if alpha is not None:
        alpha = dict(alpha, images=image_table(header, alpha["gx_format"]) if alpha["kind"] == "plane" else [])
        data += bytes(alpha["data"])
    return gt.verify(fields, pixels, data, images, layout, alpha=alpha)


def run_shard(task):
    """One map's share of the chosen textures; returns per-format totals."""
    staging, map_name, map_sha, entries, profile = task
    cache = hc.open_map(Path(staging) / "maps" / (map_name + ".map"), map_sha)
    planned = {(item["tag"], item["index"]): item
               for item in cc.plan_textures(cache, map_name, {"textures": profile})}
    totals = {}
    for path, expected_sha, tag, bitmap in entries:
        item = planned[(tag, bitmap)]
        blob = Path(path).read_bytes()
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise publish.PublishError("published texture differs from its manifest entry")
        pixels = cache.file_span(item["pixels_offset"], item["layout_bytes"])
        result = verify_blob(item["fields"], pixels, blob)
        header = gt.unpack_hwt(blob)[0]
        key = "%s:%s" % (gt.FORMAT_NAMES[item["fields"]["format"]],
                         "rgba8" if header["alpha"] is None else "split-%s" % (
                             gt.ALPHA_GX_NAMES[header["alpha"]["gx_format"]] if header["alpha"]["kind"] == "plane"
                             else "omitted"))
        t = totals.setdefault(key, {"textures": 0, "texels_checked": 0, "texels_exact": 0, "alpha_exact": 0,
                                    "max_channel_error": [0, 0, 0, 0]})
        t["textures"] += 1
        t["texels_checked"] += result["texels_checked"]
        t["texels_exact"] += result["texels_exact"]
        t["alpha_exact"] += result.get("alpha_exact", result["texels_checked"] if result["max_channel_error"][3] == 0
                                       else 0)
        t["max_channel_error"] = [max(a, b) for a, b in zip(t["max_channel_error"], result["max_channel_error"])]
    return totals


def plan(staging, root, formats):
    staging, staged, _ = cc.load_staging(staging)
    current = publish.read_current(Path(root))
    manifest = current["manifest"]
    gen_dir = Path(root) / "generations" / current["generation"]
    profile = manifest["profile"]["textures"]
    by_map = {}
    for entry in manifest["outputs"]:
        source = entry.get("source", {})
        if entry["kind"] == "texture" and source["format"] in formats:
            by_map.setdefault(source["map"], []).append(entry)
    tasks = []
    for map_name in sorted(by_map):
        entries = sorted(by_map[map_name], key=lambda e: e["path"])
        shards, current_shard, texels = [], [], 0
        for entry in entries:
            current_shard.append((str(gen_dir / entry["path"]), entry["sha256"], entry["source"]["tag"],
                                  entry["source"]["bitmap"]))
            texels += texel_count(entry["source"])
            if texels >= SHARD_TEXELS:
                shards.append(current_shard)
                current_shard, texels = [], 0
        if current_shard:
            shards.append(current_shard)
        tasks += [(str(staging), map_name, staged[map_name]["sha256"], shard, profile) for shard in shards]
    return current["generation"], manifest, tasks


def merge(into, totals):
    for key, t in totals.items():
        m = into.setdefault(key, {"textures": 0, "texels_checked": 0, "texels_exact": 0, "alpha_exact": 0,
                                  "max_channel_error": [0, 0, 0, 0]})
        for field in ("textures", "texels_checked", "texels_exact", "alpha_exact"):
            m[field] += t[field]
        m["max_channel_error"] = [max(a, b) for a, b in zip(m["max_channel_error"], t["max_channel_error"])]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staging", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path, help="content_convert.py output root")
    parser.add_argument("--formats", nargs="+", default=["dxt3", "dxt5"], choices=sorted(gt.FORMAT_IDS))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--json", type=Path, help="new JSON file for the counts")
    args = parser.parse_args(argv)
    started = time.perf_counter()
    try:
        if args.json is not None and cc.external(args.json).exists():
            raise cc.ConversionError("JSON output already exists")
        generation, manifest, tasks = plan(args.staging, args.root, set(args.formats))
        totals = {}
        with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for result in pool.map(run_shard, tasks):
                merge(totals, result)
    except (cc.ConversionError, gt.TextureError, hc.CacheError, publish.PublishError, OSError, KeyError) as error:
        print("texture verification failed: %s" % error, file=sys.stderr)
        return 1
    blocks = {}
    for entry in manifest["outputs"]:
        alpha = entry.get("gx", {}).get("alpha") if entry["kind"] == "texture" else None
        if alpha is not None and entry["source"]["format"] in args.formats:
            for case, n in alpha["colour_blocks"].items():
                blocks[case] = blocks.get(case, 0) + n
    report = {"generation": generation, "profile": manifest["profile"]["textures"]["name"],
              "formats": sorted(args.formats), "shards": len(tasks), "jobs": args.jobs, "by_kind": totals,
              "textures": sum(t["textures"] for t in totals.values()),
              "texels_checked": sum(t["texels_checked"] for t in totals.values()),
              "texels_exact": sum(t["texels_exact"] for t in totals.values()),
              "alpha_exact": sum(t["alpha_exact"] for t in totals.values()),
              "max_channel_error": [max(t["max_channel_error"][c] for t in totals.values()) for c in range(4)]
              if totals else [0, 0, 0, 0], "colour_blocks": blocks,
              "seconds": round(time.perf_counter() - started, 1)}
    text = json.dumps(report, indent=1, sort_keys=True)
    if args.json is not None:
        with open(cc.external(args.json), "x", encoding="ascii") as stream:
            stream.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
