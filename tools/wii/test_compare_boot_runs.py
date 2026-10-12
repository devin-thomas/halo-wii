"""Tests of compare_boot_runs.py on authored log lines (HWI-033)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_boot_runs as cbr  # noqa: E402

WII = """BOOT_MAP frame=0 map=ui scenario_tag=0 kind=menus changes=1 game_time=0
BOOT_SCREEN frame=44 clock_ms=1485 map=ui screen_tag=271 focused_tag=276 efb_crc=5ef90bc5 covered=127 state=11111111 state_without_scripts=22222222 game_time=43
BOOT_STATE_ALLOCATION at=first_screen frame=44 index=0 digest=aaaa0000 name=a
BOOT_STATE_ALLOCATION at=first_screen frame=44 index=1 digest=bbbb0000 name=hs thread
BOOT_MAP frame=598 map=a10 scenario_tag=0 kind=campaign changes=2 game_time=1
BOOT_TICK map=a10 tick=1 game_time=1 frame=597 state=33333333 state_without_scripts=44444444 bsp=0
BOOT_TICK map=a10 tick=2 game_time=2 frame=598 state=55555555 state_without_scripts=66666666 bsp=0
BOOT_ENVIRONMENT first frame=598 clock_ms=1 map=a10 efb_crc=0 covered=0 state=77777777 game_time=2 drawing=pending_hwi030
END target=engine mode=boot build=x reason=campaign_reached_drawing_pending elapsed_ms=1 result=pass
"""


class CompareTests(unittest.TestCase):
    def test_equal_logs(self):
        host = WII.replace("efb_crc=5ef90bc5 covered=127", "efb_crc=00000000 covered=0")
        result = cbr.compare(cbr.parse(WII), cbr.parse(host))
        self.assertEqual((result["screens"], result["screens_state_equal"]), (1, 1))
        self.assertEqual((result["ticks"], result["ticks_equal"], result["first_unequal_tick"]), (2, 2, None))
        self.assertTrue(result["environment"]["first"]["equal"])
        self.assertEqual(result["environment"]["first"]["drawing"], "pending_hwi030")
        self.assertEqual(result["allocations"]["first_screen"]["equal"], 2)
        self.assertEqual(result["wii_maps"][1]["kind"], "campaign")

    def test_differences_are_named(self):
        host = WII.replace("state=55555555", "state=55555556").replace("digest=bbbb0000", "digest=bbbb0001")
        result = cbr.compare(cbr.parse(WII), cbr.parse(host))
        self.assertEqual(result["first_unequal_tick"], 2)
        self.assertEqual(result["allocations"]["first_screen"]["differing"], ["hs thread"])

    def test_missing_lines_are_not_equal(self):
        host = "\n".join(line for line in WII.splitlines() if not line.startswith("BOOT_TICK map=a10 tick=2"))
        result = cbr.compare(cbr.parse(WII), cbr.parse(host))
        self.assertEqual(result["ticks_equal"], 1)


if __name__ == "__main__":
    unittest.main()
