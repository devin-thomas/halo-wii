"""HWI-046: a profile's and a gametype's signed block kept in generations (game_state_generation_block_save/load,
source/saved games/game_state_generations.c), fault-injected on the host (profile_generations_check.c), and the
profile, gametype and enumeration entrypoints that use them (player_profile.c, playlist_profile.c,
saved_game_files.c)."""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAVED_GAMES = ROOT / "source" / "saved games"


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


@unittest.skipUnless(compiler(), "no host C compiler")
class BlockGenerationFaultTests(unittest.TestCase):
    def test_every_interruption_and_bad_write_leaves_a_signed_block(self):
        with tempfile.TemporaryDirectory() as temp:
            name = "profile_generations_check.exe" if sys.platform == "win32" else "profile_generations_check"
            exe = Path(temp) / name
            build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(ROOT / "source"), "-o", str(exe),
                                    str(ROOT / "tools" / "wii" / "profile_generations_check.c")],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-4000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            match = re.fullmatch(r"ok (\d+) cases\s*", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            self.assertGreater(int(match.group(1)), 50)


class EntrypointTests(unittest.TestCase):
    def body(self, file, name):
        text = (SAVED_GAMES / file).read_text(encoding="utf-8")
        # the definition (its parameters then its body), not the prototype
        start = re.search(r"^\w[\w ]*\b" + name + r"\(\n[^;{]*?\)\n\{", text, re.M)
        self.assertIsNotNone(start, name)
        end = text.index("\n}\n", start.end())
        return text[start.end():end]

    def test_writes_are_generations_and_delete_nothing(self):
        for file, name in (("player_profile.c", "player_profile_write_thread_proc"),
                           ("playlist_profile.c", "playlist_profile_write_thread_proc")):
            body = self.body(file, name)
            self.assertIn("saved_game_file_write_block(", body, name)
            self.assertNotIn("delete_enumerated_saved_game_file", body, name)
            self.assertNotIn("file_write(", body, name)
            self.assertNotIn("saved_game_file_open(", body, name)

    def test_reads_try_generations(self):
        for file, name in (("player_profile.c", "player_profile_read"),
                           ("playlist_profile.c", "playlist_profile_read"),
                           ("playlist_profile.c", "playlist_profile_read_block")):
            body = self.body(file, name)
            self.assertIn("saved_game_file_read_block(", body, name)
            self.assertNotIn("saved_game_file_open(", body, name)

    def test_signed_sizes_match_enumeration(self):
        files = (SAVED_GAMES / "saved_game_files.c").read_text(encoding="utf-8")
        self.assertIn("PLAYER_PROFILE_CHECKSUM_DATA_SIZE = 48,", files)
        self.assertIn("PLAYLIST_PROFILE_CHECKSUM_DATA_SIZE = 104", files)
        self.assertIn("sizeof(struct player_profile) == 0x30", (SAVED_GAMES / "player_profile.c").read_text(
            encoding="utf-8"))
        self.assertIn("sizeof(struct playlist_profile_write_request) == 0x6C",
                      (SAVED_GAMES / "playlist_profile.c").read_text(encoding="utf-8"))

    def test_enumeration_migrates_without_writing_in_place(self):
        body = self.body("saved_game_files.c", "enumerate_memory_units")
        self.assertIn("game_state_generation_block_load(", body)
        self.assertIn("saved_game_file_type_from_generations(", body)
        # the old-style CRC is signed again as a new generation, never written over the file
        self.assertIn("game_state_generation_block_save(", body)
        self.assertNotIn("file_write(", body)
        self.assertNotIn("file_set_position(", body)


if __name__ == "__main__":
    unittest.main()
