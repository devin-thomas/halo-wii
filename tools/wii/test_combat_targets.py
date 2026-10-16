"""HWI-041: shootable targets in the real-map run (port/wii/engine/combat_targets*.c).

The host check (combat_targets_check.c) runs the targets' states and events, compiled whole: the plan's
parse and refusals, the harness's hit schedule, spawn, hits by the harness and by the world, a shield's
recharge (no hit), death by the dead bit, a body hit again (no hit), a destruction outright, removal,
refusals, and the targets' places. Each DIGEST it prints is recomputed here from its EVENT lines with zlib's
CRC-32, so the event encoding (big-endian words, IEEE bits) is checked apart from the C.

The static checks tie the engine side to the engine's own machinery: targets made by object_new from the
player's own unit definition, hits dealt by object_cause_damage with the player's weapon's projectile impact
damage filled as projectiles.c fills it, vitality and death only read (never written), nothing outside an
armed real-map run with a plan that asks, and the build's lists.
"""
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "port" / "wii" / "engine"
TRACK = ENGINE / "combat_targets.c"
SCENARIO = ENGINE / "combat_targets_scenario.c"
RUN = ENGINE / "engine_map_run.c"
CHECK = ROOT / "tools" / "wii" / "combat_targets_check.c"
PROJECTILES = ROOT / "source" / "items" / "projectiles.c"
PLAYERS = ROOT / "source" / "game" / "players.c"
ENGINE_BUILD = ROOT / "tools" / "wii" / "engine_build.py"


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


def _text(path):
    return path.read_text(encoding="utf-8")


def _function(text, name):
    """a C function definition, from the line naming it to its closing brace"""
    match = re.search(r"^(?:static )?[A-Za-z_][\w \*]*\b%s\(\n[^;{]*\)\n\{" % re.escape(name), text, re.M)
    assert match, name
    index = match.end() - 1
    depth = 0
    while True:
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
        if depth == 0:
            return text[match.start():index]


class HostCheck(unittest.TestCase):
    output = None

    @classmethod
    def setUpClass(cls):
        cc = compiler()
        if not cc:
            raise unittest.SkipTest("no host C compiler")
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "combat_targets_check.exe"
            subprocess.run([cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(program), str(CHECK),
                            str(TRACK), "-lm"], check=True, capture_output=True, text=True)
            run = subprocess.run([str(program)], capture_output=True, text=True, timeout=60)
        cls.returncode = run.returncode
        cls.output = run.stdout.splitlines()

    def test_every_check_passes(self):
        checks = [line.split() for line in self.output if line.startswith("CHECK ")]
        failed = [name for _, name, result in checks if result != "ok"]
        self.assertEqual(failed, [])
        summary = [line for line in self.output if line.startswith("CHECKS ")]
        self.assertEqual(len(summary), 1)
        passed = int(re.search(r"passed=(\d+)", summary[0]).group(1))
        self.assertGreaterEqual(passed, 50)
        self.assertEqual(passed, len(checks))
        self.assertIn("failed=0", summary[0])
        self.assertEqual(self.returncode, 0)
        print("combat_targets_check: %d checks passed" % passed)

    def test_digests_recomputed(self):
        """each DIGEST is CRC-32 of its EVENT lines' eleven big-endian words, in order"""
        words = []
        digests = {}
        for line in self.output:
            if line.startswith("EVENT "):
                words.extend(int(word, 16) for word in line.split()[1:])
                self.assertEqual(len(line.split()), 12)
            elif line.startswith("DIGEST "):
                _, name, value = line.split()
                digests[name] = (zlib.crc32(struct.pack(">%dI" % len(words), *words)), int(value, 16), len(words))
                words = []
        self.assertEqual(set(digests), {"lifecycle", "refusals"})
        for name, (expected, printed, count) in digests.items():
            self.assertGreater(count, 0, name)
            self.assertEqual(printed, expected, name)
        self.assertEqual(digests["lifecycle"][2], 9 * 11)
        self.assertEqual(digests["refusals"][2], 3 * 11)


class EngineSeam(unittest.TestCase):
    def setUp(self):
        self.scenario = _text(SCENARIO)
        self.code = re.sub(r"/\*.*?\*/", "", self.scenario, flags=re.S)

    def test_targets_made_as_player_spawning_makes_units(self):
        spawn = _function(self.code, "targets_spawn")
        self.assertIn("object_placement_data_new(&placement, definition_index, NONE);", spawn)
        self.assertIn("definition_index = unit->definition_index;", spawn)
        self.assertIn("object_index = object_new(&placement);", spawn)
        self.assertIn("placement.owner_team_index = _game_team_covenant;", spawn)
        self.assertIn("COMBAT_TARGETS_REFUSAL_NO_ROOM", spawn)
        self.assertIn("COMBAT_TARGETS_REFUSAL_NOT_DAMAGEABLE", spawn)
        self.assertIn("COMBAT_TARGETS_REFUSAL_NO_PLAYER_UNIT", spawn)
        # the player's own unit is made the same way (players.c)
        players = _text(PLAYERS)
        self.assertIn("unit_index = object_new(&placement_data);", players)

    def test_hits_filled_as_a_projectile_impact(self):
        hit = _function(self.code, "target_hit")
        projectiles = _text(PROJECTILES)
        for statement in ("SET_FLAG(damage.flags, _damage_from_weapon_bit, TRUE);",):
            self.assertIn(statement, hit)
            self.assertIn(statement, projectiles)
        self.assertIn("damage_data_new(&damage, combat_targets_globals.damage_definition_index);", hit)
        self.assertIn("damage_data_new(&damage, definition->projectile.impact_damage.index);", projectiles)
        for field in ("owner_player_index", "owner_object_index", "owner_team_index", "epicenter", "origin",
                      "direction"):
            self.assertRegex(hit, r"damage\.%s = " % field)
        self.assertIn("object_cause_damage(&damage, object_index, NONE, NONE, NONE, NULL);", hit)
        impact = _function(self.code, "weapon_impact_damage")
        self.assertIn("weapon_definition->weapon.triggers", impact)
        self.assertIn("projectile_definition->projectile.impact_damage.index", impact)

    def test_no_parallel_health(self):
        """vitality, damage flags and death are read from the object, never written"""
        for field in ("body_vitality", "shield_vitality", "damage_flags", "maximum_body_vitality",
                      "current_body_damage", "recent_body_damage"):
            self.assertNotRegex(self.code, r"object\.%s\s*(?:[-+*/|&]?=)(?!=)" % field, field)
        for call in ("object_deplete_body", "object_destroy(", "unit_kill", "object_restore_body",
                     "object_initialize_vitality", "SET_FLAG(object"):
            self.assertNotIn(call, self.code)
        observation = _function(self.code, "observation_get")
        self.assertIn("TEST_FLAG(object->object.damage_flags, _object_dead_bit)", observation)
        track = re.sub(r"/\*.*?\*/", "", _text(TRACK), flags=re.S)
        self.assertNotIn("#include \"cseries.h\"", track)
        self.assertNotIn("objects/", track)

    def test_only_in_an_armed_run_that_asks(self):
        after = _function(self.code, "combat_targets_scenario_after_tick")
        self.assertIn("if (!combat_targets_globals.plan.spawn_tick || !real_map_armed())", after)
        run = _text(RUN)
        self.assertIn("combat_targets_scenario_set_plan(&line->targets);", run)
        self.assertIn("memset(&warm_line.targets, 0, sizeof(warm_line.targets));", run)
        self.assertIn("combat_targets_scenario_after_tick(tick);", _function_platform(run, "tick_observer"))
        self.assertIn("combat_targets_parse(line->targets_text, line->ticks, &line->targets)", run)
        # ordinary play (the boot mode, the full game) never makes them
        for name in ("boot_main.c", "boot_hooks.c", "wii_engine_main.c", "engine_hooks.c", "real_map_scenario.c"):
            self.assertNotIn("combat_targets", _text(ENGINE / name), name)

    def test_build_lists(self):
        build = _text(ENGINE_BUILD)
        authored = build[build.index("ENGINE_AUTHORED = "):build.index("# Linux platform files")]
        platform = build[build.index("WII_PLATFORM = "):build.index("WII_HEADERS = ")]
        self.assertIn('ENGINE_DIR / "combat_targets_scenario.c"', authored)
        self.assertIn('"combat_targets.c"', platform)


def _function_platform(text, name):
    """a platform-side function (K&R-less one-line signature)"""
    match = re.search(r"^static void %s\([^)]*\)\n\{" % re.escape(name), text, re.M)
    assert match, name
    index = match.end() - 1
    depth = 0
    while True:
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
        if depth == 0:
            return text[match.start():index]


if __name__ == "__main__":
    unittest.main()
