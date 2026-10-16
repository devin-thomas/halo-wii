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


@unittest.skipUnless(compiler(), "no host C compiler")
class SaveFailureBehaviorTests(unittest.TestCase):
    def check_function(self, path, name, declarations, scenario):
        text = path.read_text(encoding="utf-8")
        function = re.search(r"^(?:static\s+)?(?:\w+\s+)+" + name + r"\([^)]*\)\n\{.*?^\}",
                             text, re.M | re.S)
        self.assertIsNotNone(function, name)
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "check.c"
            exe = Path(temp) / ("check.exe" if sys.platform == "win32" else "check")
            source.write_text(declarations + "\n" + function.group(0) + "\n" + scenario, encoding="utf-8")
            build = subprocess.run([compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                                    "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_failed_write_retains_checkpoint_availability(self):
        self.check_function(ROOT / "source/saved games/game_state.c", "game_state_save", r'''
#include <assert.h>
typedef int boolean;
#define TRUE 1
static struct { boolean saved_game_valid; } game_state_globals;
static int succeeds, stopped, started, before;
static void game_state_call_before_save_procs(void) { before++; }
static void main_stop_time(void) { stopped++; }
static void main_start_time(void) { started++; }
static boolean game_state_write_to_file(void) { return succeeds; }
static void game_state_note_event(char const *message) { (void)message; }
''', r'''
int main(void) {
    game_state_save();
    assert(!game_state_globals.saved_game_valid);
    succeeds = 1;
    game_state_save();
    assert(game_state_globals.saved_game_valid);
    succeeds = 0;
    game_state_save();
    assert(game_state_globals.saved_game_valid);
    assert(before == 3 && stopped == 3 && started == 3);
    return 0;
}
''')

    def test_partial_load_halts_even_when_assertions_only_log(self):
        self.check_function(XBOX, "game_state_image_file_take_result", r'''
#include <assert.h>
#include <setjmp.h>
typedef int boolean;
typedef int HANDLE;
enum { _game_state_image_taken, _game_state_image_refused, _game_state_image_failed_while_taking };
enum { _error_silent };
struct game_state_image_file { int unused; };
struct report { int refusal; };
static jmp_buf halted;
static int load_result;
static void game_state_image_file_new(struct game_state_image_file *file, HANDLE *handle)
{ (void)file; (void)handle; }
static short game_state_port_image_load(struct game_state_image_file *file, boolean check)
{ (void)file; (void)check; return load_result; }
static struct report *game_state_image_last_report(void) { static struct report report; return &report; }
static char const *game_state_image_refusal_name(int refusal) { (void)refusal; return "refused"; }
static void error(int level, char const *format, ...) { (void)level; (void)format; }
/* Release assertions return; the fatal path must be explicit. */
#define match_vassert(...) ((void)0)
#define vhalt(message) do { (void)(message); longjmp(halted, 1); } while (0)
''', r'''
int main(void) {
    load_result = _game_state_image_taken;
    assert(game_state_image_file_take_result(1, 1, "test") == _game_state_image_taken);
    load_result = _game_state_image_refused;
    assert(game_state_image_file_take_result(1, 1, "test") == _game_state_image_refused);
    load_result = _game_state_image_failed_while_taking;
    if (!setjmp(halted)) { game_state_image_file_take_result(1, 1, "test"); return 1; }
    return 0;
}
''')

    def test_fallback_difficulty_is_checked_before_loading(self):
        self.check_function(XBOX, "game_state_generation_attempt_take", r'''
#include <assert.h>
typedef int boolean;
typedef int HANDLE;
#define NULL ((void *)0)
#define TRUE 1
#define GENERIC_READ 1
#define OPEN_EXISTING 1
#define INVALID_HANDLE_VALUE (-1)
enum { _game_state_generation_taken, _game_state_generation_refused, _game_state_generation_failed };
enum { _game_state_image_taken, _game_state_image_refused, _game_state_image_failed_while_taking };
struct game_state_image_file { int unused; };
struct game_state_header { short difficulty; };
struct game_state_generation_io { int flags; char const *what; boolean check_difficulty; };
static int difficulty = 2, header_ok = 1, takes, closes;
static HANDLE CreateFileA(char const *p, int a, int b, void *c, int d, int e, void *f)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 1; }
static void CloseHandle(HANDLE file) { (void)file; closes++; }
static void game_state_image_file_new(struct game_state_image_file *file, HANDLE *handle)
{ (void)file; (void)handle; }
static boolean game_state_image_read_header(struct game_state_image_file *file, struct game_state_header *header)
{ (void)file; header->difficulty = difficulty; return header_ok; }
static short main_get_difficulty(void) { return 1; }
static short game_state_image_file_take_result(HANDLE file, boolean check, char const *what)
{ (void)file; (void)check; (void)what; takes++; return _game_state_image_taken; }
''', r'''
int main(void) {
    struct game_state_generation_io io = {0, "test", 1};
    assert(game_state_generation_attempt_take(&io, "test") == _game_state_generation_refused);
    assert(takes == 0 && closes == 1);
    difficulty = 1;
    assert(game_state_generation_attempt_take(&io, "test") == _game_state_generation_taken);
    assert(takes == 1 && closes == 2);
    header_ok = 0;
    assert(game_state_generation_attempt_take(&io, "test") == _game_state_generation_refused);
    assert(takes == 1 && closes == 3);
    return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
