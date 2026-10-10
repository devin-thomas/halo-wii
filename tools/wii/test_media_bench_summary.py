"""Tests of the HWI-008D benchmark summariser on authored logs and records."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import media_bench_summary as ms  # noqa: E402


def video_line(name, frames, result="pass"):
    return ("VIDEO name=%s codec=mpeg1 frames=%d fps=30.00000 decode_us_mean=10000 sequence_crc=0000abcd "
            "expected=0000abcd result=%s" % (name, frames, result))


def av_line(name, decoded, presented, result="pass"):
    return ("AV name=%s codec=mpeg1 fps=30.00000 frames_decoded=%d presented=%d dropped=%d result=%s"
            % (name, decoded, presented, decoded - presented, result))


def video_bins(works):
    return b"".join(ms.VIDEO_RECORD.pack(i, work - 1000, 1000, 0) for i, work in enumerate(works))


def av_bins(drifts_ms, dropped=()):
    rows = []
    for i, drift in enumerate(drifts_ms):
        flag = 1 if i in dropped else 0
        rows.append(ms.AV_RECORD.pack(i, int(i * 1e6 / 30), flag, 0 if flag else int(drift * 1000), 0, 9000))
    return b"".join(rows)


def log(run, lines, end=True, passed=True):
    text = "BEGIN target=media_bench build=0123456789abcdef run=%d fb=640x480\n" % run
    text += "".join(line + "\n" for line in lines)
    if end:
        text += "END target=media_bench build=0123456789abcdef run=%d jobs=%d passed=%d result=%s\n" % (
            run, len(lines), len(lines) if passed else 0, "pass" if passed else "fail")
    return text


class ParsingTests(unittest.TestCase):
    def test_fields_are_typed(self):
        kind, fields = ms.parse_line("VIDEO name=a frames=3 fps=29.97 sequence_crc=00ab result=pass")
        self.assertEqual(kind, "VIDEO")
        self.assertEqual((fields["frames"], fields["fps"], fields["name"]), (3, 29.97, "a"))

    def test_launch_split(self):
        text = log(1, [video_line("a", 2)]) + log(2, [video_line("a", 2)], end=False)
        parsed = ms.launches(text)
        self.assertEqual(len(parsed), 2)
        self.assertIsNotNone(parsed[0][2])
        self.assertIsNone(parsed[1][2])


class DerivedTests(unittest.TestCase):
    def test_playback_model(self):
        period = 1e6 / 30
        self.assertEqual(ms.playback_model([10000] * 60, period), (0, 0.0))
        # a 100 ms frame is absorbed by five queued frames, a long slow run is not
        self.assertEqual(ms.playback_model([10000] * 10 + [100000] + [10000] * 30, period)[0], 0)
        late, worst = ms.playback_model([10000] * 5 + [40000] * 60, period)
        self.assertGreater(late, 0)
        self.assertGreater(worst, 0)
        self.assertEqual(ms.playback_model([], period), (0, 0.0))

    def test_worst_window(self):
        self.assertEqual(ms.worst_window([1, 1, 5, 5, 1], 2), 5)

    def test_av_slope_and_drops(self):
        drifts = [i * 0.01 for i in range(300)]  # 0.01 ms per frame at 30 fps = 18 ms per minute
        summary = ms.summarize_av({"frames_decoded": 300, "presented": 298}, av_bins(drifts, dropped=(10, 11)))
        self.assertAlmostEqual(summary["drift_slope_ms_per_minute"], 18.0, places=1)
        self.assertEqual((summary["dropped"], summary["longest_drop_run"]), (2, 2))

    def test_record_count_mismatch_rejected(self):
        with self.assertRaises(ms.SummaryError):
            ms.summarize_video({"frames": 3, "fps": 30.0}, video_bins([1000, 1000]))
        with self.assertRaises(ms.SummaryError):
            ms.video_records(b"\0" * 15)


class LaunchTests(unittest.TestCase):
    def write_card(self, directory, text, files):
        media = Path(directory)
        (media / "bench.log").write_text(text, encoding="ascii")
        for name, data in files.items():
            (media / name).write_bytes(data)

    def test_card_copy_passes_and_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.write_card(tmp, log(1, [video_line("clip", 3), av_line("av", 3, 3)]),
                            {"clip-1.bin": video_bins([20000] * 3), "av-1.bin": av_bins([1, 2, 3])})
            summary = ms.from_card(tmp)
            self.assertEqual(summary["launches"][0]["guest"], "passed")
            self.assertEqual(summary["launches"][0]["jobs"][0]["derived"]["model_late_frames"], 0)
        with tempfile.TemporaryDirectory() as tmp:
            self.write_card(tmp, log(1, [video_line("clip", 3, result="fail")], passed=False),
                            {"clip-1.bin": video_bins([20000] * 3)})
            failures = ms.from_card(tmp)["launches"][0]["failures"]
            self.assertTrue(any("fail" in f for f in failures))
        with tempfile.TemporaryDirectory() as tmp:
            self.write_card(tmp, log(1, [video_line("clip", 3)]), {})
            self.assertIn("missing records for clip", ms.from_card(tmp)["launches"][0]["failures"])

    def test_runner_output_checks_run_counter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runs = []
            for number, counter in ((1, 4), (2, 6)):
                media = root / ("run%d.sd" % number) / ms.MEDIA_DIR
                media.mkdir(parents=True)
                self.write_card(media, log(counter, [video_line("clip", 2)]),
                                {"clip-%d.bin" % counter: video_bins([1000, 1000])})
                runs.append({"number": number, "host": {"outcome": "exit_zero", "exit_code_hex": "0x00000000",
                                                         "forced_stop": False, "elapsed_seconds": 1.0}})
            (root / "runtime.json").write_text(json.dumps({"runs": runs, "host": {"os": "test"}}), encoding="ascii")
            summary = ms.from_runner(root)
        self.assertEqual(summary["launches"][0]["guest"], "passed")
        self.assertEqual(summary["launches"][1]["guest"], "failed")
        self.assertIn("run counter did not advance by one", summary["launches"][1]["failures"])


if __name__ == "__main__":
    unittest.main()
