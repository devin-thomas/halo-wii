"""Tests of the HWI-008D movie transcode on an authored synthetic image.

No game data and no FFmpeg: a fake runner stands in for FFmpeg and ffprobe
and records every command, so the tests check planning, demo-launcher
exclusion, output checks, determinism and atomic publication.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_publish as cp  # noqa: E402
import movie_convert as mc  # noqa: E402


def bink(frames, width=64, height=48, payload=200, rate=44100, stereo=True):
    """An authored Bink 1 header with one audio track, padded to a fixed size."""
    tracks = 1
    header = b"BIKi" + struct.pack("<10I", 0, frames, 16, 0, width, height, 2997, 100, 0, tracks)
    header += struct.pack("<I", 0) + struct.pack("<HH", rate, 0x2000 if stereo else 0) + struct.pack("<I", 0)
    data = header + bytes(payload)
    return data[:4] + struct.pack("<I", len(data) - 8) + data[8:]


def build_image(directory):
    """A fake image: two game movies, a demo-launcher video and a demo WAV."""
    files = [("bink/b_second.bik", bink(30)), ("bink/a_first.bik", bink(60)),
             ("xdemos/videos/demo.bik", bink(90)),
             ("xdemos/media/sound.wav", b"RIFF" + struct.pack("<I", 36) + b"WAVEfmt " + bytes(24))]
    blob, entries = bytearray(), []
    for path, data in files:
        entries.append({"path": path, "kind": "file", "offset": len(blob), "size": len(data)})
        blob += data + bytes(64)
    image = Path(directory) / "image.iso"
    image.write_bytes(bytes(blob))
    inventory = {"image_sha256": hashlib.sha256(bytes(blob)).hexdigest(), "image_size": len(blob),
                 "entries": entries}
    inventory_path = Path(directory) / "inventory.json"
    inventory_path.write_text(json.dumps(inventory), encoding="ascii")
    tools = Path(directory) / "tools"
    tools.mkdir()
    for name in ("ffmpeg", "ffprobe"):
        (tools / name).write_bytes(b"fake " + name.encode())
    return image, inventory_path, tools, inventory


class FakeTools:
    """Stands in for FFmpeg and ffprobe; output bytes depend only on the input range and arguments."""

    def __init__(self, frames_delta=0, stderr="", audio=True):
        self.commands = []
        self.frames = {}
        self.frames_delta, self.stderr, self.audio = frames_delta, stderr, audio

    def __call__(self, argv, capture_output=True, text=True):
        self.commands.append(list(argv))
        result = type("Result", (), {"returncode": 0, "stdout": "", "stderr": ""})()
        if argv[1] == "-version":
            result.stdout = "%s version authored-test\n" % Path(argv[0]).name
        elif Path(argv[0]).name == "ffmpeg":
            source, target = argv[argv.index("-i") + 1], Path(argv[-1])
            start = int(source.split(",")[3])
            payload = hashlib.sha256((source.split(":")[0] + " ".join(argv[argv.index("-i") + 2:-1])).encode()).digest()
            target.write_bytes(b"\x00\x00\x01\xba" + payload * 8)
            self.frames[target.name] = start
            result.stderr = self.stderr
        else:
            target = Path(argv[-1]).name
            frames = {"game-movie-1.mpg": 60, "game-movie-2.mpg": 30}[target] + self.frames_delta
            streams = [{"codec_type": "video", "codec_name": "mpeg1video", "width": 64, "height": 48,
                        "nb_read_frames": str(frames)}]
            if self.audio:
                streams.append({"codec_type": "audio", "codec_name": "mp2", "sample_rate": "44100", "channels": 2})
            result.stdout = json.dumps({"streams": streams})
        return result


class PlanTests(unittest.TestCase):
    def test_game_movies_converted_and_demo_launcher_excluded(self):
        with tempfile.TemporaryDirectory() as tmp:
            image, _, _, inventory = build_image(tmp)
            convert, excluded = mc.plan(mc.ci.disc_files(image, inventory))
        self.assertEqual([m["label"] for m in convert], ["game-movie-1", "game-movie-2"])
        self.assertEqual([m["path"] for m in convert], ["bink/a_first.bik", "bink/b_second.bik"])
        self.assertEqual([e["file"] for e in excluded], ["demo-launcher-media-1", "demo-launcher-video-1"])
        self.assertTrue(all("ADR-019" in e["reason"] for e in excluded))
        self.assertTrue(all("path" not in e for e in excluded))

    def test_rejects_non_bink_game_movie_and_empty_plan(self):
        with self.assertRaises(mc.MovieError):
            mc.plan([{"path": "bink/x.bik", "container": "unknown", "bytes": 1}])
        with self.assertRaises(mc.MovieError):
            mc.plan([{"path": "xdemos/videos/v.bik", "container": "bink", "bytes": 1}])

    def test_output_paths_are_fat_safe(self):
        for label in ("game-movie-1", "game-movie-5"):
            cp.check_relative(mc.output_path({"label": label}))

    def test_profiles(self):
        self.assertEqual(mc.DEFAULT_PROFILE, "wii-mpeg1-q8-mp2-v1")
        arguments = mc.PROFILES[mc.DEFAULT_PROFILE]["ffmpeg_arguments"]
        for flag in ("-threads", "+bitexact", "-map_metadata"):
            self.assertIn(flag, arguments)
        self.assertEqual(arguments[arguments.index("-q:v") + 1], "8")
        self.assertEqual(arguments[-2:], ["-f", "mpeg"])


class CheckOutputTests(unittest.TestCase):
    movie = {"label": "game-movie-1", "frames": 60, "width": 64, "height": 48,
             "audio_tracks": [{"sample_rate": 44100, "channels": 2}]}

    def streams(self, frames=60, audio=True, codec="mpeg1video", size=(64, 48)):
        streams = [{"codec_type": "video", "codec_name": codec, "width": size[0], "height": size[1],
                    "nb_read_frames": str(frames)}]
        if audio:
            streams.append({"codec_type": "audio", "codec_name": "mp2", "sample_rate": "44100", "channels": 2})
        return streams

    def test_accepts_exact_output(self):
        self.assertEqual(mc.check_output(self.movie, self.streams())["frames"], 60)

    def test_rejects_frame_count_size_codec_and_audio_changes(self):
        for streams in (self.streams(frames=59), self.streams(frames=61), self.streams(audio=False),
                        self.streams(codec="mpeg2video"), self.streams(size=(32, 48))):
            with self.assertRaises(mc.MovieError):
                mc.check_output(self.movie, streams)


class RunTests(unittest.TestCase):
    def run_once(self, tmp, root, tools=None, **kwargs):
        if not (Path(tmp) / "image.iso").exists():
            build_image(tmp)
        tools = tools or FakeTools()
        return mc.run(Path(tmp) / "image.iso", Path(tmp) / "inventory.json", Path(tmp) / "tools",
                      Path(tmp) / root, runner=tools, **kwargs), tools

    def test_publishes_deterministically_and_never_touches_excluded_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            record, tools = self.run_once(tmp, "out-a")
            again, _ = self.run_once(tmp, "out-b")
            reuse, _ = self.run_once(tmp, "out-a")
            image, inventory = Path(tmp) / "image.iso", json.loads((Path(tmp) / "inventory.json").read_text())
            image_sha = hashlib.sha256(image.read_bytes()).hexdigest()
        self.assertEqual(record["generation"], again["generation"])
        self.assertTrue(reuse["reused_existing"])
        self.assertEqual(record["totals"]["movies"], 2)
        self.assertEqual(record["excluded"], ["demo-launcher-media-1", "demo-launcher-video-1"])
        demo = next(e for e in inventory["entries"] if e["path"].startswith("xdemos/videos/"))
        transcodes = [c for c in tools.commands if Path(c[0]).name == "ffmpeg" and c[1] != "-version"]
        self.assertEqual(len(transcodes), 2)
        for command in transcodes:
            self.assertNotIn("start,%d," % demo["offset"], command[command.index("-i") + 1])
        self.assertEqual(image_sha, inventory["image_sha256"])

    def test_manifest_lists_sources_exclusions_and_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            record, _ = self.run_once(tmp, "out")
            current = cp.read_current(Path(tmp) / "out", mc.verify_output)
            leftovers = [p.name for p in (Path(tmp) / "out").iterdir() if p.name.startswith(".staging")]
        manifest = current["manifest"]
        self.assertEqual(manifest["format"], "halo-wii-movie-manifest")
        self.assertEqual([o["path"] for o in manifest["outputs"]],
                         ["movies/game-movie-1.mpg", "movies/game-movie-2.mpg"])
        self.assertEqual([s["frames"] for s in manifest["sources"]], [60, 30])
        self.assertEqual(len(manifest["excluded"]), 2)
        self.assertIn("ffmpeg", manifest["tools"])
        self.assertEqual(manifest["profile"]["name"], "wii-mpeg1-q8-mp2-v1")
        self.assertEqual(leftovers, [])
        self.assertEqual(record["generation"], current["generation"])

    def test_failed_checks_and_faults_keep_the_previous_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_once(tmp, "out")
            root = Path(tmp) / "out"
            before = (root / "CURRENT").read_bytes()
            failures = [dict(tools=FakeTools(frames_delta=-1)), dict(tools=FakeTools(stderr="warning")),
                        dict(tools=FakeTools(audio=False)),
                        dict(fault=cp.FaultInjector(stage="before-current"), profile_name="wii-mpeg1-q6-mp2-v1"),
                        dict(fault=cp.FaultInjector(after_bytes=10), profile_name="wii-mpeg1-q6-mp2-v1")]
            for kwargs in failures:
                with self.assertRaises((mc.MovieError, InterruptedError, OSError)):
                    self.run_once(tmp, "out", **kwargs)
                current = cp.read_current(root, mc.verify_output)
                self.assertEqual((root / "CURRENT").read_bytes(), before)
                self.assertEqual(current["manifest"]["profile"]["name"], "wii-mpeg1-q8-mp2-v1")
            staging = [p.name for p in root.iterdir() if p.name.startswith(".staging")]
        self.assertEqual(staging, [])

    def test_rejects_output_inside_checkout_and_unknown_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            image, inventory, tools, _ = build_image(tmp)
            with self.assertRaises(mc.MovieError):
                mc.run(image, inventory, tools, mc.CHECKOUT / "build" / "movies", runner=FakeTools())
            with self.assertRaises(mc.MovieError):
                mc.run(image, inventory, tools, Path(tmp) / "out", profile_name="mjpeg", runner=FakeTools())

    def test_rejects_image_that_differs_from_inventory(self):
        with tempfile.TemporaryDirectory() as tmp:
            image, inventory, tools, _ = build_image(tmp)
            with open(image, "r+b") as stream:
                stream.seek(10)
                stream.write(b"\xff")
            with self.assertRaises(mc.MovieError):
                mc.run(image, inventory, tools, Path(tmp) / "out", runner=FakeTools())


if __name__ == "__main__":
    unittest.main()
