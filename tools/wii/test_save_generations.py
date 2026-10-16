"""HWI-080: a saved game's generations (source/saved games/game_state_generations.c), fault-injected on the
host (save_generations_check.c), and the save entrypoints that use them (game_state_xbox.c)."""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
XBOX = ROOT / "source" / "saved games" / "game_state_xbox.c"


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


@unittest.skipUnless(compiler(), "no host C compiler")
class GenerationFaultTests(unittest.TestCase):
    def test_every_interruption_and_failure_leaves_a_checked_save(self):
        with tempfile.TemporaryDirectory() as temp:
            name = "save_generations_check.exe" if sys.platform == "win32" else "save_generations_check"
            exe = Path(temp) / name
            build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(ROOT / "source"), "-o", str(exe),
                                    str(ROOT / "tools" / "wii" / "save_generations_check.c")],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-4000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            match = re.fullmatch(r"ok (\d+) cases\s*", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            self.assertGreater(int(match.group(1)), 50)


class EntrypointTests(unittest.TestCase):
    def setUp(self):
        self.text = XBOX.read_text(encoding="utf-8")

    def body(self, name):
        start = re.search(r"^\w[\w ]*\b" + name + r"\(\n", self.text, re.M)
        self.assertIsNotNone(start, name)
        end = self.text.index("\n}\n", start.end())
        return self.text[start.end():end]

    def test_saves_are_generations(self):
        for name in ("game_state_write_to_file", "game_state_write_to_persistent_storage"):
            self.assertIn("game_state_generations_save(", self.body(name), name)

    def test_loads_try_generations(self):
        for name in ("game_state_read_from_file", "game_state_read_from_persistent_storage"):
            self.assertIn("game_state_generations_take(", self.body(name), name)
        self.assertIn("game_state_generations_load(", self.body("game_state_read_header_from_persistent_storage"))

    def test_no_saved_game_is_deleted_on_failure(self):
        # (the symbol table at the top still names the upstream's _delete_persistent_storage)
        self.assertIsNone(re.search(r"\bdelete_persistent_storage\s*\(", self.text))
        for name in ("game_state_write_to_persistent_storage", "game_state_read_from_persistent_storage"):
            self.assertNotIn("DeleteFileA", self.body(name), name)

    def test_a_failure_while_taking_stops(self):
        body = self.body("game_state_generation_attempt_take")
        self.assertIn("_game_state_image_failed_while_taking", body)
        self.assertIn("_game_state_generation_failed", body)

    def test_core_export_is_not_generational(self):
        for name in ("game_state_write_core", "game_state_read_core"):
            self.assertNotIn("generations", self.body(name), name)


if __name__ == "__main__":
    unittest.main()
