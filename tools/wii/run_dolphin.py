"""Run a Wii DOL in stock Dolphin from a fresh isolated profile and record evidence.

One invocation creates a new profile under a new output directory, optionally
stages files into the emulated SD image, performs N cold launches with a timeout,
reads guest SD outputs back, and records five separate outcomes per batch:
build/DOL identity, guest scenario, persistence read-back, host process lifecycle
and observed OS stability. Physical Wii behaviour is never inferred.

Nothing here rewrites a nonzero host exit as success. A specific nonzero code may
be named with --tolerated-host-exit; it is then still recorded as a nonzero exit
and classified "tolerated", never "exit_zero". Forced stops are never tolerated.

No local path is hard-coded: Dolphin, the DOL, build manifest, SD image and staged
files are arguments. The JSON record stores file names and hashes, not the
caller's directories, so it can be reviewed for publication.
"""

import argparse
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shutil
import struct
import subprocess
import sys
import time

SCHEMA_VERSION = 1
TOOL = "tools/wii/run_dolphin.py"

# ---------------------------------------------------------------------------
# DTM authoring (Dolphin's 256-byte DTMHeader followed by 8-byte pad states)
# ---------------------------------------------------------------------------

DTM_MAGIC = b"DTM\x1a"
DTM_HEADER_BYTES = 256
# Offsets in Dolphin's packed DTMHeader (Source/Core/Core/Movie.h).
DTM_GAME_ID, DTM_IS_WII, DTM_CONTROLLERS = 4, 10, 11
DTM_FRAME_COUNT, DTM_INPUT_COUNT, DTM_TICK_COUNT = 13, 21, 237
DEFAULT_TICK_COUNT = 100_000_000_000

# ControllerState: LE16 button bits, then trigger L/R, stick X/Y, C-stick X/Y.
PAD_BUTTONS = {"START": 0x0001, "A": 0x0002, "B": 0x0004, "X": 0x0008, "Y": 0x0010,
               "Z": 0x0020, "UP": 0x0040, "DOWN": 0x0080, "LEFT": 0x0100,
               "RIGHT": 0x0200, "L": 0x0400, "R": 0x0800}
PAD_CONNECTED = 0x4000


def game_id_for_executable(name):
    """Dolphin's ID for an ELF/DOL is "ID-" + file stem, compared on 6 bytes."""
    stem = PureWindowsPath(name).name
    if "." in stem:
        stem = stem[:stem.rindex(".")]
    return ("ID-" + stem).encode("utf-8")[:6]


def pad_entries(script):
    """Expand an input script into 8-byte GC pad states, one per controller poll."""
    if not isinstance(script, dict) or script.get("port", 1) != 1:
        raise ValueError("input script must be an object driving GC port 1")
    entries = []
    for step in script.get("steps", []):
        polls = step.get("polls")
        if not isinstance(polls, int) or polls <= 0:
            raise ValueError(f"step {step.get('label')!r}: polls must be a positive integer")
        bits = PAD_CONNECTED
        for button in step.get("buttons", []):
            if button not in PAD_BUTTONS:
                raise ValueError(f"step {step.get('label')!r}: unknown button {button!r}")
            bits |= PAD_BUTTONS[button]
        values = [step.get("trigger_l", 0), step.get("trigger_r", 0),
                  *step.get("stick", (0x80, 0x80)), *step.get("cstick", (0x80, 0x80))]
        if len(values) != 6 or any(not isinstance(v, int) or not 0 <= v <= 255 for v in values):
            raise ValueError(f"step {step.get('label')!r}: analog values must be six bytes")
        entries.append(struct.pack("<H6B", bits, *values) * polls)
    if not entries:
        raise ValueError("input script has no steps")
    return b"".join(entries)


def author_dtm(game_id, body, tick_count=DEFAULT_TICK_COUNT):
    """A Wii-mode, boot-start DTM with one standard GC controller on port 1."""
    if len(body) % 8:
        raise ValueError("pad body must be whole 8-byte states")
    header = bytearray(DTM_HEADER_BYTES)
    header[0:4] = DTM_MAGIC
    header[DTM_GAME_ID:DTM_GAME_ID + 6] = game_id[:6].ljust(6, b"\0")
    header[DTM_IS_WII] = 1
    header[DTM_CONTROLLERS] = 0x01
    struct.pack_into("<Q", header, DTM_FRAME_COUNT, len(body) // 8)
    struct.pack_into("<Q", header, DTM_INPUT_COUNT, len(body) // 8)
    struct.pack_into("<Q", header, DTM_TICK_COUNT, tick_count)
    return bytes(header) + body


def dtm_game_id(data):
    if len(data) < DTM_HEADER_BYTES or data[:4] != DTM_MAGIC:
        raise ValueError("not a DTM movie")
    return data[DTM_GAME_ID:DTM_GAME_ID + 6].rstrip(b"\0")


def check_movie_matches(data, executable_name):
    """Dolphin silently ignores a movie whose game ID differs; refuse it instead."""
    expected = game_id_for_executable(executable_name)
    found = dtm_game_id(data)
    if found != expected.rstrip(b"\0"):
        raise ValueError(f"DTM game ID {found!r} does not match {expected!r} derived from the DOL name")
    if data[DTM_IS_WII] != 1:
        raise ValueError("DTM is not marked as a Wii recording")


# ---------------------------------------------------------------------------
# Isolated stock-limit profile
# ---------------------------------------------------------------------------

STOCK_REQUIRED = {("Core", "EnableCheats"): "False", ("Core", "OverclockEnable"): "False",
                  ("Core", "Overclock"): "1.0", ("Core", "VIOverclockEnable"): "False",
                  ("Core", "RAMOverrideEnable"): "False", ("Core", "EmulationSpeed"): "1.0"}


def profile_files(backend="D3D", efb_access=False, dump_frames=False):
    """Config files for a new Wii-mode profile: stock clock/memory, no cheats."""
    dolphin = {
        "Core": [("CPUThread", "False"), ("EnableCheats", "False"), ("OverclockEnable", "False"),
                 ("Overclock", "1.0"), ("VIOverclockEnable", "False"), ("RAMOverrideEnable", "False"),
                 ("EmulationSpeed", "1.0"), ("GFXBackend", backend), ("SIDevice0", "6"),
                 ("SIDevice1", "0"), ("SIDevice2", "0"), ("SIDevice3", "0"), ("WiiSDCard", "True"),
                 ("WiiSDCardAllowWrites", "True"), ("WiiSDCardEnableFolderSync", "False")],
        "Interface": [("ConfirmStop", "False"), ("UsePanicHandlers", "False")],
        "Movie": [("DumpFrames", str(dump_frames)), ("DumpFramesSilent", "True")],
        "Analytics": [("PermissionAsked", "True"), ("Enabled", "False")],
    }
    gfx = {"Settings": [("InternalResolution", "1")]}
    if dump_frames:
        gfx["Settings"].append(("DumpFramesAsImages", "True"))
    if efb_access:
        gfx["Hacks"] = [("EFBAccessEnable", "True")]
    wiimotes = {name: [("Source", "0")] for name in
                ("Wiimote1", "Wiimote2", "Wiimote3", "Wiimote4", "BalanceBoard")}
    render = lambda sections: "".join(f"[{s}]\n" + "".join(f"{k} = {v}\n" for k, v in items)
                                      for s, items in sections.items())
    return {"Dolphin.ini": render(dolphin), "GFX.ini": render(gfx), "WiimoteNew.ini": render(wiimotes)}


def parse_ini(text):
    values, section = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif "=" in line and section is not None:
            key, value = line.split("=", 1)
            values[(section, key.strip())] = value.strip()
    return values


def check_stock_limits(files):
    """Refuse a profile that is not at stock Wii limits."""
    core = parse_ini(files["Dolphin.ini"])
    wrong = [f"{s}.{k}={core.get((s, k))!r}" for (s, k), v in STOCK_REQUIRED.items() if core.get((s, k)) != v]
    if wrong:
        raise ValueError("profile is not stock-limit: " + ", ".join(wrong))


# ---------------------------------------------------------------------------
# Read-only FAT32 reader for the emulated SD image
# ---------------------------------------------------------------------------

class Fat32:
    """Minimal read-only FAT32 reader; never writes to the image."""

    def __init__(self, data):
        self.data = data
        if len(data) < 512 or data[510:512] != b"\x55\xaa":
            raise ValueError("SD image has no boot signature")
        self.bps = struct.unpack_from("<H", data, 11)[0]
        self.spc = data[13]
        self.reserved = struct.unpack_from("<H", data, 14)[0]
        self.fats = data[16]
        self.spfat = struct.unpack_from("<I", data, 36)[0]
        self.root = struct.unpack_from("<I", data, 44)[0]
        if self.bps not in (512, 1024, 2048, 4096) or not self.spc or not self.fats or not self.spfat:
            raise ValueError("SD image is not FAT32")
        self.cluster_bytes = self.bps * self.spc
        self.data_start = (self.reserved + self.fats * self.spfat) * self.bps

    def _chain(self, cluster):
        seen = set()
        while 2 <= cluster < 0x0FFFFFF8:
            if cluster in seen:
                raise ValueError("FAT cluster chain loops")
            seen.add(cluster)
            start = self.data_start + (cluster - 2) * self.cluster_bytes
            if start + self.cluster_bytes > len(self.data):
                raise ValueError("FAT cluster outside image")
            yield self.data[start:start + self.cluster_bytes]
            cluster = struct.unpack_from("<I", self.data, self.reserved * self.bps + cluster * 4)[0] & 0x0FFFFFFF

    def entries(self, cluster):
        long_name = []
        for block in self._chain(cluster):
            for offset in range(0, len(block), 32):
                entry = block[offset:offset + 32]
                if entry[0] == 0:
                    return
                if entry[0] == 0xE5:
                    long_name = []
                    continue
                if entry[11] == 0x0F:
                    chars = entry[1:11] + entry[14:26] + entry[28:32]
                    long_name.insert(0, chars.decode("utf-16-le").split("\0")[0].replace("￿", ""))
                    continue
                if entry[11] & 0x08:  # volume label
                    long_name = []
                    continue
                base, ext = entry[:8].decode("latin-1").rstrip(), entry[8:11].decode("latin-1").rstrip()
                name = "".join(long_name) if long_name else base + ("." + ext if ext else "")
                long_name = []
                first = struct.unpack_from("<H", entry, 20)[0] << 16 | struct.unpack_from("<H", entry, 26)[0]
                yield name, entry[11], first, struct.unpack_from("<I", entry, 28)[0]

    def find(self, path):
        cluster, found = self.root, None
        for part in [p for p in path.split("/") if p]:
            found = next((e for e in self.entries(cluster) if e[0].lower() == part.lower()), None)
            if found is None:
                return None
            cluster = found[2]
        return found

    def read(self, path):
        """File bytes, or None when the path is absent."""
        found = self.find(path)
        if found is None:
            return None
        if found[1] & 0x10:
            raise ValueError(f"{path} is a directory")
        if found[3] == 0:
            return b""
        data = b"".join(self._chain(found[2]))
        if len(data) < found[3]:
            raise ValueError(f"{path}: cluster chain shorter than file size")
        return data[:found[3]]


def sd_relative(path):
    """'sd:/a/b.txt' or '/a/b.txt' -> 'a/b.txt'; rejects traversal."""
    text = path[3:] if path.startswith("sd:") else path
    raw = [p for p in text.replace("\\", "/").split("/") if p]
    parts = PurePosixPath("/" + text.lstrip("/")).parts[1:]
    if not parts or any(p in ("..", ".") for p in raw):
        raise ValueError(f"invalid SD path {path!r}")
    return "/".join(parts)


# ---------------------------------------------------------------------------
# SD staging through mtools in WSL (only used when files are supplied)
# ---------------------------------------------------------------------------

def wsl_path(path):
    """Windows drive path -> /mnt/<drive>/... for WSL."""
    windows = PureWindowsPath(path)
    if not windows.drive or len(windows.drive) != 2 or windows.drive[1] != ":":
        raise ValueError(f"staging needs a local drive path: {path}")
    return "/mnt/" + windows.drive[0].lower() + "/" + "/".join(windows.parts[1:])


def staging_commands(image, staged, existing_dirs):
    """mtools argv lists: create missing directories, then copy without clobbering."""
    commands, made = [], set(existing_dirs)
    for _, destination in staged:
        parts = sd_relative(destination).split("/")
        for depth in range(1, len(parts)):
            directory = "/".join(parts[:depth])
            if directory.lower() not in made:
                commands.append(["mmd", "-i", wsl_path(image), "::/" + directory])
                made.add(directory.lower())
    for source, destination in staged:
        commands.append(["mcopy", "-n", "-i", wsl_path(image), wsl_path(source), "::/" + sd_relative(destination)])
    return commands


def parse_stage(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("--stage needs LOCAL=sd:/path")
    local, destination = value.rsplit("=", 1)
    sd_relative(destination)
    return Path(local), destination


# ---------------------------------------------------------------------------
# Host process helpers (Windows)
# ---------------------------------------------------------------------------

def parse_tasklist(text, image_name):
    """Count CSV rows from `tasklist /FO CSV /NH` naming the image."""
    count = 0
    for line in text.splitlines():
        if line.startswith('"') and line.split('","')[0].strip('"').lower() == image_name.lower():
            count += 1
    return count


def running_count(image_name):
    result = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("tasklist failed; cannot establish exclusive Dolphin ownership")
    return parse_tasklist(result.stdout, image_name)


def exit_fields(status):
    unsigned = status & 0xFFFFFFFF
    signed = unsigned - 0x100000000 if unsigned & 0x80000000 else unsigned
    return {"exit_code_hex": f"0x{unsigned:08x}", "exit_code_signed": signed}


def host_outcome(status, forced_stop, tolerated):
    if forced_stop:
        return "forced_stop_timeout"
    if status & 0xFFFFFFFF == 0:
        return "exit_zero"
    if status & 0xFFFFFFFF in tolerated:
        return "nonzero_exit_tolerated"
    return "nonzero_exit"


# ---------------------------------------------------------------------------
# OS stability: read-only Windows event log query
# ---------------------------------------------------------------------------

EVENT_QUERY = r"""
$ErrorActionPreference = 'Stop'
$start = [DateTime]::Parse('{start}').ToLocalTime(); $end = [DateTime]::Parse('{end}').ToLocalTime()
function Query($filter) {{
  try {{ @(Get-WinEvent -FilterHashtable $filter) }}
  catch {{ if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') {{ @() }} else {{ throw }} }}
}}
$system = Query @{{LogName='System'; Id=41,1001,6005,6006,6008; StartTime=$start; EndTime=$end}}
$app = Query @{{LogName='Application'; Id=1000,1002; StartTime=$start; EndTime=$end}} |
  Where-Object {{ $_.Message -match 'Dolphin' }}
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
[pscustomobject]@{{
  last_boot_utc = $boot
  system = @($system | ForEach-Object {{ [pscustomobject]@{{ id = $_.Id; provider = $_.ProviderName;
    time_utc = $_.TimeCreated.ToUniversalTime().ToString('o') }} }})
  application = @($app | ForEach-Object {{ [pscustomobject]@{{ id = $_.Id; provider = $_.ProviderName;
    time_utc = $_.TimeCreated.ToUniversalTime().ToString('o'); message = $_.Message }} }})
}} | ConvertTo-Json -Depth 4
"""

def parse_utc(text):
    """ISO-8601 with 'Z' or an offset (PowerShell 'o' adds 7 fractional digits)."""
    text = re.sub(r"(\.\d{6})\d+", r"\1", text.replace("Z", "+00:00"))
    return datetime.datetime.fromisoformat(text)


BUGCHECK_PROVIDERS = ("microsoft-windows-wer-systemerrorreporting", "bugcheck")


def sanitize_application_event(event):
    """Keep only the faulting image/module names and exception code, never paths."""
    message = event.get("message") or ""
    fields = {}
    for key, pattern in (("faulting_application", r"Faulting application name:\s*([^\s,]+)"),
                         ("faulting_module", r"Faulting module name:\s*([^\s,]+)"),
                         ("exception_code", r"Exception code:\s*(0x[0-9a-fA-F]+)")):
        match = re.search(pattern, message)
        if match:
            fields[key] = match.group(1)
    return {"id": event.get("id"), "provider": event.get("provider"), "time_utc": event.get("time_utc"), **fields}


def classify_events(result, window_start_utc):
    """Separate OS instability from Dolphin application fault/hang reports."""
    system = [{"id": e.get("id"), "provider": e.get("provider"), "time_utc": e.get("time_utc")}
              for e in result.get("system") or []]
    application = [sanitize_application_event(e) for e in result.get("application") or []]
    instability = []
    for event in system:
        provider = (event["provider"] or "").lower()
        if event["id"] == 41:
            instability.append(dict(event, meaning="kernel_power_unexpected_restart"))
        elif event["id"] == 6008:
            instability.append(dict(event, meaning="unexpected_shutdown"))
        elif event["id"] == 1001 and provider in BUGCHECK_PROVIDERS:
            instability.append(dict(event, meaning="bugcheck"))
        elif event["id"] in (6005, 6006):
            instability.append(dict(event, meaning="event_log_service_start_or_stop_reboot"))
    boot = result.get("last_boot_utc")
    rebooted = bool(boot and parse_utc(boot) > parse_utc(window_start_utc))
    if rebooted:
        instability.append({"meaning": "boot_after_window_start", "time_utc": boot})
    return {
        "observation": "instability_observed" if instability else "no_instability_events_observed",
        "instability_events": instability,
        "system_events_matched": system,
        "dolphin_application_events": application,
        "last_boot_utc": boot,
        "limits": "Event-log query over the batch window only; absence of events is not a guarantee of stability.",
    }


def query_os_events(window_start_utc, window_end_utc):
    script = EVENT_QUERY.format(start=window_start_utc, end=window_end_utc)
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return {"observation": "unmeasured", "error": "event log query failed",
                "stderr_first_line": (result.stderr.strip().splitlines() or [""])[0][:200]}
    parsed = json.loads(result.stdout) if result.stdout.strip() else {}
    for key in ("system", "application"):
        if isinstance(parsed.get(key), dict):
            parsed[key] = [parsed[key]]
    return classify_events(parsed, window_start_utc)


# ---------------------------------------------------------------------------
# Guest scenarios
# ---------------------------------------------------------------------------

PROBE_LOG, PROBE_SENTINEL = "halo-wii-probe/probe.log", "halo-wii-probe/sentinel.txt"
PROBE_BEGIN = re.compile(r"BEGIN build=([0-9a-f]+) previous_runs=(\d+) video=(\d+)x(\d+) mode=(\d+)$")
PROBE_ARENAS = re.compile(r"ARENAS mem1=(0x[0-9a-f]+)\.\.(0x[0-9a-f]+) bytes=(\d+) "
                          r"mem2=(0x[0-9a-f]+)\.\.(0x[0-9a-f]+) bytes=(\d+)$")
PROBE_END = re.compile(r"END build=([0-9a-f]+) frames=(\d+) ticks=(\d+) connected=([0-9a-f]+) "
                       r"activity=([0-9a-f]+) exit=(\w+) storage=(-?\d+) abi=(\d+)$")
PROBE_SENTINEL_TEXT = re.compile(r"halo-wii-probe-v1 (\d+)\n$")


def probe_sentinel_count(data):
    if data is None:
        return 0
    match = PROBE_SENTINEL_TEXT.fullmatch(data.decode("ascii", "replace"))
    if not match:
        raise ValueError("existing probe sentinel is not valid; preserved, not reset")
    return int(match.group(1))


def check_probe(build_id, run_number, initial_count, previous_log, files, expect_exit="pad_start"):
    """Evaluate one cold launch of the asset-free probe from its SD outputs.

    Returns (guest, persistence) dictionaries, each with its own failure list.
    """
    guest = {"failures": [], "observations": {}}
    persistence = {"failures": []}
    log_bytes, sentinel = files.get(PROBE_LOG), files.get(PROBE_SENTINEL)
    expected_count = initial_count + run_number
    persistence["expected_sentinel"] = expected_count
    if sentinel is None:
        persistence["failures"].append("sentinel_missing")
    else:
        persistence["sentinel_text"] = sentinel.decode("ascii", "replace")
        if sentinel != f"halo-wii-probe-v1 {expected_count}\n".encode():
            persistence["failures"].append("sentinel_did_not_advance_across_cold_process")
    if log_bytes is None:
        guest["failures"].append("guest_log_missing")
        persistence["failures"].append("guest_log_missing")
        return guest, persistence
    log = log_bytes.decode("ascii", "replace").replace("\r\n", "\n")
    preserved = log.startswith(previous_log)
    persistence["prior_log_preserved"] = preserved
    if not preserved:
        persistence["failures"].append("prior_guest_log_not_preserved")
    lines = (log[len(previous_log):] if preserved else log).splitlines()
    guest["report"] = lines
    begins = [i for i, line in enumerate(lines) if line.startswith("BEGIN ")]
    if len(begins) != 1:
        guest["failures"].append(f"expected_one_BEGIN_found_{len(begins)}")
        return guest, persistence
    obs = guest["observations"]
    begin = PROBE_BEGIN.match(lines[begins[0]])
    if not begin:
        guest["failures"].append("BEGIN_unparsed")
    else:
        obs["build_id"] = begin.group(1)
        obs["previous_runs"] = int(begin.group(2))
        obs["video"] = {"width": int(begin.group(3)), "height": int(begin.group(4)), "vi_tv_mode": int(begin.group(5))}
        if begin.group(1) != build_id:
            guest["failures"].append("build_id_mismatch")
        if int(begin.group(2)) != expected_count - 1:
            persistence["failures"].append("guest_previous_runs_mismatch")
        if not (int(begin.group(3)) and int(begin.group(4))):
            guest["failures"].append("video_mode_empty")
    arenas = [PROBE_ARENAS.match(line) for line in lines if line.startswith("ARENAS ")]
    if len(arenas) != 1 or not arenas[0]:
        guest["failures"].append("ARENAS_missing_or_unparsed")
    else:
        a = arenas[0]
        obs["arenas"] = {"mem1": [a.group(1), a.group(2), int(a.group(3))],
                         "mem2": [a.group(4), a.group(5), int(a.group(6))]}
        if not (int(a.group(3)) and int(a.group(6))):
            guest["failures"].append("arena_empty")
    abi = [line for line in lines if line.startswith("ABI ")]
    obs["abi_lines"] = len(abi)
    if not abi or any(line.startswith("ABI FAIL") for line in abi):
        guest["failures"].append("abi_report_missing_or_failed")
    end = PROBE_END.match(lines[-1]) if lines else None
    if not end:
        guest["failures"].append("END_missing_or_unparsed")
        return guest, persistence
    frames, ticks = int(end.group(2)), int(end.group(3))
    obs.update(frames=frames, ticks_30hz=ticks, connected=end.group(4), activity=end.group(5),
               exit=end.group(6), storage=int(end.group(7)), abi=int(end.group(8)))
    if end.group(1) != build_id:
        guest["failures"].append("END_build_id_mismatch")
    ratio = ticks / frames if frames else 0.0
    obs["ticks_per_frame"] = round(ratio, 4)
    if frames < 30 or not 0.4 <= ratio <= 0.65:
        guest["failures"].append("timer_not_consistent_with_vi_frames")
    if not int(end.group(4), 16) & 1:
        guest["failures"].append("pad0_not_connected")
    if not int(end.group(5), 16) & 1:
        guest["failures"].append("pad0_activity_not_observed")
    if end.group(6) != expect_exit:
        guest["failures"].append(f"exit_not_{expect_exit}")
    if int(end.group(7)) != 1:
        persistence["failures"].append("guest_storage_not_ready")
    if int(end.group(8)) != 0:
        guest["failures"].append("abi_nonzero")
    return guest, persistence


# ---------------------------------------------------------------------------
# Batch execution
# ---------------------------------------------------------------------------

def staged_file_failures(staged_hashes, read):
    """Staged inputs must survive every launch byte-for-byte."""
    failures = []
    for path, expected in staged_hashes.items():
        data = read.get(path)
        if data is None:
            failures.append(f"staged_file_missing:{path}")
        elif hashlib.sha256(data).hexdigest() != expected:
            failures.append(f"staged_file_changed:{path}")
    return failures


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


def combine(results, requested):
    """Batch result: any failure or missing run fails; all passed passes."""
    if "failed" in results or len(results) != requested:
        return "failed"
    return "passed" if all(r == "passed" for r in results) else "not_evaluated"


def overall_exit(record):
    """0 pass; 1 guest/persistence failure; 3 host lifecycle not accepted; 4 OS instability."""
    if record["os_stability"].get("observation") == "instability_observed":
        return 4
    if any(r["guest"]["result"] == "failed" or r["persistence"]["result"] == "failed" for r in record["runs"]):
        return 1
    if len(record["runs"]) != record["requested_runs"]:
        return 1
    if any(r["host"]["outcome"] not in ("exit_zero", "nonzero_exit_tolerated") for r in record["runs"]):
        return 3
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dolphin", type=Path, required=True, help="stock Dolphin.exe")
    parser.add_argument("--dolphin-sha256", help="refuse to run a different Dolphin executable")
    parser.add_argument("--dol", type=Path, required=True, help="guest DOL to --exec")
    parser.add_argument("--build-info", type=Path, help="build manifest whose artifacts must hash-match the DOL")
    parser.add_argument("--out", type=Path, required=True, help="new output directory (must not exist)")
    parser.add_argument("--runs", type=int, default=2, help="cold launches sharing one profile")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds before a launch is force-stopped")
    parser.add_argument("--backend", default="D3D")
    parser.add_argument("--efb-access", action="store_true", help="enable CPU EFB peeks (GFX Hacks)")
    parser.add_argument("--dump-frames", action="store_true", help="dump rendered frames as PNG")
    movie = parser.add_mutually_exclusive_group()
    movie.add_argument("--input-script", type=Path, help="JSON pad script to author into a DTM")
    movie.add_argument("--movie", type=Path, help="existing DTM; its game ID must match the DOL name")
    parser.add_argument("--sd-image", type=Path, help="FAT32 image copied (never modified) into the profile")
    parser.add_argument("--stage", type=parse_stage, action="append", default=[],
                        help="LOCAL=sd:/path, copied into the profile's SD image with mtools in WSL")
    parser.add_argument("--wsl-distro", default="Debian")
    parser.add_argument("--read-back", action="append", default=[], help="SD path to read after each launch")
    parser.add_argument("--scenario", choices=("none", "probe"), default="none")
    parser.add_argument("--probe-exit", default="pad_start", help="expected probe END exit reason")
    parser.add_argument("--tolerated-host-exit", action="append", default=[],
                        help="nonzero host exit code (hex) disclosed as tolerated, e.g. 0xc0000409")
    parser.add_argument("--event-settle", type=float, default=10.0,
                        help="seconds to wait after the last launch before querying event logs")
    args = parser.parse_args(argv)
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.stage and not args.sd_image:
        parser.error("--stage needs --sd-image (Dolphin only creates its image during a launch)")
    if args.scenario == "probe" and not args.build_info:
        parser.error("--scenario probe needs --build-info for the expected build ID")
    return args


def main(argv=None):
    args = parse_args(argv)
    out = args.out.resolve()
    if out.exists():
        print("output directory must be new", file=sys.stderr)
        return 2
    exe, dol = args.dolphin.resolve(), args.dol.resolve()
    exe_hash, dol_hash = sha256(exe), sha256(dol)
    if args.dolphin_sha256 and exe_hash != args.dolphin_sha256.lower():
        print(f"Dolphin hash {exe_hash} does not match the pinned value", file=sys.stderr)
        return 2
    tolerated = {int(code, 16) & 0xFFFFFFFF for code in args.tolerated_host_exit}
    identity = {"dol": dol.name, "dol_sha256": dol_hash, "dol_bytes": dol.stat().st_size,
                "dolphin_sha256": exe_hash, "dolphin_executable": exe.name}
    build_id = None
    if args.build_info:
        info = json.loads(args.build_info.read_text(encoding="utf-8"))
        artifact = info.get("artifacts", {}).get(dol.name, {})
        if artifact.get("sha256") != dol_hash:
            print("DOL hash does not match the build manifest", file=sys.stderr)
            return 2
        build_id = info.get("build_id")
        identity.update(build_id=build_id, source_commit=info.get("source_commit"),
                        source_dirty=info.get("source_dirty"), scope=info.get("scope"),
                        compiler=info.get("compiler"), auto_exit_frames=info.get("probe_auto_exit_frames"))
    files = profile_files(args.backend, args.efb_access, args.dump_frames)
    check_stock_limits(files)
    if running_count(exe.name):
        print("another Dolphin process is running; exclusive ownership required", file=sys.stderr)
        return 2

    profile = out / "profile"
    (profile / "Config").mkdir(parents=True)
    (profile / "Load").mkdir()
    for name, text in files.items():
        (profile / "Config" / name).write_text(text, encoding="ascii")
    movie_path, input_record = None, {"kind": "none"}
    if args.input_script:
        script = json.loads(args.input_script.read_text(encoding="utf-8"))
        data = author_dtm(game_id_for_executable(dol.name), pad_entries(script))
        movie_path = out / "input.dtm"
        movie_path.write_bytes(data)
        input_record = {"kind": "authored DTM, standard GC pad port 1; virtual input",
                        "script": args.input_script.name, "script_sha256": sha256(args.input_script),
                        "steps": [[s.get("label"), s["polls"]] for s in script["steps"]]}
    elif args.movie:
        data = args.movie.read_bytes()
        check_movie_matches(data, dol.name)
        movie_path = out / "input.dtm"
        movie_path.write_bytes(data)
        input_record = {"kind": "supplied DTM; virtual input"}
    if movie_path:
        input_record.update(movie_sha256=sha256(movie_path), game_id=dtm_game_id(movie_path.read_bytes()).decode(),
                            polls=(movie_path.stat().st_size - DTM_HEADER_BYTES) // 8)

    sd = profile / "Load" / "WiiSD.raw"
    staging = {"used": False}
    if args.sd_image:
        shutil.copyfile(args.sd_image, sd)
        staging = {"used": bool(args.stage), "sd_image_sha256_before": sha256(sd), "files": [], "commands": []}
        if args.stage:
            fat = Fat32(sd.read_bytes())
            dirs = {name.lower() for name, attr, _, _ in fat.entries(fat.root) if attr & 0x10}
            for argv in staging_commands(sd, args.stage, dirs):
                result = subprocess.run(["wsl", "-d", args.wsl_distro, "--", *argv], capture_output=True, text=True)
                staging["commands"].append({"tool": argv[0], "target": argv[-1], "exit_code": result.returncode})
                if result.returncode != 0:
                    print(f"staging failed: {argv[0]} {argv[-1]}: {result.stderr.strip()}", file=sys.stderr)
                    return 2
            fat = Fat32(sd.read_bytes())
            for source, destination in args.stage:
                copied = fat.read(sd_relative(destination))
                staged_ok = copied is not None and hashlib.sha256(copied).hexdigest() == sha256(source)
                staging["files"].append({"sd_path": destination, "sha256": sha256(source), "verified": staged_ok})
                if not staged_ok:
                    print(f"staged file {destination} did not read back identically", file=sys.stderr)
                    return 2
            staging["sd_image_sha256_after_staging"] = sha256(sd)

    read_paths = [sd_relative(p) for p in args.read_back]
    if args.scenario == "probe":
        read_paths = [PROBE_SENTINEL, PROBE_LOG] + [p for p in read_paths if p not in (PROBE_SENTINEL, PROBE_LOG)]
    staged_hashes = {sd_relative(d): sha256(s) for s, d in args.stage}
    read_paths += [p for p in staged_hashes if p not in read_paths]
    initial = {"sd_image_present": sd.is_file()}
    previous_log, initial_count = "", 0
    if sd.is_file():
        fat = Fat32(sd.read_bytes())
        initial["files"] = {p: (None if (d := fat.read(p)) is None else hashlib.sha256(d).hexdigest())
                            for p in read_paths}
        if args.scenario == "probe":
            initial_count = probe_sentinel_count(fat.read(PROBE_SENTINEL))
            existing = fat.read(PROBE_LOG)
            previous_log = existing.decode("ascii", "replace").replace("\r\n", "\n") if existing else ""
    initial["probe_sentinel_count"] = initial_count if args.scenario == "probe" else None

    launch = ["Dolphin.exe", "--user", "<new isolated profile>", "--batch", "--video_backend", args.backend,
              "--exec", dol.name] + (["--movie", "input.dtm"] if movie_path else [])
    record = {
        "schema_version": SCHEMA_VERSION, "tool": TOOL, "scenario": args.scenario,
        "identity": identity,
        "profile": {"new": True, "isolated": True, "stock_limits": {f"{s}.{k}": v for (s, k), v in STOCK_REQUIRED.items()},
                    "backend": args.backend, "efb_access": args.efb_access, "dump_frames": args.dump_frames,
                    "config_sha256": {n: hashlib.sha256(t.encode()).hexdigest() for n, t in files.items()},
                    "controllers": "GC port 1 standard controller; Wii remotes disabled"},
        "input": input_record, "staging": staging, "initial_sd": initial,
        "launch_shape": launch, "timeout_seconds": args.timeout,
        "tolerated_host_exits": sorted(f"0x{c:08x}" for c in tolerated),
        "requested_runs": args.runs, "runs": [], "os_stability": {"observation": "pending"},
        "physical_wii": "untested",
    }
    (out / "config").mkdir()
    for name, text in files.items():
        (out / "config" / name).write_text(text, encoding="ascii")

    def save():
        (out / "runtime.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    save()
    window_start = utc_now()
    for number in range(1, args.runs + 1):
        if running_count(exe.name):
            record["aborted"] = f"run {number}: another Dolphin process appeared"
            break
        argv = [str(exe), "--user", str(profile), "--batch", "--video_backend", args.backend, "--exec", str(dol)]
        if movie_path:
            argv += ["--movie", str(movie_path)]
        run = {"number": number, "cold_process": True, "started_utc": utc_now()}
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        begin = time.monotonic()
        with (out / f"run{number}.stdout.txt").open("wb") as so, (out / f"run{number}.stderr.txt").open("wb") as se:
            process = subprocess.Popen(argv, stdout=so, stderr=se, startupinfo=startup)
            try:
                status, forced = process.wait(timeout=args.timeout), False
            except subprocess.TimeoutExpired:
                process.kill()
                status, forced = process.wait(), True
        elapsed = round(time.monotonic() - begin, 3)
        run["host"] = {**exit_fields(status), "forced_stop": forced, "elapsed_seconds": elapsed,
                       "outcome": host_outcome(status, forced, tolerated),
                       "dolphin_processes_after": running_count(exe.name)}
        run["ended_utc"] = utc_now()
        frames_dir = profile / "Dump" / "Frames"
        if frames_dir.is_dir():
            target = out / f"run{number}-frames"
            shutil.move(str(frames_dir), str(target))
            pngs = sorted(target.glob("*.png"))
            run["frame_dump"] = {"count": len(pngs),
                                 "first_sha256": sha256(pngs[0]) if pngs else None,
                                 "last_sha256": sha256(pngs[-1]) if pngs else None}
        read = {}
        if sd.is_file():
            fat = Fat32(sd.read_bytes())
            for path in read_paths:
                data = fat.read(path)
                read[path] = data
                if data is not None:
                    local = out / f"run{number}.sd" / path
                    local.parent.mkdir(parents=True, exist_ok=True)
                    local.write_bytes(data)
            run["sd_image_sha256"] = sha256(sd)
        run["read_back"] = {p: None if d is None else {"bytes": len(d), "sha256": hashlib.sha256(d).hexdigest()}
                            for p, d in read.items()}
        if args.scenario == "probe":
            guest, persistence = check_probe(build_id, number, initial_count, previous_log, read, args.probe_exit)
            log = read.get(PROBE_LOG)
            if log is not None:
                previous_log = log.decode("ascii", "replace").replace("\r\n", "\n")
        else:
            guest = {"failures": [], "note": "no scenario checker; read-back only"}
            missing = [p for p, d in read.items() if d is None] if sd.is_file() else read_paths
            persistence = {"failures": [f"missing:{p}" for p in missing]}
        persistence["failures"] += staged_file_failures(staged_hashes, read)
        if forced:
            guest["failures"].append("forced_stop_before_natural_exit")
        guest["result"] = "failed" if guest["failures"] else ("passed" if args.scenario != "none" else "not_evaluated")
        persistence["result"] = "failed" if persistence["failures"] else ("passed" if read_paths else "not_evaluated")
        run["guest"], run["persistence"] = guest, persistence
        record["runs"].append(run)
        save()
        print(f"run {number}: guest={guest['result']} persistence={persistence['result']} "
              f"host={run['host']['outcome']} {run['host']['exit_code_hex']} forced={forced} "
              f"elapsed={elapsed}s", flush=True)
        if guest["failures"] and any(f.startswith("forced_stop") for f in guest["failures"]):
            record["aborted"] = f"run {number} was force-stopped; later runs skipped"
            break
    time.sleep(args.event_settle)
    window_end = utc_now()
    record["os_stability"] = query_os_events(window_start, window_end)
    record["os_stability"]["window_utc"] = [window_start, window_end]
    record["outcomes"] = {
        "guest": combine([r["guest"]["result"] for r in record["runs"]], args.runs),
        "persistence": combine([r["persistence"]["result"] for r in record["runs"]], args.runs),
        "host": sorted({r["host"]["outcome"] for r in record["runs"]}),
        "os_stability": record["os_stability"]["observation"],
        "physical_wii": "untested",
    }
    record["tool_exit"] = overall_exit(record)
    save()
    print(json.dumps(record["outcomes"]), flush=True)
    return record["tool_exit"]


if __name__ == "__main__":
    sys.exit(main())
