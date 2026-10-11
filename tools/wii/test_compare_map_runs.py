"""Tests of compare_map_runs.py's cadence invariance (HWI-016F)."""
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


if __name__ == "__main__":
    unittest.main()
