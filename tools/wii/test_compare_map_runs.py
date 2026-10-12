"""Tests of compare_map_runs.py's cadence invariance (HWI-016F), revert-class requests (HWI-034) and the
canonical digest's pointer places (HWI-079)."""
import unittest

import compare_map_runs as cmr


def run_line(cadence, cycle=0, sim_raw="11111111", chain="22222222", divergent="-1", name="a10"):
    return (f"RUN map={name} cycle={cycle} cadence={cadence} ticks=10 frames=10 sim=33333333 sim_raw={sim_raw} "
            f"sim_raw_chain={chain} first_divergent_tick={divergent} seed=1 objects=4 player_unit=e0000001 "
            f"position=1.0,2.0,3.0 tags_end=55555555 bsp=0 bsp_switches=0 sim_hwi015b=66666666 "
            f"sim_hwi015b_raw=77777777 sim_hwi015b_raw_chain=88888888 frame_effects=0 frame_sounds=1")


class CadenceInvariance(unittest.TestCase):
    def log(self, *lines):
        return cmr.parse("\n".join(lines) + "\n")

    def test_equal_runs_are_invariant(self):
        log = self.log(run_line("30hz", cycle=-1, sim_raw="99999999"), run_line("30hz"), run_line("60hz"),
                       run_line("vsync_measured"))
        result = cmr.cadence_invariance(log)["a10"]
        self.assertTrue(result["invariant"])
        self.assertEqual(result["runs"], 3)  # the warm-up (cycle -1) is not a run
        self.assertEqual(result["cadences"], ["30hz", "60hz", "vsync_measured"])

    def test_a_different_run_is_named(self):
        log = self.log(run_line("30hz"), run_line("60hz", chain="deadbeef", divergent="52"))
        result = cmr.cadence_invariance(log)["a10"]
        self.assertFalse(result["invariant"])
        self.assertEqual(result["different"], [{"cycle": 0, "cadence": "60hz",
                                                "keys": ["sim_raw_chain", "first_divergent_tick"]}])

    def test_maps_are_separate(self):
        log = self.log(run_line("30hz"), run_line("30hz", name="prisoner", sim_raw="abababab"))
        result = cmr.cadence_invariance(log)
        self.assertTrue(result["a10"]["invariant"] and result["prisoner"]["invariant"])

    def test_wii_and_host_compare_the_hwi015b_simulation(self):
        wii = self.log(run_line("30hz"))
        host = cmr.parse(run_line("30hz").replace("sim_hwi015b_raw=77777777", "sim_hwi015b_raw=00000000") + "\n")
        report = cmr.compare(wii, host)
        self.assertFalse(report["simulation_all_same"])
        self.assertEqual(report["runs"][0]["simulation_differences"], ["sim_hwi015b_raw"])


def stopped_line(cadence, after="0", clock="600", ran="598", carried="2", stopped="1"):
    # (HWI-034) a RUN line with the revert-class request fields
    return (run_line(cadence, name="a10rv") + f" requests=1 request_tick=51 ticks_after_request={after} "
            f"clock_ticks={clock} ticks_run={ran} carried={carried} frames_stopped={stopped} carried_total=6 "
            f"most_carried=6 queue_resets=1 core_saved_tick=-1")


class RevertClassRequests(unittest.TestCase):
    def test_a_stopped_frame_that_keeps_every_tick_passes(self):
        log = cmr.parse("\n".join([stopped_line("30hz", carried="0", ran="600", stopped="0"),
                                   stopped_line("irregular")]) + "\n")
        entries = cmr.revert_class(log)
        self.assertEqual([entry["conserved"] for entry in entries], [True, True])
        self.assertEqual(entries[1]["frames_stopped"], 1)
        self.assertTrue(cmr.revert_class_pass(entries))

    def test_a_tick_after_the_request_fails(self):
        log = cmr.parse(stopped_line("irregular", after="3") + "\n")
        self.assertFalse(cmr.revert_class_pass(cmr.revert_class(log)))

    def test_a_lost_tick_fails(self):
        log = cmr.parse(stopped_line("irregular", ran="597") + "\n")
        entries = cmr.revert_class(log)
        self.assertFalse(entries[0]["conserved"])
        self.assertFalse(cmr.revert_class_pass(entries))

    def test_logs_without_the_fields_are_left_out(self):
        log = cmr.parse(run_line("30hz") + "\n")
        self.assertEqual(cmr.revert_class(log), [])
        self.assertTrue(cmr.revert_class_pass([]))

    def test_wii_and_host_compare_the_counts(self):
        wii = cmr.parse(stopped_line("irregular") + "\n")
        host = cmr.parse(stopped_line("irregular").replace("carried_total=6", "carried_total=5") + "\n")
        report = cmr.compare(wii, host)
        self.assertEqual(report["runs"][0]["simulation_differences"], ["carried_total"])
        self.assertTrue(report["revert_class_pass"])


class PointerPlaces(unittest.TestCase):
    def line(self, places="2741", failed="0", values="4"):
        return run_line("30hz") + f" places={places} places_failed={failed} address_values={values}"

    def test_found_places_pass(self):
        log = cmr.parse(self.line() + "\n")
        entries = cmr.pointer_places(log)
        self.assertEqual(entries, [{"map": "a10", "cycle": 0, "cadence": "30hz", "places": 2741, "places_failed": 0,
                                    "address_values": 4}])
        self.assertTrue(cmr.pointer_places_pass(entries))

    def test_a_digest_without_places_fails(self):
        self.assertFalse(cmr.pointer_places_pass(cmr.pointer_places(cmr.parse(self.line(failed="3") + "\n"))))

    def test_logs_without_the_fields_are_left_out(self):
        self.assertEqual(cmr.pointer_places(cmr.parse(run_line("30hz") + "\n")), [])

    def test_wii_and_host_compare_the_places_not_the_values(self):
        wii = cmr.parse(self.line() + "\n")
        host = cmr.parse(self.line(places="2740", values="9") + "\n")
        report = cmr.compare(wii, host)
        self.assertEqual(report["runs"][0]["simulation_differences"], ["places"])
        self.assertTrue(report["pointer_places_pass"])


if __name__ == "__main__":
    unittest.main()
