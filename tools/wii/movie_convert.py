"""Deterministic import-time transcode of the game's movies for the Wii (HWI-008D).

Reads the owned XISO image at the byte ranges its inventory lists (FFmpeg's
subfile protocol; nothing is extracted), transcodes every game movie with the
user's own FFmpeg build into MPEG-1 video plus MP2 audio in an MPEG program
stream (default profile wii-mpeg1-q8-mp2-v1), which the Wii decodes with
the vendored pl_mpeg decoder (port/wii/third_party), and publishes one
validated generation under its own output root with content_publish.py:

  movies/game-movie-<n>.mpg    n = ordinal of the game movie by disc path

The disc's demo-launcher videos and audio are out of scope (ADR-019). They are
listed in the manifest as excluded and are never read past their header or
transcoded.

Determinism: FFmpeg runs single-threaded with bit-exact flags and no metadata,
and the manifest records the FFmpeg binary's SHA-256, the profile and the
SHA-256 of this module, with no timestamps or host paths. The same image,
FFmpeg binary and profile give byte-identical outputs and the same
generation. Each output is checked before publication: FFmpeg's own probe
must count exactly the source header's frame count, one MPEG-1 video stream
of the source size and one MP2 stream at the source audio rate and channels.

Use an output root of its own (not the content_convert.py root), since a root
has one CURRENT generation.

Exit status: 0 published (or an identical generation already existed);
1 invalid input or a failed check; 2 usage; 3 insufficient destination space;
4 I/O failure or interruption. In every non-zero case the previously published
generation is left as it was.
"""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_inventory as ci  # noqa: E402
import content_publish as publish  # noqa: E402
import halo_cache as hc  # noqa: E402

CONVERTER = "halo-wii-movie-convert"
CONVERTER_VERSION = "1.0.0"
CHECKOUT = Path(__file__).resolve().parents[2]
MODULES = ("movie_convert.py", "content_publish.py")
GAME_KIND = "game-movie"
EXCLUDED_REASON = "demo-launcher media, out of scope (ADR-019); never converted"
# Same arguments, in the same order, as the HWI-008D benchmark clips.
BITEXACT = ["-threads", "1", "-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact",
            "-map_metadata", "-1"]


def mpeg1_profile(quality):
    return {
        "name": "wii-mpeg1-q%d-mp2-v1" % quality,
        "container": "MPEG-1 program stream (.mpg)",
        "video": {"codec": "mpeg1video", "quality": "-q:v %d" % quality,
                  "frame_rate": "30000/1001 (nearest MPEG-1 rate)", "size": "source size"},
        "audio": {"codec": "mp2", "bitrate": "192k", "rate_and_channels": "source"},
        "wii_decoder": "pl_mpeg (MIT) with the HWI-008D bit-exact speed patch, port/wii/third_party/pl_mpeg",
        "ffmpeg_arguments": [*BITEXACT, "-c:v", "mpeg1video", "-q:v", str(quality), "-c:a", "mp2", "-b:a", "192k",
                             "-f", "mpeg"],
    }


# q:v 8 is the HWI-008D default: the only measured setting whose heaviest
# benchmark passage stays near real time in stock Dolphin. q:v 6 and 4 are
# kept for re-encoding once hardware measurements (HWI-040) show headroom.
PROFILES = {profile["name"]: profile for profile in (mpeg1_profile(8), mpeg1_profile(6), mpeg1_profile(4))}
DEFAULT_PROFILE = "wii-mpeg1-q8-mp2-v1"


class MovieError(ValueError):
    """An invalid input or failed check; the message names movies by kind and ordinal only."""


def external(path):
    resolved = Path(path).resolve()
    if resolved == CHECKOUT or CHECKOUT in resolved.parents:
        raise MovieError("inputs and outputs must be outside the checkout")
    return resolved


def code_identity():
    here = Path(__file__).resolve().parent
    return {name: hashlib.sha256((here / name).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            for name in MODULES}


def plan(rows):
    """Split disc rows into game movies to convert and excluded demo-launcher files.

    Ordinals follow the disc path order within each kind, as in the inventory."""
    convert, excluded, counts = [], [], {}
    for row in sorted(rows, key=lambda r: (ci.classify(r), r["path"])):
        kind = ci.classify(row)
        if kind == GAME_KIND or kind.startswith("demo-launcher"):
            counts[kind] = counts.get(kind, 0) + 1
            label = "%s-%d" % (kind, counts[kind])
        if kind == GAME_KIND:
            if row.get("container") != "bink":
                raise MovieError("%s: not a Bink file" % label)
            convert.append(dict(row, label=label, ordinal=counts[kind]))
        elif kind.startswith("demo-launcher"):
            excluded.append({"file": label, "kind": kind, "container": row.get("container"),
                             "bytes": row["bytes"], "reason": EXCLUDED_REASON})
    if not convert:
        raise MovieError("the image inventory lists no game movies")
    return convert, excluded


def output_path(movie):
    return "movies/%s.mpg" % movie["label"]


def subfile(image, offset, size):
    return "subfile,,start,%d,end,%d,,:%s" % (offset, offset + size, Path(image).resolve().as_posix())


def transcode_argv(ffmpeg, source, profile, target):
    return [str(ffmpeg), "-hide_banner", "-nostdin", "-v", "error", "-i", source,
            *profile["ffmpeg_arguments"], str(target)]


def probe_argv(ffprobe, target):
    return [str(ffprobe), "-v", "error", "-count_frames", "-show_entries",
            "stream=codec_type,codec_name,width,height,nb_read_frames,sample_rate,channels", "-of", "json",
            str(target)]


def check_output(movie, streams):
    """The transcode must keep every frame, the picture size and the audio format."""
    video = [s for s in streams if s.get("codec_type") == "video"]
    audio = [s for s in streams if s.get("codec_type") == "audio"]
    tracks = movie.get("audio_tracks") or []
    problems = []
    if len(video) != 1 or video[0].get("codec_name") != "mpeg1video":
        problems.append("one MPEG-1 video stream expected")
    elif int(video[0].get("nb_read_frames", -1)) != movie["frames"]:
        problems.append("frame count differs from the source header")
    elif (video[0].get("width"), video[0].get("height")) != (movie["width"], movie["height"]):
        problems.append("picture size differs from the source")
    if len(tracks) != 1 or len(audio) != 1 or audio[0].get("codec_name") != "mp2":
        problems.append("one MP2 audio stream expected")
    elif (int(audio[0].get("sample_rate", 0)), audio[0].get("channels")) != \
            (tracks[0]["sample_rate"], tracks[0]["channels"]):
        problems.append("audio rate or channels differ from the source")
    if problems:
        raise MovieError("%s: %s" % (movie["label"], "; ".join(problems)))
    return {"frames": int(video[0]["nb_read_frames"]), "audio_rate": int(audio[0]["sample_rate"]),
            "audio_channels": audio[0]["channels"]}


def tool_identity(executable, runner=subprocess.run):
    result = runner([str(executable), "-version"], capture_output=True, text=True)
    if result.returncode != 0 or not result.stdout:
        raise MovieError("tool did not report a version")
    return {"version_line": result.stdout.splitlines()[0], "sha256": hc.sha256_file(executable)}


def range_sha256(image, offset, size):
    digest = hashlib.sha256()
    with open(image, "rb") as stream:
        stream.seek(offset)
        left = size
        while left:
            block = stream.read(min(left, 1 << 20))
            if not block:
                raise MovieError("image shorter than its inventory")
            digest.update(block)
            left -= len(block)
    return digest.hexdigest()


def run(image, image_inventory, ffmpeg_bin, output_root, profile_name=DEFAULT_PROFILE, fault=None,
        runner=subprocess.run, disk_usage=None):
    if profile_name not in PROFILES:
        raise MovieError("unknown profile")
    profile = PROFILES[profile_name]
    image, output_root = external(image), external(output_root)
    if output_root == image.parent or output_root in image.parents:
        raise MovieError("output root must not contain the image")
    suffix = ".exe" if (Path(ffmpeg_bin) / "ffmpeg.exe").exists() else ""
    ffmpeg, ffprobe = Path(ffmpeg_bin) / ("ffmpeg" + suffix), Path(ffmpeg_bin) / ("ffprobe" + suffix)
    inventory_bytes = Path(image_inventory).read_bytes()
    inventory = json.loads(inventory_bytes)
    image_sha = hc.sha256_file(image)
    if image_sha != inventory.get("image_sha256"):
        raise MovieError("image hash differs from its inventory")
    entries = {e["path"]: e for e in inventory["entries"]}
    convert, excluded = plan(ci.disc_files(image, inventory))
    tools = {"ffmpeg": tool_identity(ffmpeg, runner), "ffprobe": tool_identity(ffprobe, runner)}
    kwargs = {"fault": fault} if disk_usage is None else {"fault": fault, "disk_usage": disk_usage}
    generation = publish.Generation(output_root, **kwargs)
    # Discarded with the staging directory by publish.discard_stale_staging if a run is killed.
    work = output_root / (".staging-work-" + secrets.token_hex(8))
    record = {"movies": {}, "excluded": [e["file"] for e in excluded]}
    started = time.perf_counter()
    try:
        work.mkdir()
        generation.require_space(sum(m["bytes"] for m in convert) // 2)
        outputs, sources = [], []
        for movie in convert:
            entry = entries[movie["path"]]
            source_sha = range_sha256(image, entry["offset"], entry["size"])
            target = work / (movie["label"] + ".mpg")
            t0 = time.perf_counter()
            result = runner(transcode_argv(ffmpeg, subfile(image, entry["offset"], entry["size"]), profile, target),
                            capture_output=True, text=True)
            seconds = round(time.perf_counter() - t0, 3)
            if result.returncode != 0 or result.stderr.strip():
                raise MovieError("%s: FFmpeg transcode failed or reported messages" % movie["label"])
            probed = runner(probe_argv(ffprobe, target), capture_output=True, text=True)
            if probed.returncode != 0:
                raise MovieError("%s: FFmpeg probe of the output failed" % movie["label"])
            facts = check_output(movie, json.loads(probed.stdout)["streams"])
            data = target.read_bytes()
            target.unlink()
            written = generation.write(output_path(movie), data)
            sources.append({"file": movie["label"], "source_path": movie["path"], "bytes": movie["bytes"],
                            "sha256": source_sha, "frames": movie["frames"], "fps": movie["fps"],
                            "width": movie["width"], "height": movie["height"],
                            "audio_tracks": movie["audio_tracks"]})
            outputs.append(dict(written, kind="movie", source=movie["label"], **facts))
            record["movies"][movie["label"]] = {"transcode_seconds": seconds, "bytes": len(data),
                                                "source_bytes": movie["bytes"]}
        manifest = {
            "format": "halo-wii-movie-manifest", "format_version": 1,
            "converter": {"name": CONVERTER, "version": CONVERTER_VERSION, "modules_sha256": code_identity()},
            "profile": profile,
            "profile_sha256": hashlib.sha256(json.dumps(profile, sort_keys=True).encode("ascii")).hexdigest(),
            "tools": tools,
            "source_chain": {"image_sha256": image_sha,
                             "image_inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest()},
            "sources": sources, "excluded": excluded,
            "totals": {"movies": len(outputs), "bytes": sum(o["bytes"] for o in outputs),
                       "source_bytes": sum(s["bytes"] for s in sources)},
            "outputs": outputs,
        }
        result = generation.finish(manifest, verify_output)
    except BaseException:
        generation.abandon()
        raise
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if hc.sha256_file(image) != image_sha:
        raise MovieError("image changed during conversion")
    record.update(result, seconds=round(time.perf_counter() - started, 3), totals=manifest["totals"],
                  tools=tools, image_rehashed_after=True)
    return record


def verify_output(entry, data):
    """Re-read check: an MPEG program stream starts with a pack header."""
    if entry.get("kind") == "movie" and data[:4] != b"\x00\x00\x01\xba":
        raise publish.PublishError("movie output is not an MPEG program stream")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--image-inventory", required=True, type=Path)
    parser.add_argument("--ffmpeg-bin", required=True, type=Path, help="directory holding ffmpeg and ffprobe")
    parser.add_argument("--output-root", required=True, type=Path, help="private publication root for movies")
    parser.add_argument("--profile", default=DEFAULT_PROFILE, choices=sorted(PROFILES))
    parser.add_argument("--record", type=Path, help="new private run-record JSON file")
    parser.add_argument("--fault-after-bytes", type=int, help="simulate a full disk after this many bytes")
    parser.add_argument("--fault-stage", choices=("before-manifest", "before-validate", "before-promote",
                                                  "before-current"), help="simulate an interruption")
    args = parser.parse_args(argv)
    fault = publish.FaultInjector(args.fault_after_bytes, args.fault_stage)
    status, record = 0, None
    try:
        if args.record is not None and external(args.record).exists():
            print("Movie conversion failed: run record already exists", file=sys.stderr)
            return 1
        record = run(args.image, args.image_inventory, args.ffmpeg_bin, args.output_root, args.profile, fault)
        print("Published generation %s: %d movies, %d bytes; %d demo-launcher files excluded." % (
            record["generation"], record["totals"]["movies"], record["totals"]["bytes"], len(record["excluded"])))
    except publish.PublishError as error:
        status = 3 if "insufficient destination space" in str(error) else 1
        print("Movie conversion failed: %s" % error, file=sys.stderr)
    except (MovieError, hc.CacheError) as error:
        status = 1
        print("Movie conversion failed: %s" % error, file=sys.stderr)
    except InterruptedError as error:
        status = 4
        print("Movie conversion interrupted: %s" % error, file=sys.stderr)
    except OSError as error:
        status = 4
        print("Movie conversion I/O failure (errno %s); previous generation unchanged." % error.errno,
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
