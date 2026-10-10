"""Summarise HWI-008D media benchmark runs into sanitized JSON.

Input is either a tools/wii/run_dolphin.py output directory (runtime.json,
run<N>.sd/halo-wii-media/ read-backs, and the profile's SD image, from which
per-frame record files are read when no read-back holds them) or, for a physical Wii, a copy of the SD card's
halo-wii-media directory. For every launch it checks the guest log (BEGIN and
END lines, the run counter, every job's result) and turns each job line into
numbers. From the per-frame records it adds what the log cannot hold:

  video  a playback model with the A/V job's queue: frames are decoded
         back to back into QUEUE_FRAMES slots, playback starts when the
         slots are full, and a frame whose decode ends after its display
         time is late. Reports late frames and the worst lateness, and the
         worst one-second window of decode plus present time.
  av     drift statistics and the least-squares drift slope (ms per minute)
         of the audio clock against the frame times, over presented frames.

The output holds clip names as the job file gave them, numbers and hashes;
no paths. Timings are guest-timebase numbers of whatever ran the DOL: in
Dolphin they are emulator numbers, not hardware performance.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

MEDIA_DIR = "halo-wii-media"
VIDEO_RECORD = struct.Struct(">IIII")  # crc, decode_us, present_us, decoder heap bytes
AV_RECORD = struct.Struct(">IIIiiI")   # frame, time_us, dropped, drift_us, vi_minus_audio_us, decode_us


class SummaryError(ValueError):
    pass


def parse_line(line):
    """'KIND key=value ...' -> (kind, dict) with numbers converted."""
    parts = line.split()
    if not parts:
        return None, {}
    fields = {}
    for part in parts[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        try:
            fields[key] = int(value)
        except ValueError:
            try:
                fields[key] = float(value)
            except ValueError:
                fields[key] = value
    return parts[0], fields


def launches(log_text):
    """Split a bench.log into launches: [(begin, [(kind, fields)], end or None)]."""
    result, current = [], None
    for line in log_text.replace("\r\n", "\n").split("\n"):
        kind, fields = parse_line(line)
        if kind == "BEGIN":
            current = [fields, [], None]
            result.append(current)
        elif kind == "END" and current is not None:
            current[2] = fields
            current = None
        elif kind and current is not None:
            current[1].append((kind, fields))
    return result


QUEUE_FRAMES = 5  # decoded frames the A/V job can hold ahead (six XFB slots, one on screen)


def playback_model(work_us, period_us, queue=QUEUE_FRAMES):
    """Decode-ahead playback with `queue` slots; returns (late frames, worst lateness in us).

    Frame i may start decoding once frame i - queue has been displayed (its
    slot is free); playback starts when the first `queue` frames are decoded
    and frame i is due at start + i * period."""
    finished, ready = 0.0, []
    start = None
    late, worst = 0, 0.0
    for index, work in enumerate(work_us):
        begin = finished
        if index >= queue:
            if start is None:
                start = ready[queue - 1]
            begin = max(begin, start + (index - queue) * period_us)
        finished = begin + work
        ready.append(finished)
    if start is None:
        start = ready[-1] if ready else 0.0
    for index, done in enumerate(ready):
        lateness = done - (start + index * period_us)
        if lateness > 0:
            late += 1
            worst = max(worst, lateness)
    return late, worst


def worst_window(values_us, frames):
    if len(values_us) < frames:
        return sum(values_us) / max(1, len(values_us))
    window = sum(values_us[:frames])
    worst = window
    for i in range(frames, len(values_us)):
        window += values_us[i] - values_us[i - frames]
        worst = max(worst, window)
    return worst / frames


def video_records(data):
    if len(data) % VIDEO_RECORD.size:
        raise SummaryError("video record file is not whole records")
    return [VIDEO_RECORD.unpack_from(data, i) for i in range(0, len(data), VIDEO_RECORD.size)]


def av_records(data):
    if len(data) % AV_RECORD.size:
        raise SummaryError("A/V record file is not whole records")
    return [AV_RECORD.unpack_from(data, i) for i in range(0, len(data), AV_RECORD.size)]


def slope(points):
    """Least-squares slope of (x, y) points."""
    n = len(points)
    if n < 2:
        return 0.0
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    return sum((x - mx) * (y - my) for x, y in points) / sxx if sxx else 0.0


def summarize_video(fields, data):
    records = video_records(data)
    if len(records) != fields.get("frames"):
        raise SummaryError("video records disagree with the log's frame count")
    period = 1e6 / fields["fps"]
    work = [decode + present for _, decode, present, _ in records]
    late, worst = playback_model(work, period)
    return {"model_queue_frames": QUEUE_FRAMES, "model_late_frames": late,
            "model_worst_lateness_ms": round(worst / 1000, 3),
            "worst_1s_window_ms_per_frame": round(worst_window(work, round(fields["fps"])) / 1000, 3),
            "records": len(records)}


def summarize_av(fields, data):
    records = av_records(data)
    if len(records) != fields.get("frames_decoded"):
        raise SummaryError("A/V records disagree with the log's frame count")
    shown = [(t / 1e6, drift / 1000) for _, t, dropped, drift, _, _ in records if not dropped]
    vi = [(t / 1e6, value / 1000) for _, t, dropped, _, value, _ in records if not dropped]
    if len(shown) != fields.get("presented"):
        raise SummaryError("A/V records disagree with the log's presented count")
    drifts = sorted(d for _, d in shown)
    return {"presented": len(shown), "dropped": len(records) - len(shown),
            "drift_ms_median": round(drifts[len(drifts) // 2], 3) if drifts else None,
            "drift_ms_p95_abs": round(sorted(abs(d) for d in drifts)[int(0.95 * (len(drifts) - 1))], 3)
            if drifts else None,
            "drift_slope_ms_per_minute": round(slope(shown) * 60, 4),
            "vi_minus_audio_slope_ms_per_minute": round(slope(vi) * 60, 4),
            "longest_drop_run": longest_run([r[2] for r in records])}


def longest_run(flags):
    best = run = 0
    for flag in flags:
        run = run + 1 if flag else 0
        best = max(best, run)
    return best


def summarize_launch(begin, jobs, end, read):
    """One launch; read(name) returns a per-frame record file's bytes or None."""
    failures = []
    if end is None:
        failures.append("missing END line")
    elif end.get("result") != "pass":
        failures.append("END result %s" % end.get("result"))
    out = []
    for kind, fields in jobs:
        entry = {"kind": kind, **fields}
        if fields.get("result") != "pass":
            failures.append("%s %s: %s" % (kind, fields.get("name"), fields.get("result")))
        name = fields.get("name")
        try:
            if kind == "VIDEO":
                data = read("%s-%d.bin" % (name, begin["run"]))
                if data is None:
                    failures.append("missing records for %s" % name)
                else:
                    entry["derived"] = summarize_video(fields, data)
            elif kind == "AV":
                data = read("%s-%d.bin" % (name, begin["run"]))
                if data is None:
                    failures.append("missing records for %s" % name)
                else:
                    entry["derived"] = summarize_av(fields, data)
        except SummaryError as error:
            failures.append("%s: %s" % (name, error))
        out.append(entry)
    if end is not None and end.get("jobs") != len(jobs):
        failures.append("END job count differs from the job lines")
    return {"run": begin.get("run"), "build": begin.get("build"), "jobs": out,
            "guest": "passed" if not failures else "failed", "failures": failures}


def sd_image_reader(directory):
    """Read-only access to the profile's emulated SD card after the batch."""
    image = Path(directory) / "profile" / "Load" / "WiiSD.raw"
    if not image.is_file():
        return lambda name: None
    import run_dolphin  # noqa: E402  (same directory; read-only FAT32 reader)
    fat = run_dolphin.Fat32(image.read_bytes())
    return lambda name: fat.read("%s/%s" % (MEDIA_DIR, name))


def from_runner(directory):
    """A run_dolphin.py output directory: one launch per run<N>.sd read-back."""
    directory = Path(directory)
    card = sd_image_reader(directory)
    runtime = json.loads((directory / "runtime.json").read_text(encoding="utf-8"))
    result = {"source": "run_dolphin", "host": runtime.get("host"), "identity": runtime.get("identity"),
              "launches": [], "host_lifecycle": [], "os_stability": runtime.get("os_stability", {}).get("observation"),
              "persistence": runtime.get("outcomes", {}).get("persistence")}
    previous = None
    for run in runtime.get("runs", []):
        media = directory / ("run%d.sd" % run["number"]) / MEDIA_DIR
        log = (media / "bench.log").read_text(encoding="ascii")
        parsed = launches(log)
        if not parsed:
            raise SummaryError("run %d: no launch in the guest log" % run["number"])
        begin, jobs, end = parsed[-1]
        summary = summarize_launch(begin, jobs, end,
                                   lambda name, media=media: read_optional(media / name) or card(name))
        summary["launches_in_log"] = len(parsed)
        if previous is not None and begin.get("run") != previous + 1:
            summary["failures"].append("run counter did not advance by one")
            summary["guest"] = "failed"
        previous = begin.get("run")
        result["launches"].append(summary)
        result["host_lifecycle"].append({"run": run["number"], "outcome": run["host"]["outcome"],
                                         "exit_code_hex": run["host"]["exit_code_hex"],
                                         "forced_stop": run["host"]["forced_stop"],
                                         "elapsed_seconds": run["host"]["elapsed_seconds"]})
    return result


def from_card(directory):
    """A copy of a physical SD card's halo-wii-media directory."""
    media = Path(directory)
    parsed = launches((media / "bench.log").read_text(encoding="ascii"))
    return {"source": "sd_card_copy", "launches": [summarize_launch(b, j, e, lambda name: read_optional(media / name))
                                                    for b, j, e in parsed]}


def read_optional(path):
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--runner-output", type=Path, help="tools/wii/run_dolphin.py output directory")
    group.add_argument("--sd-copy", type=Path, help="copy of the SD card's halo-wii-media directory")
    parser.add_argument("--output", type=Path, help="new JSON file (default: stdout)")
    args = parser.parse_args(argv)
    try:
        summary = from_runner(args.runner_output) if args.runner_output else from_card(args.sd_copy)
    except (SummaryError, OSError, KeyError, ValueError) as error:
        print("Summary failed: %s" % error, file=sys.stderr)
        return 1
    text = json.dumps(summary, indent=1, sort_keys=True) + "\n"
    if args.output:
        with open(args.output, "x", encoding="ascii") as stream:
            stream.write(text)
    else:
        sys.stdout.write(text)
    passed = all(launch["guest"] == "passed" for launch in summary["launches"]) and summary["launches"]
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
