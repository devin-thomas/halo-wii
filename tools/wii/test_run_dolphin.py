"""Pure parts of the Dolphin runner: no emulator, WSL or event log is touched."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

import run_dolphin as rd

BUILD = "0123456789abcdef"


class Fat32Image:
    """Authored FAT32 image: 512-byte sectors, one sector per cluster."""

    def __init__(self, clusters=256):
        self.reserved, self.fats, self.spfat = 32, 2, (clusters + 2) * 4 // 512 + 1
        self.data_sector = self.reserved + self.fats * self.spfat
        self.image = bytearray((self.data_sector + clusters) * 512)
        boot = self.image
        boot[0:3] = b"\xeb\x58\x90"
        struct.pack_into("<HBH", boot, 11, 512, 1, self.reserved)
        boot[16] = self.fats
        struct.pack_into("<I", boot, 36, self.spfat)
        struct.pack_into("<I", boot, 44, 2)
        boot[510:512] = b"\x55\xaa"
        self.next = 3
        self.set_fat(0, 0x0FFFFFF8)
        self.set_fat(1, 0x0FFFFFFF)
        self.set_fat(2, 0x0FFFFFFF)
        self.dirs = {2: 0}

    def set_fat(self, cluster, value):
        struct.pack_into("<I", self.image, self.reserved * 512 + cluster * 4, value)

    def offset(self, cluster):
        return (self.data_sector + cluster - 2) * 512

    def allocate(self, count):
        first = self.next
        for index in range(count):
            self.set_fat(first + index, first + index + 1 if index + 1 < count else 0x0FFFFFFF)
        self.next += count
        return first

    def add_entry(self, directory, raw):
        position = self.offset(directory) + self.dirs[directory]
        self.image[position:position + 32] = raw
        self.dirs[directory] += 32

    def add(self, directory, long_name, short, attr, first, size):
        assert len(short) == 11
        checksum = 0
        for byte in short:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
        if long_name:
            chars = [long_name[i:i + 13] for i in range(0, len(long_name), 13)]
            for sequence in range(len(chars), 0, -1):
                piece = chars[sequence - 1].encode("utf-16-le")
                piece = (piece + b"\0\0" + b"\xff" * 26)[:26] if len(piece) < 26 else piece
                entry = bytearray(32)
                entry[0] = sequence | (0x40 if sequence == len(chars) else 0)
                entry[1:11], entry[14:26], entry[28:32] = piece[0:10], piece[10:22], piece[22:26]
                entry[11], entry[13] = 0x0F, checksum
                self.add_entry(directory, bytes(entry))
        entry = bytearray(32)
        entry[0:11] = short
        entry[11] = attr
        struct.pack_into("<H", entry, 20, first >> 16)
        struct.pack_into("<H", entry, 26, first & 0xFFFF)
        struct.pack_into("<I", entry, 28, size)
        self.add_entry(directory, bytes(entry))

    def mkdir(self, parent, name, short):
        cluster = self.allocate(1)
        self.dirs[cluster] = 0
        self.add(parent, name, short, 0x10, cluster, 0)
        return cluster

    def write(self, directory, name, short, data):
        count = max(1, -(-len(data) // 512))
        first = self.allocate(count)
        for index in range(count):
            chunk = data[index * 512:(index + 1) * 512]
            self.image[self.offset(first + index):self.offset(first + index) + len(chunk)] = chunk
        self.add(directory, name, short, 0x20, first, len(data))


def probe_image(sentinel=None, log=None):
    image = Fat32Image()
    image.add_entry(2, b"WIISD      \x08" + bytes(20))
    folder = image.mkdir(2, "halo-wii-probe", b"HALO-W~1   ")
    if sentinel is not None:
        image.write(folder, "sentinel.txt", b"SENTINELTXT", sentinel)
    if log is not None:
        image.write(folder, "probe.log", b"PROBE   LOG", log)
    return bytes(image.image)


def probe_log(run, frames=150, ticks=74, exit_reason="pad_start", build=BUILD, abi="ABI CANONICAL checks=9"):
    return (f"BEGIN build={build} previous_runs={run - 1} video=640x480 mode=0\n"
            "ARENAS mem1=0x801c5000..0x81800000 bytes=23310336 mem2=0x90002000..0x933c6f60 bytes=54284128\n"
            f"{abi}\nABI TARGET pointer_bytes=4 int_bytes=4 double_bytes=8\n"
            f"END build={build} frames={frames} ticks={ticks} connected=1 activity=1 exit={exit_reason} storage=1 abi=0\n")


class MovieTests(unittest.TestCase):
    def test_game_id_follows_dolphin_file_name_rule(self):
        self.assertEqual(rd.game_id_for_executable("probe.dol"), b"ID-pro")
        self.assertEqual(rd.game_id_for_executable(r"C:\x y\gx_scene.dol"), b"ID-gx_")
        self.assertEqual(rd.game_id_for_executable("ab"), b"ID-ab")

    def test_pad_entries_encode_buttons_triggers_and_sticks(self):
        body = rd.pad_entries({"port": 1, "steps": [
            {"label": "n", "polls": 2},
            {"label": "a", "polls": 1, "buttons": ["A"], "trigger_l": 64, "stick": [180, 128]},
            {"label": "s", "polls": 1, "buttons": ["START"]}]})
        self.assertEqual(len(body), 32)
        self.assertEqual(body[0:8], bytes.fromhex("0040000080808080"))
        self.assertEqual(body[16:24], bytes.fromhex("02404000b4808080"))
        self.assertEqual(body[24:32], bytes.fromhex("0140000080808080"))

    def test_pad_entries_reject_bad_scripts(self):
        for script in ({"port": 2, "steps": [{"polls": 1}]}, {"steps": []},
                       {"steps": [{"polls": 0}]}, {"steps": [{"polls": 1, "buttons": ["HOME"]}]},
                       {"steps": [{"polls": 1, "stick": [300, 0]}]}):
            with self.assertRaises(ValueError):
                rd.pad_entries(script)

    def test_authored_header_layout(self):
        body = rd.pad_entries({"steps": [{"polls": 5}]})
        movie = rd.author_dtm(b"ID-pro", body)
        self.assertEqual(len(movie), 256 + 40)
        self.assertEqual(movie[0:10], b"DTM\x1aID-pro")
        self.assertEqual(movie[10:13], b"\x01\x01\x00")
        self.assertEqual(struct.unpack_from("<QQ", movie, 13), (5, 5))
        self.assertEqual(struct.unpack_from("<Q", movie, 237)[0], 100_000_000_000)
        untouched = bytearray(movie[:256])
        for start, end in ((0, 13), (13, 29), (237, 245)):
            untouched[start:end] = bytes(end - start)
        self.assertEqual(bytes(untouched), bytes(256))
        self.assertEqual(rd.dtm_game_id(movie), b"ID-pro")
        rd.check_movie_matches(movie, "probe.dol")

    def test_mismatched_game_id_is_refused(self):
        movie = rd.author_dtm(b"ID-pro", rd.pad_entries({"steps": [{"polls": 1}]}))
        with self.assertRaises(ValueError):
            rd.check_movie_matches(movie, "gx_scene.dol")
        with self.assertRaises(ValueError):
            rd.dtm_game_id(b"XYZ" + bytes(300))

    def test_shipped_probe_script_presses_start_after_activity(self):
        script = json.loads((Path(__file__).parent / "inputs/probe-start.json").read_text(encoding="utf-8"))
        labels = [step["label"] for step in script["steps"]]
        self.assertLess(labels.index("a_trigger_l_stick_right"), labels.index("start_press"))
        self.assertEqual(len(rd.pad_entries(script)) % 8, 0)


class ProfileTests(unittest.TestCase):
    def test_profile_is_stock_limit_and_options_are_explicit(self):
        files = rd.profile_files()
        rd.check_stock_limits(files)
        core = rd.parse_ini(files["Dolphin.ini"])
        self.assertEqual(core[("Core", "SIDevice0")], "6")
        self.assertEqual(core[("Core", "WiiSDCardEnableFolderSync")], "False")
        self.assertNotIn("EFBAccessEnable", files["GFX.ini"])
        self.assertNotIn("DumpFramesAsImages", files["GFX.ini"])
        extra = rd.profile_files("OGL", efb_access=True, dump_frames=True)
        self.assertIn("EFBAccessEnable = True", extra["GFX.ini"])
        self.assertIn("DumpFramesAsImages = True", extra["GFX.ini"])
        self.assertIn("DumpFrames = True", extra["Dolphin.ini"])
        self.assertIn("GFXBackend = OGL", extra["Dolphin.ini"])

    def test_non_stock_profile_is_refused(self):
        for old, new in (("OverclockEnable = False", "OverclockEnable = True"),
                         ("RAMOverrideEnable = False", "RAMOverrideEnable = True"),
                         ("EnableCheats = False", "EnableCheats = True"),
                         ("EmulationSpeed = 1.0", "EmulationSpeed = 0.0")):
            files = rd.profile_files()
            files["Dolphin.ini"] = files["Dolphin.ini"].replace(old, new)
            with self.assertRaises(ValueError):
                rd.check_stock_limits(files)


class FatTests(unittest.TestCase):
    def test_reads_long_names_multi_cluster_files_and_missing_paths(self):
        log = ("x" * 700 + "\n").encode()
        fat = rd.Fat32(probe_image(b"halo-wii-probe-v1 2\n", log))
        self.assertEqual(fat.read("halo-wii-probe/sentinel.txt"), b"halo-wii-probe-v1 2\n")
        self.assertEqual(fat.read("HALO-WII-PROBE/probe.log"), log)
        self.assertIsNone(fat.read("halo-wii-probe/missing.txt"))
        self.assertIsNone(fat.read("absent/probe.log"))
        with self.assertRaises(ValueError):
            fat.read("halo-wii-probe")

    def test_rejects_non_fat_and_looping_chains(self):
        with self.assertRaises(ValueError):
            rd.Fat32(bytes(1024))
        image = Fat32Image()
        folder = image.mkdir(2, "loop", b"LOOP       ")
        image.write(folder, "a.bin", b"A       BIN", b"a" * 1024)
        first = image.next - 2
        image.set_fat(first + 1, first)
        with self.assertRaises(ValueError):
            rd.Fat32(bytes(image.image)).read("loop/a.bin")

    def test_sd_paths(self):
        self.assertEqual(rd.sd_relative("sd:/halo-wii-probe/probe.log"), "halo-wii-probe/probe.log")
        self.assertEqual(rd.sd_relative("/a//b"), "a/b")
        for bad in ("sd:/", "sd:/a/../b", "./x"):
            with self.assertRaises(ValueError):
                rd.sd_relative(bad)


class StagingTests(unittest.TestCase):
    def test_wsl_paths(self):
        self.assertEqual(rd.wsl_path(r"C:\work dir\sd.raw"), "/mnt/c/work dir/sd.raw")
        for bad in (r"\\server\share\x", "relative\\x"):
            with self.assertRaises(ValueError):
                rd.wsl_path(bad)

    def test_commands_create_missing_directories_once_and_never_clobber(self):
        commands = rd.staging_commands(r"D:\p\sd.raw", [(r"D:\in\a.bin", "sd:/game/data/a.bin"),
                                                         (r"D:\in\b.txt", "sd:/game/b.txt"),
                                                         (r"D:\in\c.txt", "sd:/apps/c.txt")], {"apps"})
        self.assertEqual(commands[0], ["mmd", "-i", "/mnt/d/p/sd.raw", "::/game"])
        self.assertEqual(commands[1], ["mmd", "-i", "/mnt/d/p/sd.raw", "::/game/data"])
        copies = [c for c in commands if c[0] == "mcopy"]
        self.assertEqual(len(commands), 5)
        self.assertTrue(all(c[1] == "-n" for c in copies))
        self.assertEqual(copies[0][-2:], ["/mnt/d/in/a.bin", "::/game/data/a.bin"])

    def test_stage_argument(self):
        self.assertEqual(rd.parse_stage(r"C:\a=b.bin=sd:/x/y.bin"), (Path(r"C:\a=b.bin"), "sd:/x/y.bin"))
        with self.assertRaises(Exception):
            rd.parse_stage("nothing")


    def test_staged_files_must_survive_launches(self):
        expected = {"a/x.bin": hashlib.sha256(b"x").hexdigest(), "a/y.bin": "0" * 64}
        self.assertEqual(rd.staged_file_failures(expected, {"a/x.bin": b"x", "a/y.bin": b"y"}),
                         ["staged_file_changed:a/y.bin"])
        self.assertEqual(rd.staged_file_failures(expected, {"a/x.bin": None}),
                         ["staged_file_missing:a/x.bin", "staged_file_missing:a/y.bin"])


class HostTests(unittest.TestCase):
    def test_tasklist_parsing(self):
        text = '"Dolphin.exe","1234","Console","1","100,000 K"\n"dolphin.exe","5","Console","1","1 K"\n'
        self.assertEqual(rd.parse_tasklist(text, "Dolphin.exe"), 2)
        self.assertEqual(rd.parse_tasklist("INFO: No tasks are running which match the specified criteria.\n",
                                           "Dolphin.exe"), 0)
        self.assertEqual(rd.parse_tasklist('"DolphinTool.exe","1","Console","1","1 K"\n', "Dolphin.exe"), 0)

    def test_exit_codes_are_recorded_not_rewritten(self):
        self.assertEqual(rd.exit_fields(0xC0000409), {"exit_code_hex": "0xc0000409", "exit_code_signed": -1073740791})
        self.assertEqual(rd.exit_fields(-1073740791)["exit_code_hex"], "0xc0000409")
        self.assertEqual(rd.host_outcome(0, False, set()), "exit_zero")
        self.assertEqual(rd.host_outcome(0xC0000409, False, set()), "nonzero_exit")
        self.assertEqual(rd.host_outcome(0xC0000409, False, {0xC0000409}), "nonzero_exit_tolerated")
        self.assertEqual(rd.host_outcome(1, False, {0xC0000409}), "nonzero_exit")
        self.assertEqual(rd.host_outcome(1, True, {1}), "forced_stop_timeout")
        self.assertEqual(rd.host_outcome(0, True, set()), "forced_stop_timeout")


class EventTests(unittest.TestCase):
    START = "2026-10-10T12:00:00+00:00"

    def test_no_events(self):
        result = rd.classify_events({"system": [], "application": [], "last_boot_utc": "2026-10-08T01:02:03.1234567Z"},
                                    self.START)
        self.assertEqual(result["observation"], "no_instability_events_observed")
        self.assertEqual(result["instability_events"], [])

    def test_instability_and_application_faults_are_separate_and_path_free(self):
        message = ("Faulting application name: Dolphin.exe, version: 1.0\nFaulting module name: ucrtbase.dll, v\n"
                   "Exception code: 0xc0000409\nFaulting application path: C:\\Users\\someone\\Dolphin.exe\n")
        result = rd.classify_events({
            "system": [{"id": 1001, "provider": "Microsoft-Windows-WER-SystemErrorReporting", "time_utc": "t"},
                       {"id": 1001, "provider": "Other", "time_utc": "t"},
                       {"id": 6008, "provider": "EventLog", "time_utc": "t"}],
            "application": [{"id": 1000, "provider": "Application Error", "time_utc": "t", "message": message}],
            "last_boot_utc": "2026-10-10T12:00:05.0000000Z"}, self.START)
        meanings = [e["meaning"] for e in result["instability_events"]]
        self.assertEqual(meanings, ["bugcheck", "unexpected_shutdown", "boot_after_window_start"])
        app = result["dolphin_application_events"][0]
        self.assertEqual((app["faulting_application"], app["faulting_module"], app["exception_code"]),
                         ("Dolphin.exe", "ucrtbase.dll", "0xc0000409"))
        self.assertNotIn("Users", json.dumps(result))

    def test_application_fault_alone_is_not_os_instability(self):
        result = rd.classify_events({"system": [], "application": [
            {"id": 1000, "provider": "Application Error", "time_utc": "t", "message": "Exception code: 0xc0000409"}],
            "last_boot_utc": None}, self.START)
        self.assertEqual(result["observation"], "no_instability_events_observed")
        self.assertEqual(len(result["dolphin_application_events"]), 1)


class ProbeScenarioTests(unittest.TestCase):
    def files(self, sentinel, log):
        fat = rd.Fat32(probe_image(sentinel, log))
        return {rd.PROBE_SENTINEL: fat.read(rd.PROBE_SENTINEL), rd.PROBE_LOG: fat.read(rd.PROBE_LOG)}

    def test_two_cold_launches_pass(self):
        first = probe_log(1)
        guest, persistence = rd.check_probe(BUILD, 1, 0, "", self.files(b"halo-wii-probe-v1 1\n", first.encode()))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertEqual(guest["observations"]["video"], {"width": 640, "height": 480, "vi_tv_mode": 0})
        self.assertEqual(guest["observations"]["arenas"]["mem2"][2], 54284128)
        self.assertEqual(guest["observations"]["exit"], "pad_start")
        both = first + probe_log(2, frames=160, ticks=80)
        guest, persistence = rd.check_probe(BUILD, 2, 0, first, self.files(b"halo-wii-probe-v1 2\n", both.encode()))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertTrue(persistence["prior_log_preserved"])
        self.assertEqual(len(guest["report"]), 5)

    def test_failures_are_reported_in_their_own_category(self):
        cases = [
            (dict(build="ffffffffffffffff"), b"halo-wii-probe-v1 1\n", "guest", "build_id_mismatch"),
            (dict(exit_reason="auto_exit"), b"halo-wii-probe-v1 1\n", "guest", "exit_not_pad_start"),
            (dict(frames=150, ticks=150), b"halo-wii-probe-v1 1\n", "guest", "timer_not_consistent_with_vi_frames"),
            (dict(abi="ABI FAIL canonical check=3"), b"halo-wii-probe-v1 1\n", "guest", "abi_report_missing_or_failed"),
            ({}, b"halo-wii-probe-v1 0\n", "persistence", "sentinel_did_not_advance_across_cold_process"),
        ]
        for options, sentinel, category, failure in cases:
            guest, persistence = rd.check_probe(BUILD, 1, 0, "", self.files(sentinel, probe_log(1, **options).encode()))
            self.assertIn(failure, (guest if category == "guest" else persistence)["failures"], failure)

    def test_rewritten_prior_log_and_missing_outputs_fail_persistence(self):
        log = probe_log(2)
        _, persistence = rd.check_probe(BUILD, 2, 0, "something else\n", self.files(b"halo-wii-probe-v1 2\n", log.encode()))
        self.assertIn("prior_guest_log_not_preserved", persistence["failures"])
        guest, persistence = rd.check_probe(BUILD, 1, 0, "", {})
        self.assertIn("guest_log_missing", guest["failures"])
        self.assertIn("sentinel_missing", persistence["failures"])

    def test_existing_sentinel_is_continued_and_invalid_one_is_not_reset(self):
        self.assertEqual(rd.probe_sentinel_count(None), 0)
        self.assertEqual(rd.probe_sentinel_count(b"halo-wii-probe-v1 7\n"), 7)
        with self.assertRaises(ValueError):
            rd.probe_sentinel_count(b"garbage")
        guest, persistence = rd.check_probe(BUILD, 1, 7, "", self.files(b"halo-wii-probe-v1 8\n",
                                                                      probe_log(8).encode()))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))


    def test_invalid_sentinel_control_requires_byte_identical_preservation(self):
        bad, log = b"halo-wii-probe-v1 x\n", b"BEGIN old\n"
        guest, persistence = rd.check_probe_invalid_sentinel(bad, log, {rd.PROBE_SENTINEL: bad, rd.PROBE_LOG: log})
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertTrue(persistence["invalid_sentinel_preserved"])
        _, persistence = rd.check_probe_invalid_sentinel(bad, log, {rd.PROBE_SENTINEL: b"halo-wii-probe-v1 1\n",
                                                                     rd.PROBE_LOG: log + b"BEGIN new\n"})
        self.assertEqual(persistence["failures"], ["invalid_sentinel_not_preserved",
                                                   "existing_log_changed_while_storage_refused"])


GXM_CRC = "53843a35"


def gxm_log(previous=0, build=BUILD, failed_check=False, crc2=GXM_CRC, heap_after3="100", missed_negative=False,
            result="pass", exit_reason="pad_start", toggles=2, loop_growth="0", unexpected="0"):
    """An authored gx_materials guest log for one launch."""
    lines = [f"BEGIN target=gx_materials build={build} previous_runs={previous} storage=1 video=640x480 "
             "efb_height=480 mode=0 aa=0"]
    for n in range(1, 5):
        for name in ("tex_i8", "control_rgb565_untiled_order"):
            ok = "fail" if failed_check and n == 2 and name == "tex_i8" else "pass"
            lines.append(f"CHECK cycle={n} name={name} kind=positive probes=4 matched=4 max_err=0 result={ok}")
        if n == 1:
            lines.append("NEGATIVE case=misaligned_base detected=1")
            lines.append(f"NEGATIVE case=overlaps_gx_fifo detected={0 if missed_negative else 1}")
        lines.append(f"CYCLE n={n} checks_passed=2 total=2 crc={crc2 if n == 2 else GXM_CRC} heap_before=100 "
                     f"heap_loaded=200 heap_after={heap_after3 if n == 3 else '100'} texture_bytes=10 pool_peak=64 "
                     "upload_us=1 checks_us=2 fifo_check_peak=8")
    lines.append(f"SUMMARY checks_passed=8 total=8 per_cycle=2 cycles=4 crc_stable={int(crc2 == GXM_CRC)} "
                 "heap_growth_max=0 heap_peak=200 negatives=2/2 guard_failures=0 leaks=0 "
                 f"libogc_size_disagreements=1 unexpected_failures={unexpected}")
    lines.append("FIFO bytes=262144 gallery_per_frame_peak=47680 avg=46399")
    lines.append(f"HEAP start=1 baseline=100 peak=200 display_loaded=200 loop_end=200 end=100 growth=0 "
                 f"loop_growth={loop_growth} max_cycle_growth=0 arena2_restored=1")
    lines.append(f"END target=gx_materials build={build} frames=600 connected=1 activity=1 toggles={toggles} "
                 f"exit={exit_reason} storage=1 checks=8/8 negatives=2/2 unexpected={unexpected} heap_growth=0 "
                 f"crc_stable=1 result={result}")
    return "\n".join(lines) + "\n"


class GxMaterialsScenarioTests(unittest.TestCase):
    @staticmethod
    def files(counter, log):
        return {rd.GXM_RUNS: counter, rd.GXM_LOG: log.encode()}

    def test_two_cold_launches_pass(self):
        first = gxm_log(0)
        guest, persistence = rd.check_gx_materials(BUILD, 1, 0, "", self.files(b"halo-wii-gxm-v1 1\n", first))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        obs = guest["observations"]
        self.assertEqual(obs["checks"], {"lines": 8, "distinct": 2, "failed": [], "controls": 1})
        self.assertEqual(len(obs["cycles"]), 4)
        self.assertEqual(obs["summary"]["libogc_size_disagreements"], "1")
        self.assertEqual(obs["fifo"]["gallery_per_frame_peak"], "47680")
        both = first + gxm_log(1)
        guest, persistence = rd.check_gx_materials(BUILD, 2, 0, first, self.files(b"halo-wii-gxm-v1 2\n", both))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertTrue(persistence["prior_log_preserved"])

    def test_guest_failures_are_reported_in_the_guest_category(self):
        cases = [
            (dict(build="ffffffffffffffff"), "build_id_mismatch"),
            (dict(failed_check=True), "efb_checks_failed_or_missing"),
            (dict(crc2="00000000"), "cycle_results_differ"),
            (dict(heap_after3="132"), "heap_grew_across_load_cycles"),
            (dict(missed_negative=True), "deliberate_invalid_case_not_detected"),
            (dict(result="fail"), "guest_reported_fail"),
            (dict(exit_reason="auto_exit"), "exit_not_pad_start"),
            (dict(toggles=1), "input_toggles_not_observed"),
            (dict(loop_growth="16"), "heap_growth_nonzero"),
            (dict(unexpected="1"), "summary_unexpected_failures_nonzero"),
        ]
        for options, failure in cases:
            guest, persistence = rd.check_gx_materials(BUILD, 1, 0, "", self.files(b"halo-wii-gxm-v1 1\n",
                                                                                 gxm_log(0, **options)))
            self.assertIn(failure, guest["failures"], failure)
            self.assertEqual(persistence["failures"], [], failure)

    def test_persistence_failures_stay_separate(self):
        guest, persistence = rd.check_gx_materials(BUILD, 1, 0, "", self.files(b"halo-wii-gxm-v1 0\n", gxm_log(0)))
        self.assertEqual(guest["failures"], [])
        self.assertIn("run_counter_did_not_advance_across_cold_process", persistence["failures"])
        _, persistence = rd.check_gx_materials(BUILD, 2, 0, "something else\n",
                                               self.files(b"halo-wii-gxm-v1 2\n", gxm_log(1)))
        self.assertIn("prior_guest_log_not_preserved", persistence["failures"])
        guest, persistence = rd.check_gx_materials(BUILD, 1, 0, "", {})
        self.assertIn("guest_log_missing", guest["failures"])
        self.assertIn("run_counter_missing", persistence["failures"])
        guest, _ = rd.check_gx_materials(BUILD, 1, 0, "", self.files(b"halo-wii-gxm-v1 1\n",
                                                                     gxm_log(0).rsplit("END ", 1)[0]))
        self.assertTrue(any(f.startswith("expected_one_BEGIN") for f in guest["failures"]))

    def test_repeated_log_keys_are_kept(self):
        self.assertEqual(rd.fields("TIMING frames=667 submit_us_avg=723 max=726 copy_us_avg=2 max=2"),
                         {"frames": "667", "submit_us_avg": "723", "max": "726", "copy_us_avg": "2", "max_2": "2"})

    def test_run_counter_parsing_never_resets_an_invalid_counter(self):
        self.assertEqual(rd.gxm_runs_count(None), 0)
        self.assertEqual(rd.gxm_runs_count(b"halo-wii-gxm-v1 5\n"), 5)
        with self.assertRaises(ValueError):
            rd.gxm_runs_count(b"halo-wii-gxm-v1 x\n")

    def test_scenario_requires_build_info_and_efb_access(self):
        base = ["--dolphin", "D.exe", "--dol", "gx_materials.dol", "--out", "o", "--scenario", "gx_materials"]
        with self.assertRaises(SystemExit):
            rd.parse_args(base + ["--build-info", "b.json"])
        with self.assertRaises(SystemExit):
            rd.parse_args(base + ["--efb-access"])
        self.assertTrue(rd.parse_args(base + ["--build-info", "b.json", "--efb-access"]).efb_access)

    def test_shipped_materials_script_toggles_then_exits(self):
        script = json.loads((Path(__file__).parent / "inputs/gx-materials.json").read_text(encoding="utf-8"))
        labels = [step["label"] for step in script["steps"]]
        self.assertLess(labels.index("a_press_spin"), labels.index("b_press_overlay"))
        self.assertLess(labels.index("b_press_overlay"), labels.index("start_press"))
        self.assertEqual(len(rd.pad_entries(script)) % 8, 0)


CAL_NAMES = ("front_depth_cull", "backdrop_visible", "clear_corner", "control_cull_none", "control_depth_off",
             "pitch_top_visible", "pitch_front_lower", "yaw_left_visible", "depth_order")


def gxs_log(previous=0, build=BUILD, failed_cal=None, passed=9, leak="0", result="pass", exit_reason="pad_start",
            resets=1, range_failures="0"):
    """An authored gx_scene guest log for one launch (shape of the HWI-014A guest report)."""
    lines = [f"BEGIN target=gx_scene build={build} previous_runs={previous} storage=1 video=640x480 "
             "efb_height=480 mode=0 aa=0"]
    lines += [f"CAL name={name} pixel=1,1 expected=e03030 observed=e03030ff z=faf6b1 "
              f"pass={0 if name == failed_cal else 1}" for name in CAL_NAMES]
    lines.append(f"CALIBRATION passed={passed} total=9 scope=EFB_readback_in_stock_Dolphin_not_hardware")
    lines.append("INPUT frame=295 auto_spin=1")
    lines.append("TIMING frames=431 render_us_min=69 max=82 avg=74 interval_us_min=16434 max=16702 scope=x")
    lines.append(f"HEAP end=1669664 after_init=1669664 leak_since_init={leak}")
    lines.append(f"END target=gx_scene build={build} frames=431 connected=1 activity=1 first_input_frame=95 "
                 f"toggles=1 resets={resets} exit={exit_reason} storage=1 calibration={passed}/9 "
                 f"range_failures={range_failures} result={result}")
    return "\n".join(lines) + "\n"


class GxSceneScenarioTests(unittest.TestCase):
    @staticmethod
    def files(counter, log):
        return {rd.GXS_RUNS: counter, rd.GXS_LOG: log.encode()}

    def test_two_cold_launches_pass(self):
        first = gxs_log(0)
        guest, persistence = rd.check_gx_scene(BUILD, 1, 0, "", self.files(b"halo-wii-gx-v1 1\n", first))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertEqual(guest["observations"]["calibration"], "9/9")
        self.assertEqual(guest["observations"]["timing"]["max_2"], "16702")
        guest, persistence = rd.check_gx_scene(BUILD, 2, 0, first,
                                               self.files(b"halo-wii-gx-v1 2\n", first + gxs_log(1)))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertTrue(persistence["prior_log_preserved"])

    def test_guest_failures_are_reported_in_the_guest_category(self):
        cases = [
            (dict(build="ffffffffffffffff"), "build_id_mismatch"),
            (dict(failed_cal="clear_corner", passed=8), "calibration_failed_or_incomplete"),
            (dict(failed_cal="clear_corner"), "calibration_check_failed"),
            (dict(leak="16"), "heap_leak_since_init"),
            (dict(result="fail"), "guest_reported_fail"),
            (dict(exit_reason="auto_exit"), "exit_not_pad_start"),
            (dict(resets=0), "input_not_observed"),
            (dict(range_failures="1"), "owned_range_failures"),
        ]
        for options, failure in cases:
            guest, persistence = rd.check_gx_scene(BUILD, 1, 0, "", self.files(b"halo-wii-gx-v1 1\n",
                                                                             gxs_log(0, **options)))
            self.assertIn(failure, guest["failures"], failure)
            self.assertEqual(persistence["failures"], [], failure)

    def test_persistence_failures_stay_separate(self):
        guest, persistence = rd.check_gx_scene(BUILD, 2, 0, "", self.files(b"halo-wii-gx-v1 1\n", gxs_log(0)))
        self.assertIn("run_counter_did_not_advance_across_cold_process", persistence["failures"])
        self.assertIn("guest_previous_runs_mismatch", persistence["failures"])
        self.assertEqual(guest["failures"], [])
        guest, persistence = rd.check_gx_scene(BUILD, 1, 0, "", {})
        self.assertIn("guest_log_missing", guest["failures"])
        self.assertIn("run_counter_missing", persistence["failures"])


GEO_CULL = {"none": ("aac744a4", 63), "content_back": ("18b17d9a", 47), "opposite_front": ("b9cf71f5", 52)}


def geo_log(previous=0, build=BUILD, cycles=4, index_crc3="bad05748", back_crc2="18b17d9a", mem2_restored=1,
            fault_clean=1, exit_reason="pad_start", failures=0, manifest_ok=1):
    """An authored geometry_view guest log for one launch (shape of the HWI-016 guest report)."""
    lines = [f"BEGIN target=geometry_view build={build} previous_runs={previous} storage=1 video=640x480 "
             "efb_height=480",
             f"MANIFEST ok={manifest_ok} material=3 surfaces=2990 vertices=5863 tag_bytes=1 bsp_bytes=1",
             "MALFORMED manifest_rejections=3/3"]
    for n in range(cycles):
        index_crc = index_crc3 if n == 3 else "bad05748"
        lines.append(f"CYCLE n={n} placement={n % 2} required=1 charge=1 remaining=1 load_us={1699738 + n} "
                     f"index_crc={index_crc} position_crc=db11989c degenerate=0 normals_along=0 opposed=2990 "
                     "unclear=0 none_visible=63/768 none_grid_crc=aac744a4")
        for mode, (crc, visible) in GEO_CULL.items():
            if mode == "content_back" and n == 2:
                crc = back_crc2
            lines.append(f"CULL n={n} mode={mode} shaded_pose0 visible={visible} same_as_none=740 crc={crc} "
                         f"draw_us={280 + n} classified_pose0 lit=30 unlit=33 unclear=0 visible={visible} "
                         f"draw_us={2227 + n}")
    lines.append(f"INTERACTIVE activity=1 stick_seen=1 input_yaw=8.418 input_zoom=46.72 resets=2 exit={exit_reason}")
    lines.append(f"FAULT kind=bsp_crc_flip reason=crc clean={fault_clean}")
    lines.append("FAULT kind=tag_length_minus_one reason=trailing clean=1")
    lines.append(f"MEMORY mem2_bytes=54284128 heap_start=178320 heap_end=1669488 mem2_restored={mem2_restored}")
    lines.append("GX fifo_bytes=262144 fifo_submitted_peak=77312 render_max_us=305 frames=784 cull=content_back")
    lines.append(f"END target=geometry_view build={build} cycles={cycles} stale_rejected=4 faults_rejected=2 "
                 f"failures={failures} result={'pass' if not failures else 'fail'}")
    return "\n".join(lines) + "\n"


class GeometryViewScenarioTests(unittest.TestCase):
    @staticmethod
    def files(counter, log):
        return {rd.GEO_RUNS: counter, rd.GEO_LOG: log.encode()}

    def test_two_cold_launches_pass_and_cull_results_are_recorded_without_timings(self):
        first = geo_log(0)
        guest, persistence = rd.check_geometry_view(BUILD, 1, 0, "", self.files(b"halo-wii-geometry-v1 1\n", first))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        cull = guest["observations"]["cull"]
        self.assertEqual({mode: c["crc"] for mode, c in cull.items()},
                         {mode: crc for mode, (crc, _) in GEO_CULL.items()})
        self.assertNotIn("draw_us", json.dumps(cull))
        self.assertEqual(guest["observations"]["cull_draw_us"]["none"], ["280", "2227"])
        self.assertEqual(guest["observations"]["cycles"][3]["index_crc"], "bad05748")
        guest, persistence = rd.check_geometry_view(BUILD, 2, 0, first,
                                                    self.files(b"halo-wii-geometry-v1 2\n", first + geo_log(1)))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))

    def test_guest_failures_are_reported_in_the_guest_category(self):
        cases = [
            (dict(cycles=3), "expected_4_load_cycles_found_3"),
            (dict(index_crc3="00000000"), "cycle_results_differ"),
            (dict(back_crc2="00000000"), "cull_results_differ_or_missing"),
            (dict(mem2_restored=0), "mem2_not_restored"),
            (dict(fault_clean=0), "fault_case_not_rejected_cleanly"),
            (dict(exit_reason="auto_exit"), "exit_not_pad_start"),
            (dict(failures=1), "guest_reported_fail"),
            (dict(manifest_ok=0), "manifest_not_accepted"),
        ]
        for options, failure in cases:
            guest, persistence = rd.check_geometry_view(BUILD, 1, 0, "", self.files(b"halo-wii-geometry-v1 1\n",
                                                                                  geo_log(0, **options)))
            self.assertIn(failure, guest["failures"], failure)
            self.assertEqual(persistence["failures"], [], failure)

    def test_run_counter_is_continued_and_never_reset(self):
        self.assertEqual(rd.run_counter(None, rd.GEO_MAGIC), 0)
        self.assertEqual(rd.run_counter(b"halo-wii-geometry-v1 3\n", rd.GEO_MAGIC), 3)
        for bad in (b"halo-wii-geometry-v1 x\n", b"halo-wii-gx-v1 3\n"):
            with self.assertRaises(ValueError):
                rd.run_counter(bad, rd.GEO_MAGIC)

    def test_new_scenarios_require_build_info_and_efb_access(self):
        for scenario in ("gx_scene", "geometry_view"):
            base = ["--dolphin", "Dolphin.app", "--dol", "x.dol", "--out", "o", "--scenario", scenario]
            with self.assertRaises(SystemExit):
                rd.parse_args(base + ["--build-info", "b.json"])
            with self.assertRaises(SystemExit):
                rd.parse_args(base + ["--efb-access"])
            self.assertIsNone(rd.parse_args(base + ["--build-info", "b.json", "--efb-access"]).backend)

    def test_shipped_scripts_end_with_start(self):
        for name, polls in (("gx-scene.json", 1278), ("geometry-view.json", 36000)):
            script = json.loads((Path(__file__).parent / "inputs" / name).read_text(encoding="utf-8"))
            self.assertEqual(len(rd.pad_entries(script)) // 8, polls, name)
            self.assertIn("START", [b for step in script["steps"] for b in step.get("buttons", [])], name)


def content_log(run, cycles=2, count=3, failed_case=None, crc2=None, heap=0, exit_reason="pad_start", result="pass",
                cases_ok=1, mismatches=0, controls="2/3"):
    """An authored content-loader guest report for launch `run` (0-based)."""
    lines = [f"BEGIN target=content_loaders build={BUILD} previous_runs={run} storage=1",
             f"CASES ok={cases_ok} count={count} efb=640x480 crop=32 cycles=2"]
    for n in range(1, cycles + 1):
        for i in range(count):
            verdict = "fail" if failed_case == (n, i) else "pass"
            lines.append(f"CASE cycle={n} id=t{i:02d} kind=texture expect=ok got=ok bytes=96 file_sha=1 "
                         f"decoded_match=1 units=1 gx_images=1 mip_levels=0 draws=2 peeks=32 mismatches=0 "
                         f"interpolated=0 max_err=0,0,0,0 readback_crc=12345678 load_us=10 decode_us=5 "
                         f"result={verdict}")
        failed = 1 if failed_case and failed_case[0] == n else 0
        crc = crc2 if (crc2 and n == 2) else "0badcafe"
        lines.append(f"CYCLE n={n} passed={count - failed} failed={failed} rejected_as_expected=0 textures={count}/{count} "
                     f"animations=0/0 sounds=0/0 gx_images={count} mip_levels=0 draws=6 peeks=96 "
                     f"mismatches={mismatches} max_err=0,0,0,0 controls_detected={controls} crc={crc} heap_growth={heap}")
        lines.append(f"TIMING cycle={n} kind=texture valid_bytes=288 load_us=30 decode_us=15 scope=x")
    lines.append(f"SUMMARY cycles=2 cases={count} stable=1 all_passed=1 heap_growth_max={heap}")
    lines.append(f"END target=content_loaders build={BUILD} exit={exit_reason} storage=1 cases={count} stable=1 "
                 f"heap_growth={heap} result={result}")
    return "\n".join(lines) + "\n"


class ContentLoadersScenarioTests(unittest.TestCase):
    @staticmethod
    def files(counter, log):
        return {rd.CNT_RUNS: counter, rd.CNT_LOG: log.encode()}

    def test_two_cold_launches_pass(self):
        first = content_log(0)
        guest, persistence = rd.check_content_loaders(BUILD, 1, 0, "", self.files(b"halo-wii-content-v1 1\n", first))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))
        self.assertEqual(guest["observations"]["cycles"][1]["crc"], "0badcafe")
        guest, persistence = rd.check_content_loaders(
            BUILD, 2, 0, first, self.files(b"halo-wii-content-v1 2\n", first + content_log(1)))
        self.assertEqual((guest["failures"], persistence["failures"]), ([], []))

    def test_guest_failures_are_reported_in_the_guest_category(self):
        cases = [
            (dict(failed_case=(2, 1)), "cases_failed"),
            (dict(cycles=1), "expected_2_cycles_of_3_cases"),
            (dict(crc2="00000000"), "cycle_results_differ"),
            (dict(mismatches=1), "cycle_not_all_passed"),
            (dict(heap=32), "summary_not_stable_passed_or_heap_grew"),
            (dict(exit_reason="auto_exit"), "exit_not_pad_start"),
            (dict(result="fail"), "guest_reported_fail"),
            (dict(cases_ok=0), "case_list_not_accepted"),
            (dict(controls="0/3"), "texture_control_not_detected"),
        ]
        for options, failure in cases:
            guest, persistence = rd.check_content_loaders(BUILD, 1, 0, "", self.files(
                b"halo-wii-content-v1 1\n", content_log(0, **options)))
            self.assertIn(failure, guest["failures"], failure)
            self.assertEqual(persistence["failures"], [], failure)

    def test_counter_that_did_not_advance_is_a_persistence_failure(self):
        guest, persistence = rd.check_content_loaders(BUILD, 1, 0, "", self.files(b"halo-wii-content-v1 7\n",
                                                                                  content_log(0)))
        self.assertIn("run_counter_did_not_advance_across_cold_process", persistence["failures"])

    def test_scenario_requires_build_info_and_efb_access_and_script_holds_start(self):
        base = ["--dolphin", "Dolphin.exe", "--dol", "content_loaders.dol", "--out", "o", "--scenario",
                "content_loaders"]
        with self.assertRaises(SystemExit):
            rd.parse_args(base + ["--build-info", "b.json"])
        self.assertEqual(rd.parse_args(base + ["--build-info", "b.json", "--efb-access"]).scenario, "content_loaders")
        script = json.loads((Path(__file__).parent / "inputs" / "content-loaders.json").read_text(encoding="utf-8"))
        self.assertEqual(len(rd.pad_entries(script)) // 8, 200120)
        self.assertEqual(script["steps"][-1]["buttons"], ["START"])


class MacHostTests(unittest.TestCase):
    START = "2026-10-10T12:00:00+00:00"

    def test_host_platform_and_dolphin_bundle(self):
        self.assertEqual(rd.host_platform("win32"), "windows")
        self.assertEqual(rd.host_platform("darwin"), "macos")
        self.assertEqual(rd.host_platform("linux"), "unsupported")
        self.assertEqual(rd.resolve_dolphin(Path("/A/Dolphin.app")).as_posix(), "/A/Dolphin.app/Contents/MacOS/Dolphin")
        self.assertEqual(rd.resolve_dolphin(Path("D/Dolphin.exe")), Path("D/Dolphin.exe"))
        self.assertEqual(rd.DEFAULT_BACKEND, {"windows": "D3D", "macos": "Vulkan"})

    def test_repeat_multiplies_the_pad_body(self):
        script = {"steps": [{"label": "a", "polls": 2, "buttons": ["A"]}]}
        self.assertEqual(rd.pad_entries(dict(script, repeat=3)), rd.pad_entries(script) * 3)
        for bad in (0, -1, "2", True):
            with self.assertRaises(ValueError):
                rd.pad_entries(dict(script, repeat=bad))

    def test_native_staging_uses_host_paths_without_wsl(self):
        commands = rd.staging_commands("/p/sd.raw", [("/in/a b.bin", "sd:/game/a.bin")], set(), str)
        self.assertEqual(commands, [["mmd", "-i", "/p/sd.raw", "::/game"],
                                    ["mcopy", "-n", "-i", "/p/sd.raw", "/in/a b.bin", "::/game/a.bin"]])
        self.assertEqual(rd.staging_argv(commands[0], "macos"), commands[0])
        self.assertEqual(rd.staging_argv(commands[0], "windows", "Debian")[:4], ["wsl", "-d", "Debian", "--"])

    def test_pgrep_and_signal_exit(self):
        self.assertEqual(rd.parse_pgrep("123\n456\n"), 2)
        self.assertEqual(rd.parse_pgrep(""), 0)
        self.assertEqual(rd.exit_fields(-9, posix=True)["terminated_by_signal"], 9)
        self.assertNotIn("terminated_by_signal", rd.exit_fields(-1073740791))
        self.assertEqual(rd.host_outcome(-9, False, set()), "nonzero_exit")

    def test_boottime(self):
        self.assertEqual(rd.parse_boottime("{ sec = 1791592405, usec = 404105 } Fri Oct  9 19:33:25 2026"),
                         "2026-10-10T00:33:25+00:00")
        self.assertIsNone(rd.parse_boottime(""))

    def test_no_macos_events(self):
        result = rd.classify_macos([("ChatGPT_2026-10-10_host.diag", "t")], [{"count": 0, "finished": 1}],
                                   "2026-10-09T00:33:25+00:00", self.START)
        self.assertEqual(result["observation"], "no_instability_events_observed")
        self.assertEqual(result["dolphin_application_events"], [])

    def test_macos_instability_and_dolphin_faults_are_separate_and_path_free(self):
        log = [{"processImagePath": "/kernel", "eventMessage": "panic(cpu 1 caller 0x1): watchdog", "timestamp": "t"},
               {"processImagePath": "/kernel", "eventMessage": "AGX: GPU restart (count 1)", "timestamp": "t"},
               {"processImagePath": "/kernel", "eventMessage": "harmless match", "timestamp": "t"},
               {"processImagePath": "/Users/someone/Applications/Dolphin.app/Contents/MacOS/Dolphin",
                "eventMessage": "fault in /Users/someone/x", "timestamp": "t"}]
        result = rd.classify_macos([("Kernel-2026-10-10-host.panic", "t"), ("Dolphin-2026-10-10-1.ips", "t")], log,
                                   "2026-10-10T12:00:05+00:00", self.START)
        meanings = [e["meaning"] for e in result["instability_events"]]
        self.assertEqual(meanings, ["kernel_panic_report", "kernel_panic_gpu_restart_or_watchdog_message",
                                    "kernel_panic_gpu_restart_or_watchdog_message", "boot_after_window_start"])
        self.assertEqual(len(result["kernel_log_matched"]), 3)
        self.assertEqual([e["kind"] for e in result["dolphin_application_events"]],
                         ["dolphin_crash_report", "dolphin_fault_or_crash_log_message"])
        text = json.dumps(result)
        self.assertNotIn("someone", text)
        self.assertNotIn("host.panic", text)

    def test_dolphin_crash_alone_is_not_instability_and_failed_log_is_unmeasured(self):
        result = rd.classify_macos([("Dolphin-2026-10-10-1.ips", "t")], [], None, self.START)
        self.assertEqual(result["observation"], "no_instability_events_observed")
        self.assertEqual(result["dolphin_application_event_count"], 1)
        self.assertEqual(rd.classify_macos([], [], None, self.START, log_ok=False)["observation"], "unmeasured")


class OutcomeTests(unittest.TestCase):
    def record(self, guest="passed", persistence="passed", host="exit_zero", os="no_instability_events_observed"):
        run = {"guest": {"result": guest}, "persistence": {"result": persistence}, "host": {"outcome": host}}
        return {"requested_runs": 2, "runs": [run, dict(run)], "os_stability": {"observation": os}}

    def test_exit_status_keeps_outcomes_distinct(self):
        self.assertEqual(rd.overall_exit(self.record()), 0)
        self.assertEqual(rd.overall_exit(self.record(host="nonzero_exit_tolerated")), 0)
        self.assertEqual(rd.overall_exit(self.record(host="nonzero_exit")), 3)
        self.assertEqual(rd.overall_exit(self.record(host="forced_stop_timeout")), 3)
        self.assertEqual(rd.overall_exit(self.record(guest="failed", host="exit_zero")), 1)
        self.assertEqual(rd.overall_exit(self.record(persistence="failed")), 1)
        self.assertEqual(rd.overall_exit(self.record(os="instability_observed")), 4)
        short = self.record()
        short["runs"].pop()
        self.assertEqual(rd.overall_exit(short), 1)

    def test_combine(self):
        self.assertEqual(rd.combine(["passed", "passed"], 2), "passed")
        self.assertEqual(rd.combine(["passed"], 2), "failed")
        self.assertEqual(rd.combine(["passed", "failed"], 2), "failed")
        self.assertEqual(rd.combine(["not_evaluated", "not_evaluated"], 2), "not_evaluated")


if __name__ == "__main__":
    unittest.main()
