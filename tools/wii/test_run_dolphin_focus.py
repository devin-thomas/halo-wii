"""Focus- and keyboard-safe Dolphin launch (HWI-077): no emulator is launched.

Profile generation (no host input device bound anywhere), the launch
environment, and the focus guard with mocked frontmost/focus queries.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import run_dolphin as rd
from test_run_dolphin_lanes import Clock, FakeProcess, Host


class ProfileTests(unittest.TestCase):
    def test_legacy_profile_is_unchanged(self):
        """--no-focus-safe writes exactly the pre-HWI-077 files (hashes taken from bae56603)."""
        cases = [((), {"Dolphin.ini": "db4443269e0a3a9c", "GFX.ini": "ba2a11b85ae391ab",
                       "WiimoteNew.ini": "3f8ccc51b4bd3aaa"}),
                 (("Vulkan", True, True, True, 2), {"Dolphin.ini": "0fb78769da4a4f8d", "GFX.ini": "2b1e23527b46478f",
                                                     "WiimoteNew.ini": "3f8ccc51b4bd3aaa"})]
        for args, expected in cases:
            files = rd.profile_files(*args)
            self.assertEqual({n: hashlib.sha256(t.encode()).hexdigest()[:16] for n, t in files.items()}, expected)

    def test_every_input_config_is_written_with_no_device_and_empty_controls(self):
        controls = rd.load_controls()
        for host in ("windows", "macos"):
            with self.subTest(host=host):
                files = rd.profile_files("Vulkan", True, focus_safe=True, host=host)
                rd.check_stock_limits(files)
                for name in ("GCPadNew.ini", "GCKeyNew.ini", "GBA.ini", "FreeLookController.ini", "Hotkeys.ini",
                             "WiimoteNew.ini"):
                    self.assertIn(name, files)
                    values = rd.parse_ini(files[name])
                    spec = controls["files"][name]
                    for section in spec["sections"]:
                        self.assertEqual(values[(section, "Device")], "None/0/No Device")
                        for control in spec["controls"]:
                            self.assertEqual(values[(section, control)], "")
                    bound = {k: v for k, v in values.items() if v and k[1] != "Device"}
                    if name == "WiimoteNew.ini":
                        self.assertEqual(set(bound.values()), {"0"})
                        self.assertEqual({k[1] for k in bound}, {"Source"})
                    else:
                        self.assertEqual(bound, {})
                core = rd.parse_ini(files["Dolphin.ini"])
                self.assertEqual(core[("Input", "BackgroundInput")], "False")
                self.assertEqual(core[("Core", "PauseOnFocusLost")], "False")
                self.assertEqual(core[("Interface", "PauseOnFocusLost")], "False")
                self.assertEqual(core[("Display", "KeepWindowOnTop")], "False")
                self.assertEqual(core.get(("SDL_Hints", "SDL_JOYSTICK_MFI")), "0" if host == "macos" else None)
                self.assertNotIn(("DSP", "Backend"), core)

    def test_default_keyboard_bindings_are_all_covered(self):
        """The controls Dolphin 2609 binds by default, including the hotkeys that act on the emulator."""
        files = rd.load_controls()["files"]
        for name, control in (("GCPadNew.ini", "Buttons/A"), ("GCPadNew.ini", "Main Stick/Up"),
                              ("GCKeyNew.ini", "Keys/SCR LK"), ("GCKeyNew.ini", "Keys/A"),
                              ("GBA.ini", "Buttons/START"), ("FreeLookController.ini", "Move/Up"),
                              ("Hotkeys.ini", "General/Stop"), ("Hotkeys.ini", "General/Toggle Fullscreen"),
                              ("Hotkeys.ini", "General/Toggle Pause"), ("Hotkeys.ini", "Load State/Load State Slot 1"),
                              ("Hotkeys.ini", "Save State/Save State Slot 1"), ("Hotkeys.ini", "Stepping/Step Into")):
            self.assertIn(control, files[name]["controls"], (name, control))
        self.assertEqual(len(files["Hotkeys.ini"]["controls"]), 144)
        for name, spec in files.items():
            self.assertTrue(spec["sections"], name)
            self.assertFalse([c for c in spec["controls"] if "=" in c or c.endswith("/Calibration")], name)

    def test_audio_is_off_only_when_asked(self):
        quiet = rd.parse_ini(rd.profile_files(focus_safe=True, host="macos", audio=False)["Dolphin.ini"])
        self.assertEqual(quiet[("DSP", "Backend")], "No Audio Output")
        self.assertEqual(quiet[("DSP", "Volume")], "0")
        legacy_quiet = rd.parse_ini(rd.profile_files(audio=False)["Dolphin.ini"])
        self.assertEqual(legacy_quiet[("DSP", "Backend")], "No Audio Output")
        self.assertEqual(rd.DEFAULT_AUDIO, {"windows": True, "macos": False})

    def test_audit_passes_a_focus_safe_profile_and_flags_default_bindings(self):
        with tempfile.TemporaryDirectory() as temp:
            config = Path(temp)
            for name, text in rd.profile_files(focus_safe=True, host="macos").items():
                (config / name).write_text(text, encoding="ascii")
            audit = rd.input_binding_audit(config)
            self.assertEqual(audit["result"], "no_input_bound")
            self.assertEqual(len(audit["files_present"]), 6)
            # what Dolphin saves after a run of the focus-safe profile: Device lines and settings only
            (config / "GCPadNew.ini").write_text("[GCPad1]\nDevice = None/0/No Device\nMain Stick/Dead Zone = 5.0\n"
                                                 "Main Stick/Calibration = 100.00 141.42 100.00\n", encoding="ascii")
            self.assertEqual(rd.input_binding_audit(config)["result"], "no_input_bound")
            # Dolphin's own defaults when no file is written (the pre-HWI-077 profile on Windows)
            (config / "GCPadNew.ini").write_text("[GCPad1]\nDevice = DInput/0/Keyboard Mouse\nButtons/A = `X`\n",
                                                 encoding="ascii")
            (config / "Hotkeys.ini").write_text("[Hotkeys]\nDevice = None/0/No Device\nGeneral/Stop = ESCAPE\n"
                                                "Unknown/New Hotkey = @(Ctrl+O)\n", encoding="ascii")
            audit = rd.input_binding_audit(config)
            self.assertEqual(audit["result"], "input_bound")
            self.assertEqual(audit["devices_bound"], ["GCPadNew.ini:GCPad1"])
            self.assertEqual(audit["controls_bound"], ["GCPadNew.ini:GCPad1:Buttons/A",
                                                       "Hotkeys.ini:Hotkeys:General/Stop",
                                                       "Hotkeys.ini:Hotkeys:Unknown/New Hotkey"])


class EnvironmentTests(unittest.TestCase):
    def test_macos_adds_the_qt_and_sdl_variables_only(self):
        self.assertEqual(rd.launch_environment("macos", True),
                         {"QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM": "1", "QT_MAC_SET_RAISE_PROCESS": "0",
                          "SDL_JOYSTICK_MFI": "0"})
        self.assertEqual(rd.launch_environment("windows", True), {})
        self.assertEqual(rd.launch_environment("macos", False), {})

    def test_defaults_are_focus_safe_and_enforcing(self):
        base = ["--dolphin", "d", "--dol", "x.dol", "--out", "o"]
        args = rd.parse_args(base)
        self.assertTrue(args.focus_safe)
        self.assertEqual(args.focus_guard, "enforce")
        self.assertIsNone(args.audio)
        self.assertFalse(rd.parse_args(base + ["--no-audio"]).audio)
        self.assertTrue(rd.parse_args(base + ["--audio"]).audio)
        legacy = rd.parse_args(base + ["--no-focus-safe"])
        self.assertFalse(legacy.focus_safe)
        self.assertEqual(legacy.focus_guard, "off")

    def test_record_lists_names_and_values_without_paths(self):
        record = rd.focus_safety_record("macos", True, "enforce", False, rd.launch_environment("macos", True))
        self.assertEqual(record["launch_environment_added"]["QT_MAC_SET_RAISE_PROCESS"], "0")
        self.assertEqual(record["dolphin_ini_added"]["DSP"], ["Backend = No Audio Output", "Volume = 0"])
        self.assertEqual(record["dolphin_ini_added"]["AutoUpdate"], ["UpdateTrack ="])
        self.assertEqual(len(record["input_configs"]), 6)
        self.assertEqual(record["focus_guard"]["acts_on"], "this run's own Dolphin process tree only")
        self.assertNotIn("/", json.dumps(record["window"]).replace("NSRunningApplication", ""))
        self.assertEqual(json.dumps(record, sort_keys=True), json.dumps(
            rd.focus_safety_record("macos", True, "enforce", False, rd.launch_environment("macos", True)),
            sort_keys=True))
        legacy = rd.focus_safety_record("windows", False, "off", True, {})
        self.assertEqual((legacy["enabled"], legacy["input_configs"], legacy["dolphin_ini_added"]), (False, [], {}))


class FakeProbe:
    """Scripted focus queries: front/focus are lists consumed per sample (last value repeats)."""

    def __init__(self, front, focus=(None,), states=None, fail=()):
        self.front, self.focus, self.states, self.fail = list(front), list(focus), states or {}, set(fail)
        self.calls, self.hidden_requests = 0, []

    def _next(self, values):
        return values.pop(0) if len(values) > 1 else values[0]

    def front_pid(self):
        self.calls += 1
        if self.calls in self.fail:
            raise RuntimeError("query failed")
        return self._next(self.front)

    def focus_pid(self):
        return self._next(self.focus)

    def app_state(self, pid, hide=False):
        state = dict(self.states.get(pid, {"hidden": True, "active": False}))
        if hide and not state["hidden"]:
            self.hidden_requests.append(pid)
            state["hide_requested"] = True
            self.states[pid] = {"hidden": True, "active": state["active"]}
        return state

    def visible_windows(self, pids):
        return None


class FocusGuardTests(unittest.TestCase):
    def owned(self, others=(900,), children=(), host_name="macos"):
        clock = Clock()
        process = FakeProcess(500, clock, exit_at=None)
        host = Host("Dolphin", process, others, children)
        owned = rd.OwnedDolphin(process, "Dolphin", host_name, host.table, kill=host.kill, poll=2.0, clock=clock)
        owned.snapshot()
        return owned, host, process

    def test_quiet_run_is_sampled_and_nothing_is_stopped(self):
        owned, host, process = self.owned()
        guard = rd.FocusGuard(owned, FakeProbe(front=[42]), clock=Clock())
        for _ in range(5):
            self.assertIsNone(guard.sample())
        summary = guard.summary()
        self.assertEqual((summary["samples"], summary["own_frontmost"], summary["violation"]), (5, 0, False))
        self.assertIsNone(summary["stopped_dolphin"])
        self.assertFalse(process.killed)
        self.assertEqual(host.killed, [])

    def test_own_dolphin_frontmost_is_stopped_through_its_handle(self):
        owned, host, process = self.owned()
        guard = rd.FocusGuard(owned, FakeProbe(front=[42, 500]), clock=Clock())
        guard.sample()
        self.assertEqual(guard.sample(), "frontmost")
        self.assertTrue(process.killed)
        self.assertEqual(host.killed, [])  # never by PID lookup, never another lane
        summary = guard.summary()
        self.assertEqual(summary["stopped_dolphin"], {"reason": "frontmost", "pid_role": "root", "after_seconds": 0.0})
        self.assertTrue(summary["violation"])
        guard.sample()  # a second sample never stops again
        self.assertEqual(summary["own_frontmost"], 1)

    def test_another_lanes_dolphin_frontmost_is_counted_never_stopped(self):
        owned, host, process = self.owned(others=(900,))
        guard = rd.FocusGuard(owned, FakeProbe(front=[900], focus=[900]), clock=Clock())
        for _ in range(3):
            self.assertIsNone(guard.sample())
        self.assertFalse(process.killed)
        self.assertEqual(host.killed, [])
        self.assertIn(900, host.table())
        summary = guard.summary()
        self.assertEqual((summary["other_dolphin_frontmost"], summary["violation"]), (3, False))

    def test_own_descendant_with_focus_is_stopped_by_explicit_pid_only(self):
        owned, host, process = self.owned(children=((501, None),))
        self.assertIn(501, owned.owned)
        guard = rd.FocusGuard(owned, FakeProbe(front=[42], focus=[501]), clock=Clock())
        self.assertEqual(guard.sample(), "keyboard_focus")
        self.assertEqual(host.killed, [501])
        self.assertFalse(process.killed)
        self.assertEqual(guard.summary()["stopped_dolphin"]["pid_role"], "descendant")

    def test_kill_refuses_a_pid_outside_the_tree(self):
        owned, host, _ = self.owned(others=(900,))
        guard = rd.FocusGuard(owned, FakeProbe(front=[42]), clock=Clock())
        with self.assertRaises(ValueError):
            guard.stop_own(900, "frontmost")
        self.assertEqual(host.killed, [])

    def test_unhidden_dolphin_is_hidden_and_an_active_one_is_stopped(self):
        owned, host, process = self.owned()
        probe = FakeProbe(front=[42], states={500: {"hidden": False, "active": False}})
        guard = rd.FocusGuard(owned, probe, clock=Clock())
        guard.sample()
        guard.sample()
        self.assertEqual(probe.hidden_requests, [500])
        summary = guard.summary()
        self.assertEqual((summary["hide_requests"], summary["hide_failures"], summary["own_unhidden"]), (1, 0, 1))
        self.assertFalse(process.killed)
        probe.states[500] = {"hidden": True, "active": True}
        self.assertEqual(guard.sample(), "active")
        self.assertTrue(process.killed)

    def test_observe_mode_never_hides_or_stops(self):
        owned, host, process = self.owned()
        probe = FakeProbe(front=[500], focus=[500], states={500: {"hidden": False, "active": True}})
        guard = rd.FocusGuard(owned, probe, mode="observe", clock=Clock())
        self.assertEqual(guard.sample(), "frontmost")
        self.assertEqual(probe.hidden_requests, [])
        self.assertFalse(process.killed)
        summary = guard.summary()
        self.assertEqual((summary["own_frontmost"], summary["own_keyboard_focus"], summary["own_active"]), (1, 1, 1))
        self.assertIsNone(summary["stopped_dolphin"])

    def test_query_errors_are_counted_not_fatal(self):
        owned, _, process = self.owned()
        guard = rd.FocusGuard(owned, FakeProbe(front=[42], fail={1, 2}), clock=Clock())
        for _ in range(3):
            guard.sample()
        self.assertEqual(guard.summary()["query_errors"], 2)
        self.assertEqual(guard.summary()["no_frontmost"], 2)
        self.assertFalse(process.killed)

    def test_thread_samples_until_finished(self):
        owned, _, process = self.owned()
        guard = rd.FocusGuard(owned, FakeProbe(front=[42]), interval=0.01).start()
        import time
        deadline = time.monotonic() + 5
        while guard.counts["samples"] < 3 and time.monotonic() < deadline:
            time.sleep(0.01)
        summary = guard.finish()
        self.assertGreaterEqual(summary["samples"], 3)
        self.assertFalse(guard.thread.is_alive())
        self.assertFalse(process.killed)


class ProbeTests(unittest.TestCase):
    def test_lsappinfo_front_is_parsed(self):
        replies = iter([subprocess.CompletedProcess([], 0, stdout="ASN:0x0-0x1af1af:\n", stderr=""),
                        subprocess.CompletedProcess([], 0, stdout='"Dolphin" ASN:0x0-0x1af1af: (in front)\n'
                                                                    "    pid = 30381 type=\"Foreground\"\n", stderr=""),
                        subprocess.CompletedProcess([], 0, stdout="[ NULL ]\n", stderr="")])
        calls = []

        def run(argv, **kwargs):
            calls.append(argv)
            return next(replies)

        probe = rd.MacFocusProbe(run=run)
        self.assertEqual(probe.front_pid(), 30381)
        self.assertEqual(calls[1], ["/usr/bin/lsappinfo", "info", "-only", "pid", "ASN:0x0-0x1af1af:"])
        self.assertIsNone(probe.front_pid())
        failed = rd.MacFocusProbe(run=lambda argv, **k: subprocess.CompletedProcess([], 1, stdout="", stderr=""))
        with self.assertRaises(RuntimeError):
            failed.front_pid()

    @unittest.skipUnless(rd.host_platform() == "macos", "lsappinfo and AppKit exist only on macOS")
    def test_real_macos_probe_answers_read_only(self):
        import os
        probe = rd.MacFocusProbe()
        front = probe.front_pid()
        self.assertTrue(front is None or isinstance(front, int))
        self.assertIsNotNone(probe.objc)
        self.assertIsNone(probe.app_state(os.getpid()))  # this Python has no application record
        if front:
            state = probe.app_state(front)  # hide=False: only read
            self.assertTrue(state is None or set(state) == {"hidden", "active"})

    @unittest.skipUnless(rd.host_platform() == "windows", "user32 exists only on Windows")
    def test_real_windows_probe_answers(self):
        probe = rd.WindowsFocusProbe()
        for value in (probe.front_pid(), probe.focus_pid()):
            self.assertTrue(value is None or isinstance(value, int))
        self.assertEqual(probe.visible_windows({0xFFFFFFF0}), 0)
        self.assertIsNone(probe.app_state(1, hide=True))


class MainTests(unittest.TestCase):
    """main() with the process table, Popen, focus queries and the OS query mocked."""

    def run_main(self, host_name, extra, probe=None, exit_at=0.0):
        image = "Dolphin.exe" if host_name == "windows" else "Dolphin"
        temp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(temp, ignore_errors=True))
        exe, dol = temp / image, temp / "lane.dol"
        exe.write_bytes(b"dolphin")
        dol.write_bytes(b"dol")
        clock = Clock()
        state = {"process": None, "popen": []}
        pids = iter(range(500, 600))

        def table(host=None):
            rows = {1: (0, "init"), 900: (1, image)}
            process = state["process"]
            if process is not None and process.poll() is None:
                rows[process.pid] = (1, image)
            return rows

        def popen(argv, **kwargs):
            state["process"] = FakeProcess(next(pids), clock, exit_at=exit_at)
            state["popen"].append(kwargs)
            return state["process"]

        stability = {"observation": "no_instability_events_observed", "dolphin_application_events": []}
        argv = ["--dolphin", str(exe), "--dol", str(dol), "--out", str(temp / "out"), "--runs", "2",
                "--event-settle", "0", "--allow-concurrent"] + extra
        probe = probe or FakeProbe(front=[42])
        with mock.patch.object(rd, "host_platform", return_value=host_name), \
                mock.patch.object(rd, "process_table", side_effect=table), \
                mock.patch.object(rd.subprocess, "Popen", side_effect=popen), \
                mock.patch.object(rd, "launch_options", return_value={}), \
                mock.patch.object(rd, "host_record", return_value={"os": host_name}), \
                mock.patch.object(rd, "focus_probe", return_value=probe), \
                mock.patch.object(rd, "kill_pid") as kill, \
                mock.patch.object(rd, "query_host_os", return_value=stability), \
                mock.patch("builtins.print"):
            code = rd.main(argv)
        record = json.loads((temp / "out" / "runtime.json").read_text(encoding="utf-8"))
        config = {p.name: p.read_text(encoding="ascii") for p in (temp / "out" / "config").iterdir()}
        return code, record, config, state["popen"], kill

    def test_macos_default_launch_is_focus_safe_and_recorded(self):
        code, record, config, popen, kill = self.run_main("macos", [])
        self.assertEqual(code, 0)
        kill.assert_not_called()
        for kwargs in popen:
            env = kwargs["env"]
            self.assertEqual(env["QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM"], "1")
            self.assertEqual(env["QT_MAC_SET_RAISE_PROCESS"], "0")
            self.assertEqual(env["SDL_JOYSTICK_MFI"], "0")
        safety = record["focus_safety"]
        self.assertTrue(safety["enabled"])
        self.assertFalse(safety["audio_output"])
        self.assertEqual(safety["focus_guard"]["mode"], "enforce")
        self.assertEqual(len(config), 8)
        self.assertIn("Backend = No Audio Output", config["Dolphin.ini"])
        self.assertEqual(record["profile"]["config_sha256"]["Hotkeys.ini"],
                         hashlib.sha256(config["Hotkeys.ini"].encode()).hexdigest())
        for run in record["runs"]:
            self.assertEqual(run["focus_guard"]["mode"], "enforce")
            self.assertFalse(run["focus_guard"]["violation"])
            self.assertEqual(run["input_binding_audit"]["result"], "no_input_bound")
            self.assertEqual(run["host"]["outcome"], "exit_zero")
        self.assertEqual(record["launch_shape"][0], "Dolphin")  # unchanged shape

    def test_windows_default_keeps_audio_and_adds_no_environment(self):
        code, record, config, popen, _ = self.run_main("windows", [])
        self.assertEqual(code, 0)
        self.assertTrue(all(kwargs["env"] is None for kwargs in popen))
        self.assertTrue(record["focus_safety"]["audio_output"])
        self.assertNotIn("[DSP]", config["Dolphin.ini"])
        self.assertNotIn("SDL_Hints", config["Dolphin.ini"])
        self.assertIn("BackgroundInput = False", config["Dolphin.ini"])

    def test_legacy_launch_has_no_guard_and_old_files(self):
        code, record, config, popen, _ = self.run_main("macos", ["--no-focus-safe"])
        self.assertEqual(code, 0)
        self.assertEqual(sorted(config), ["Dolphin.ini", "GFX.ini", "WiimoteNew.ini"])
        self.assertTrue(all(kwargs["env"] is None for kwargs in popen))
        self.assertFalse(record["focus_safety"]["enabled"])
        self.assertTrue(all("focus_guard" not in run for run in record["runs"]))

    def test_unavailable_guard_refuses_to_launch(self):
        temp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(temp, ignore_errors=True))
        (temp / "Dolphin").write_bytes(b"dolphin")
        (temp / "x.dol").write_bytes(b"dol")
        argv = ["--dolphin", str(temp / "Dolphin"), "--dol", str(temp / "x.dol"), "--out", str(temp / "out")]
        with mock.patch.object(rd, "host_platform", return_value="macos"), \
                mock.patch.object(rd, "process_table", return_value={1: (0, "init")}), \
                mock.patch.object(rd, "focus_probe", side_effect=OSError("no AppKit")), \
                mock.patch.object(rd.subprocess, "Popen") as popen, \
                mock.patch("builtins.print"):
            self.assertEqual(rd.main(argv), 2)
        popen.assert_not_called()
        self.assertFalse((temp / "out").exists())

    def test_guard_stop_fails_the_run_and_skips_later_launches(self):
        probe = FakeProbe(front=[500])  # the first launch's own PID is frontmost
        code, record, _, _, kill = self.run_main("macos", [], probe=probe, exit_at=None)
        kill.assert_not_called()  # the root is stopped through its handle; PID 900 untouched
        self.assertEqual(code, 1)  # a guest failure, as for a forced stop; the host outcome names the guard
        self.assertEqual(len(record["runs"]), 1)
        run = record["runs"][0]
        self.assertEqual(run["host"]["outcome"], "focus_guard_stop")
        self.assertIn("focus_guard_stop_before_natural_exit", run["guest"]["failures"])
        self.assertEqual(run["focus_guard"]["stopped_dolphin"]["reason"], "frontmost")
        self.assertIn("focus guard", record["aborted"])
        self.assertEqual(run["host"]["teardown_check"], "own_process_tree_exited")


if __name__ == "__main__":
    unittest.main()
