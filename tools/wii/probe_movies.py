"""Host-only decode probe of the movies on an owned XISO image (read-only).

For every Bink file the image inventory lists, runs the user's own FFmpeg
build directly on the file's byte range inside the image (FFmpeg's subfile
protocol; nothing is extracted), and records:

  - ffprobe stream facts (codec, size, rate, audio) next to the header parse;
  - a full decode of every stream to the null muxer: frames decoded, decode
    errors and host CPU/real time (a host measurement, not a Wii estimate);
  - optionally (--transcode-sample DIR), sizes of candidate Wii-decodable
    encodings of the shortest game movie, written only to that private DIR.

FFmpeg is an external host tool here; no FFmpeg code is linked into any Wii
binary by this probe. The JSON report names files by kind and ordinal and
carries no paths. Exit status 0 when every movie decodes with its header's
frame count and no decoder errors, 1 otherwise.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_convert as cc  # noqa: E402
import content_inventory as ci  # noqa: E402
import halo_cache as hc  # noqa: E402

CANDIDATES = {
    "mpeg1-video_mp2-audio": ["-c:v", "mpeg1video", "-q:v", "4", "-c:a", "mp2", "-b:a", "192k", "-f", "mpeg"],
    "mjpeg-intra_pcm16be-audio": ["-c:v", "mjpeg", "-q:v", "5", "-c:a", "pcm_s16be", "-f", "mov"],
}


def subfile(image, offset, size):
    return "subfile,,start,%d,end,%d,,:%s" % (offset, offset + size, Path(image).resolve().as_posix())


def tool_identity(executable):
    version = subprocess.run([str(executable), "-version"], capture_output=True, text=True, check=True)
    return {"version_line": version.stdout.splitlines()[0], "sha256": hc.sha256_file(executable)}


def probe(ffprobe, source):
    result = subprocess.run([str(ffprobe), "-v", "error", "-show_entries",
                             "stream=codec_name,codec_type,width,height,r_frame_rate,sample_rate,channels,duration",
                             "-of", "json", source], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)["streams"]


def decode(ffmpeg, source):
    started = time.perf_counter()
    result = subprocess.run([str(ffmpeg), "-hide_banner", "-nostdin", "-v", "error", "-stats", "-benchmark",
                             "-i", source, "-map", "0", "-f", "null", "-"], capture_output=True, text=True)
    text = result.stderr
    frames = [int(v) for v in re.findall(r"frame=\s*(\d+)", text)]
    bench = re.search(r"bench: utime=([\d.]+)s stime=([\d.]+)s rtime=([\d.]+)s", text)
    errors = [line for line in text.replace("\r", "\n").splitlines()
              if line and not line.startswith(("frame=", "bench:", "size=")) and "speed=" not in line]
    return {"exit": result.returncode, "frames": frames[-1] if frames else None,
            "host_user_seconds": float(bench.group(1)) if bench else None,
            "host_real_seconds": float(bench.group(3)) if bench else round(time.perf_counter() - started, 3),
            "decoder_messages": len(errors)}


def transcode(ffmpeg, source, directory, name, arguments):
    target = cc.external(directory) / ("%s.out" % name)
    if target.exists():
        raise cc.ConversionError("transcode output already exists")
    started = time.perf_counter()
    result = subprocess.run([str(ffmpeg), "-hide_banner", "-nostdin", "-v", "error", "-i", source] + arguments +
                            [str(target)], capture_output=True, text=True)
    return {"exit": result.returncode, "bytes": target.stat().st_size if target.exists() else None,
            "host_real_seconds": round(time.perf_counter() - started, 3), "arguments": arguments}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--image-inventory", required=True, type=Path)
    parser.add_argument("--ffmpeg-bin", required=True, type=Path, help="directory holding ffmpeg and ffprobe")
    parser.add_argument("--output", required=True, type=Path, help="new private JSON report")
    parser.add_argument("--transcode-sample", type=Path, help="private directory for candidate encodings")
    args = parser.parse_args(argv)
    output = cc.external(args.output)
    if output.exists():
        parser.error("output exists")
    suffix = ".exe" if (args.ffmpeg_bin / "ffmpeg.exe").exists() else ""
    ffmpeg, ffprobe = args.ffmpeg_bin / ("ffmpeg" + suffix), args.ffmpeg_bin / ("ffprobe" + suffix)
    inventory = json.loads(args.image_inventory.read_bytes())
    image_sha = hc.sha256_file(args.image)
    if image_sha != inventory["image_sha256"]:
        print("Image hash differs from its inventory.", file=sys.stderr)
        return 1
    rows = ci.disc_files(args.image, inventory)
    kinds = {}
    movies = []
    ok = True
    entries = {e["path"]: e for e in inventory["entries"]}
    for row in sorted(rows, key=lambda r: (ci.classify(r), r["path"])):
        kind = ci.classify(row)
        kinds[kind] = kinds.get(kind, 0) + 1
        if row["container"] != "bink":
            continue
        entry = entries[row["path"]]
        source = subfile(args.image, entry["offset"], entry["size"])
        streams = probe(ffprobe, source)
        result = decode(ffmpeg, source)
        good = result["exit"] == 0 and result["frames"] == row["frames"] and result["decoder_messages"] == 0
        ok &= good
        movies.append({"file": "%s-%d" % (kind, kinds[kind]), "kind": kind, "bytes": row["bytes"],
                       "header": {k: row[k] for k in ("revision", "frames", "width", "height", "fps",
                                                      "duration_seconds", "audio_tracks")},
                       "ffprobe": streams, "decode": result, "decodes_completely": good,
                       "_source": source})
    record = {"format": "halo-wii-movie-probe", "image_sha256": image_sha,
              "ffmpeg": tool_identity(ffmpeg), "ffprobe": tool_identity(ffprobe),
              "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "scope": "host decode with the user's FFmpeg build; not a Wii decoder or Wii timing",
              "movies": movies}
    if args.transcode_sample is not None:
        games = [m for m in movies if m["kind"] == "game-movie"]
        sample = min(games, key=lambda m: m["header"]["frames"])
        record["transcode_sample"] = {"file": sample["file"], "seconds": sample["header"]["duration_seconds"],
                                      "candidates": {name: transcode(ffmpeg, sample["_source"],
                                                                     args.transcode_sample, name, arguments)
                                                     for name, arguments in CANDIDATES.items()}}
        for result in record["transcode_sample"]["candidates"].values():
            if result["bytes"]:
                result["kbps"] = round(result["bytes"] * 8 / sample["header"]["duration_seconds"] / 1000, 1)
    for movie in movies:
        movie.pop("_source")
    record["all_movies_decode_completely"] = ok
    with open(output, "x", encoding="ascii", newline="\n") as stream:
        json.dump(record, stream, indent=1, sort_keys=True)
        stream.write("\n")
    print("Movies probed: %d, all decode completely: %s." % (len(movies), ok))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
