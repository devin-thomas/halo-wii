"""PID-scoped Dolphin ownership and concurrent lanes (HWI-073): no emulator is launched.

Process queries are mocked for both hosts: the Windows Toolhelp snapshot
(rd.windows_process_table) and macOS `ps` (subprocess.run).
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import run_dolphin as rd


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FakeProcess:
    """A launched Dolphin: exits at exit_at (fake clock) or never; kill() ends it through its handle."""

    def __init__(self, pid, clock, exit_at=None, status=0):
        self.pid, self.clock, self.exit_at, self.status = pid, clock, exit_at, status
        self.returncode, self.killed = None, False

    def poll(self):
        if self.returncode is None and self.exit_at is not None and self.clock.now >= self.exit_at:
            self.returncode = self.status
        return self.returncode

    def wait(self, timeout=None):
        if self.poll() is not None:
            return self.returncode
        if self.exit_at is not None and (timeout is None or self.clock.now + timeout >= self.exit_at):
            self.clock.now = max(self.clock.now, self.exit_at)
            return self.poll()
        if timeout is None:
            raise AssertionError("wait() without timeout on a process that never exits")
        self.clock.now += timeout
        raise subprocess.TimeoutExpired("Dolphin", timeout)

    def kill(self):
        self.killed = True
        self.returncode = 1


class Host:
    """A mutable process table: the root is present while the fake process runs."""

    def __init__(self, image, process, others=(), children=()):
        self.image, self.process = image, process
        self.others = {pid: (1, image) for pid in others}
        self.children = {pid: (process.pid, image if name is None else name) for pid, name in children}
        self.killed = []

    def table(self):
        table = {1: (0, "launchd"), 4: (1, "System"), **self.others, **self.children}
        if self.process.poll() is None:
            table[self.process.pid] = (1, self.image)
        return table

    def kill(self, pid, host):
        self.killed.append(pid)
        self.children.pop(pid, None)
        self.others.pop(pid, None)


class QuietProbe:
    """Focus queries for main(): another application is always frontmost (HWI-077 guard)."""

    def front_pid(self):
        return 42

    def focus_pid(self):
        return None

    def app_state(self, pid, hide=False):
        return None

    def visible_windows(self, pids):
        return None


class ProcessQueryTests(unittest.TestCase):
    def test_macos_ps_parsing_keeps_base_names_and_skips_noise(self):
        text = ("  PID  PPID COMM\n    1     0 /sbin/launchd\n  500     1 /Users/x/Applications/Dolphin.app/"
                "Contents/MacOS/Dolphin\n  501   500 /System/Library/Helper With Space\n  bogus\n")
        table = rd.parse_ps(text)
        self.assertEqual(table[500], (1, "Dolphin"))
        self.assertEqual(table[501], (500, "Helper With Space"))
        self.assertNotIn("bogus", str(table))

    def test_macos_query_runs_ps_and_a_failed_query_raises(self):
        ok = subprocess.CompletedProcess([], 0, stdout="500 1 /a/Dolphin\n600 1 /b/Dolphin\n", stderr="")
        with mock.patch.object(rd.subprocess, "run", return_value=ok) as run:
            table = rd.process_table("macos")
        self.assertEqual(run.call_args[0][0], ["ps", "-A", "-o", "pid=,ppid=,comm="])
        self.assertEqual(rd.dolphin_pids(table, "Dolphin", "macos"), {500, 600})
        failed = subprocess.CompletedProcess([], 1, stdout="", stderr="no")
        with mock.patch.object(rd.subprocess, "run", return_value=failed):
            with self.assertRaises(RuntimeError):
                rd.process_table("macos")

    def test_windows_query_uses_the_snapshot_and_names_are_case_insensitive(self):
        snapshot = {4: (0, "System"), 700: (4, "dolphin.exe"), 701: (700, "Dolphin.exe"), 800: (4, "DolphinTool.exe")}
        with mock.patch.object(rd, "windows_process_table", return_value=snapshot):
            table = rd.process_table("windows")
        self.assertEqual(rd.dolphin_pids(table, "Dolphin.exe", "windows"), {700, 701})
        self.assertEqual(rd.dolphin_pids({1: (0, "dolphin")}, "Dolphin", "macos"), set())

    def test_process_tree_follows_parent_links_only(self):
        table = {1: (0, "init"), 10: (1, "Dolphin"), 11: (10, "helper"), 12: (11, "grandchild"),
                 20: (1, "Dolphin"), 21: (20, "helper"), 30: (30, "self-parent")}
        self.assertEqual(rd.process_tree(table, 10), {10, 11, 12})
        self.assertEqual(rd.process_tree(table, 99), set())
        self.assertEqual(rd.process_tree(table, 30), {30})

    @unittest.skipUnless(rd.host_platform() == "windows", "real Toolhelp snapshot exists only on Windows")
    def test_real_windows_snapshot_contains_this_process(self):
        import os
        table = rd.windows_process_table()
        self.assertIn(os.getpid(), table)
        self.assertEqual(table[os.getpid()][0], os.getppid())


class OwnershipTests(unittest.TestCase):
    def owned(self, host_name="windows", image="Dolphin.exe", exit_at=5.0, others=(), children=(), timeout=None):
        clock = Clock()
        process = FakeProcess(500, clock, exit_at)
        host = Host(image, process, others, children)
        owned = rd.OwnedDolphin(process, image, host_name, host.table, kill=host.kill, poll=2.0, clock=clock)
        return owned, host, process

    def test_teardown_of_own_pid_while_another_dolphin_lives(self):
        for host_name, image in (("windows", "Dolphin.exe"), ("macos", "Dolphin")):
            with self.subTest(host=host_name):
                owned, host, process = self.owned(host_name, image, others=(900,))
                status, forced = owned.wait(60)
                self.assertEqual((status, forced), (0, False))
                teardown = owned.teardown()
                self.assertEqual(teardown["teardown_check"], "own_process_tree_exited")
                self.assertEqual(teardown["own_pid"], 500)
                self.assertEqual(teardown["own_tree_alive_after"], [])
                self.assertEqual(teardown["other_dolphin_processes_at_end"], 1)
                self.assertEqual(teardown["dolphin_processes_after"], 1)  # host-wide, as before
                self.assertEqual(owned.others_max, 1)
                self.assertEqual(host.killed, [])
                self.assertIn(900, host.table())

    def test_forced_stop_never_touches_a_foreign_pid(self):
        for host_name, image in (("windows", "Dolphin.exe"), ("macos", "Dolphin")):
            with self.subTest(host=host_name):
                owned, host, process = self.owned(host_name, image, exit_at=None, others=(900, 901),
                                                  children=((501, None),))
                status, forced = owned.wait(10)
                self.assertTrue(forced)
                self.assertTrue(process.killed)  # the root, through its own handle
                self.assertEqual(host.killed, [501])  # only the observed child of our root
                self.assertIn(900, host.table())
                self.assertIn(901, host.table())
                teardown = owned.teardown()
                self.assertEqual(teardown["teardown_check"], "own_process_tree_exited")
                self.assertEqual(teardown["own_tree_observed"], [500, 501])
                self.assertEqual(teardown["other_dolphin_processes_at_end"], 2)

    def test_kill_owned_refuses_a_pid_outside_the_tree(self):
        killed = []
        with self.assertRaises(ValueError):
            rd.kill_owned([501, 900], {500: "Dolphin", 501: "Dolphin"}, "macos", lambda pid, host: killed.append(pid))
        self.assertEqual(killed, [])  # nothing is stopped when any PID is foreign

    def test_kill_pid_targets_exactly_one_pid_on_each_host(self):
        with mock.patch.object(rd.subprocess, "run") as run:
            rd.kill_pid(501, "windows")
        self.assertEqual(run.call_args[0][0], ["taskkill", "/PID", "501", "/F"])  # no /T tree walk
        with mock.patch.object(rd.os, "kill") as kill:
            rd.kill_pid(501, "macos")
        self.assertEqual(kill.call_args[0][0], 501)

    def test_leftover_own_child_is_stopped_and_fails_the_teardown_check(self):
        owned, host, process = self.owned(exit_at=5.0, others=(900,), children=((501, "crashpad.exe"),))
        owned.wait(60)
        with mock.patch.object(rd.time, "sleep"):
            teardown = owned.teardown()
        self.assertEqual(teardown["own_tree_alive_after"], [501])
        self.assertEqual(teardown["own_tree_stopped_after_exit"], [501])
        self.assertEqual(teardown["teardown_check"], "own_process_tree_remained_then_stopped")
        self.assertEqual(host.killed, [501])
        run = {"guest": {"result": "passed"}, "persistence": {"result": "passed"},
               "host": {"outcome": "exit_zero", **teardown}}
        record = {"requested_runs": 1, "runs": [run], "os_stability": {"observation": "no_instability_events_observed"}}
        self.assertEqual(rd.overall_exit(record), 3)

    def test_reused_pid_under_another_image_is_not_ours(self):
        owned = {500: "Dolphin", 501: "helper"}
        table = {501: (1, "somebody-else"), 900: (1, "Dolphin")}
        self.assertEqual(rd.owned_alive(table, owned, 500, "macos"), [])

    def test_failed_queries_make_teardown_unmeasured_not_clean(self):
        clock = Clock()
        process = FakeProcess(500, clock, exit_at=1.0)

        def broken():
            raise RuntimeError("ps failed")
        owned = rd.OwnedDolphin(process, "Dolphin", "macos", broken, poll=2.0, clock=clock)
        self.assertEqual(owned.wait(60), (0, False))
        teardown = owned.teardown()
        self.assertEqual(teardown["teardown_check"], "unmeasured_process_query_failed")
        self.assertGreater(owned.query_errors, 0)

    def test_old_records_without_lane_fields_keep_their_exit(self):
        run = {"guest": {"result": "passed"}, "persistence": {"result": "passed"},
               "host": {"outcome": "exit_zero", "dolphin_processes_after": 0}}
        record = {"requested_runs": 1, "runs": [run], "os_stability": {"observation": "no_instability_events_observed"}}
        self.assertEqual(rd.overall_exit(record), 0)


class LaneAttributionTests(unittest.TestCase):
    def test_windows_fault_events_name_their_lane_and_stay_host_wide(self):
        events = {"system": [], "application": [
            {"id": 1000, "provider": "Application Error", "time_utc": "2026-10-11T12:00:01Z",
             "message": "Faulting application name: Dolphin.exe, version: 1\nFaulting module name: ucrtbase.dll\n"
                        "Exception code: 0xc0000409\nFaulting process id: 0x1f4\nFaulting application path: C:\\x"},
            {"id": 1000, "provider": "Application Error", "time_utc": "2026-10-11T12:00:02Z",
             "message": "Faulting application name: Dolphin.exe\nFaulting process id: 0x384"},
            {"id": 1002, "provider": "Application Hang", "time_utc": "2026-10-11T12:00:03Z",
             "message": "The program Dolphin.exe stopped interacting"}]}
        result = rd.classify_events(events, "2026-10-11T11:59:00+00:00")
        rd.attribute_lanes(result, {500, 501}, True)
        lanes = [(e.get("process_id"), e["lane"]) for e in result["dolphin_application_events"]]
        self.assertEqual(lanes, [(500, "own"), (900, "other"), (None, "unattributed")])
        self.assertEqual(result["scope"], "host_wide")
        self.assertTrue(result["shared_with_concurrent_lanes"])
        self.assertNotIn("C:\\x", json.dumps(result))
        self.assertEqual(result["observation"], "no_instability_events_observed")

    def test_macos_log_entries_keep_their_process_id(self):
        entries = [{"timestamp": "t", "processImagePath": "/x/Dolphin.app/Contents/MacOS/Dolphin", "processID": 900,
                    "eventMessage": "fault"}]
        result = rd.classify_macos([("Dolphin-2026.ips", "t")], entries, None, "2026-10-11T11:59:00+00:00")
        rd.attribute_lanes(result, {500}, False)
        self.assertEqual([e["lane"] for e in result["dolphin_application_events"]], ["unattributed", "other"])
        self.assertFalse(result["shared_with_concurrent_lanes"])


class ConcurrentStartTests(unittest.TestCase):
    """main() with the process table, Popen and the OS query mocked; host is forced per case."""

    def run_main(self, host_name, extra, others=(900,)):
        image = "Dolphin.exe" if host_name == "windows" else "Dolphin"
        temp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(temp, ignore_errors=True))
        exe, dol = temp / image, temp / "lane.dol"
        exe.write_bytes(b"dolphin")
        dol.write_bytes(b"dol")
        clock = Clock()
        state = {"process": None, "others": set(others)}
        pids = iter(range(500, 600))

        def table(host=None):
            rows = {1: (0, "init"), **{pid: (1, image) for pid in state["others"]}}
            process = state["process"]
            if process is not None and process.poll() is None:
                rows[process.pid] = (1, image)
            return rows

        def popen(argv, **kwargs):
            state["process"] = FakeProcess(next(pids), clock, exit_at=clock.now)
            return state["process"]

        stability = {"observation": "no_instability_events_observed", "dolphin_application_events": []}
        argv = ["--dolphin", str(exe), "--dol", str(dol), "--out", str(temp / "out"), "--runs", "2",
                "--event-settle", "0"] + extra
        with mock.patch.object(rd, "host_platform", return_value=host_name), \
                mock.patch.object(rd, "process_table", side_effect=table), \
                mock.patch.object(rd.subprocess, "Popen", side_effect=popen), \
                mock.patch.object(rd, "launch_options", return_value={}), \
                mock.patch.object(rd, "host_record", return_value={"os": host_name}), \
                mock.patch.object(rd, "focus_probe", return_value=QuietProbe()), \
                mock.patch.object(rd, "kill_pid") as kill, \
                mock.patch.object(rd, "query_host_os", return_value=stability), \
                mock.patch("builtins.print"):
            code = rd.main(argv)
        record_path = temp / "out" / "runtime.json"
        record = json.loads(record_path.read_text(encoding="utf-8")) if record_path.exists() else None
        return code, record, kill

    def test_exclusive_mode_still_refuses_beside_another_dolphin(self):
        for host_name in ("windows", "macos"):
            with self.subTest(host=host_name):
                code, record, _ = self.run_main(host_name, [])
                self.assertEqual(code, 2)
                self.assertIsNone(record)

    def test_concurrent_lane_runs_beside_another_dolphin_and_records_it(self):
        for host_name in ("windows", "macos"):
            with self.subTest(host=host_name):
                code, record, kill = self.run_main(host_name, ["--allow-concurrent"])
                kill.assert_not_called()
                self.assertEqual(record["concurrency"]["mode"], "concurrent")
                self.assertEqual(record["concurrency"]["ownership"], "pid_scoped_process_tree")
                self.assertEqual(record["concurrency"]["other_dolphin_processes_at_start"], 1)
                self.assertEqual(record["concurrency"]["other_dolphin_processes_at_end"], 1)
                self.assertTrue(record["concurrency"]["host_shared"])
                self.assertEqual(record["concurrency"]["lanes_max"], 2)
                self.assertTrue(record["outcomes"]["host_shared"])
                self.assertEqual(record["os_stability"]["scope"], "host_wide")
                self.assertTrue(record["os_stability"]["shared_with_concurrent_lanes"])
                self.assertEqual(len(record["runs"]), 2)
                for run in record["runs"]:
                    host = run["host"]
                    self.assertEqual(host["teardown_check"], "own_process_tree_exited")
                    self.assertTrue(host["host_shared"])
                    self.assertEqual(host["other_dolphin_processes_at_start"], 1)
                    self.assertEqual(host["other_dolphin_processes_at_end"], 1)
                    self.assertEqual(host["dolphin_processes_after"], 1)
                    self.assertNotIn(900, host["own_tree_observed"])
                self.assertEqual(code, 0)

    def test_exclusive_lane_alone_records_an_unshared_host(self):
        code, record, kill = self.run_main("windows", [], others=())
        self.assertEqual(code, 0)
        self.assertEqual(record["concurrency"]["mode"], "exclusive")
        self.assertFalse(record["concurrency"]["host_shared"])
        self.assertEqual(record["concurrency"]["lanes_max"], 1)
        self.assertEqual([r["host"]["host_shared"] for r in record["runs"]], [False, False])
        self.assertEqual([r["host"]["dolphin_processes_after"] for r in record["runs"]], [0, 0])


if __name__ == "__main__":
    unittest.main()
