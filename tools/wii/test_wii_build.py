"""Unit tests for the Wii build's identity (HWI-016G): source_dirty and the
line-ending-independent build ID. Temporary git repositories, no toolchain."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))

from tools import wii_build  # noqa: E402

INPUTS = [Path("configure.py"), Path("source/hs/hs.c"), Path("source/hs/hs.h"), Path("port/wii/probe/main.c"),
          Path("tools/wii/engine_semantics.py")]


def git(*args, cwd):
    subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "user.name=test", "-c", "user.email=test@example.invalid",
                    *args], cwd=cwd, check=True, capture_output=True)


class CheckoutTests(unittest.TestCase):
    """source_identity() over a temporary committed checkout"""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        git("init", "-q", cwd=self.root)
        git("config", "core.autocrlf", "false", cwd=self.root)
        for path in INPUTS:
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_bytes(f"/* {path.as_posix()} */\n".encode())
        (self.root / "docs").mkdir()
        (self.root / "docs/notes.md").write_bytes(b"notes\n")
        (self.root / ".gitignore").write_bytes(b"build/\n")
        git("add", "-A", cwd=self.root)
        git("commit", "-q", "-m", "fixture", cwd=self.root)
        self.previous = os.getcwd()
        os.chdir(self.root)

    def tearDown(self):
        os.chdir(self.previous)
        self.directory.cleanup()

    def dirty(self, directories=()):
        commit, dirty = wii_build.source_identity(INPUTS, directories)
        self.assertRegex(commit, r"^[0-9a-f]{40}$")
        return dirty

    def test_clean_checkout_is_clean(self):
        self.assertFalse(self.dirty())

    def test_modified_engine_source_is_dirty(self):
        (self.root / "source/hs/hs.c").write_bytes(b"/* changed */\n")
        self.assertTrue(self.dirty())

    def test_staged_change_is_dirty(self):
        (self.root / "source/hs/hs.h").write_bytes(b"/* changed */\n")
        git("add", "source/hs/hs.h", cwd=self.root)
        self.assertTrue(self.dirty())

    def test_deleted_file_in_a_consumed_directory_is_dirty(self):
        (self.root / "source/hs/hs.h").unlink()
        self.assertTrue(self.dirty())

    def test_untracked_file_in_a_consumed_directory_is_dirty(self):
        (self.root / "source/hs/hs_new.h").write_bytes(b"/* new */\n")
        self.assertTrue(self.dirty())

    def test_untracked_file_below_a_consumed_directory_is_dirty(self):
        (self.root / "port/wii/probe/sub").mkdir()
        (self.root / "port/wii/probe/sub/included.h").write_bytes(b"/* new */\n")
        self.assertTrue(self.dirty())

    def test_untracked_file_in_a_searched_directory_is_dirty(self):
        (self.root / "port/include/xdk").mkdir(parents=True)
        (self.root / "port/include/xdk/xtl.h").write_bytes(b"/* new */\n")
        self.assertFalse(self.dirty())
        self.assertTrue(self.dirty([Path("port/include/xdk")]))

    def test_untracked_input_is_dirty(self):
        (self.root / "port/wii/engine").mkdir(parents=True)
        (self.root / "port/wii/engine/unit.c").write_bytes(b"/* new */\n")
        commit, dirty = wii_build.source_identity([*INPUTS, Path("port/wii/engine/unit.c")], ())
        self.assertTrue(dirty)

    def test_ignored_input_is_dirty(self):
        (self.root / "build").mkdir()
        (self.root / "build/generated.h").write_bytes(b"/* generated */\n")
        self.assertFalse(self.dirty())
        commit, dirty = wii_build.source_identity([*INPUTS, Path("build/generated.h")], ())
        self.assertTrue(dirty)

    def test_changes_outside_the_consumed_paths_stay_clean(self):
        (self.root / "docs/notes.md").write_bytes(b"changed\n")
        (self.root / "docs/new.md").write_bytes(b"new\n")
        (self.root / "README.md").write_bytes(b"new\n")
        (self.root / "build").mkdir()
        (self.root / "build/probe.o").write_bytes(b"\x00object")
        self.assertFalse(self.dirty())

    def test_a_crlf_checkout_is_clean(self):
        # what git writes with core.autocrlf=true (Titan) is not a change
        git("config", "core.autocrlf", "true", cwd=self.root)
        (self.root / "source/hs/hs.c").unlink()
        subprocess.run(["git", "checkout", "--", "source/hs/hs.c"], cwd=self.root, check=True, capture_output=True)
        self.assertEqual((self.root / "source/hs/hs.c").read_bytes(), b"/* source/hs/hs.c */\r\n")
        self.assertFalse(self.dirty())


class ConsumedPathTests(unittest.TestCase):
    def test_outermost_directories_and_root_files(self):
        paths = wii_build.consumed_paths(
            [Path("configure.py"), Path("source/hs/hs.c"), Path("source/hs/hs.h"), Path("tools/wii_build.py"),
             Path("tools/wii/build.py"), Path("port/wii/probe/main.c")], [Path("source"), Path(".")])
        self.assertEqual(paths, ["configure.py", "port/wii/probe", "source", "tools"])

    def test_checkout_covers_every_engine_source_and_tool(self):
        previous = os.getcwd()
        os.chdir(ROOT)
        try:
            inputs = wii_build.build_inputs()
            paths = wii_build.consumed_paths(inputs, wii_build.searched_directories())
        finally:
            os.chdir(previous)
        for required in ("configure.py", "source", "tools", "port/wii/engine", "port/linux"):
            self.assertIn(required, paths)
        covered = lambda path: any(path == entry or path.startswith(entry + "/") for entry in paths)  # noqa: E731
        uncovered = [path.as_posix() for path in inputs if not covered(path.as_posix())]
        self.assertEqual(uncovered, [])
        self.assertTrue(any(path.as_posix().startswith("source/hs/") for path in inputs))


class InputHashTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def build_id(self, inputs):
        hashes = {path.name: wii_build.input_sha256(path) for path in inputs}
        return wii_build.build_id({"inputs_sha256": hashes, "inputs_sha256_rule": wii_build.INPUTS_SHA256_RULE})

    def test_crlf_and_lf_copies_hash_and_identify_the_same(self):
        lf = self.write("lf.c", b"int a;\n/* two */\nint b;\n")
        crlf = self.write("crlf.c", b"int a;\r\n/* two */\r\nint b;\r\n")
        self.assertEqual(wii_build.input_sha256(lf), wii_build.input_sha256(crlf))
        self.assertEqual(self.build_id([self.write("unit.c", lf.read_bytes())]),
                         self.build_id([self.write("unit.c", crlf.read_bytes())]))

    def test_lf_hash_is_the_plain_sha256(self):
        import hashlib
        data = b"int a;\n"
        self.assertEqual(wii_build.input_sha256(self.write("a.c", data)), hashlib.sha256(data).hexdigest())

    def test_a_content_change_changes_the_id(self):
        before = self.build_id([self.write("unit.c", b"int a;\r\n")])
        after = self.build_id([self.write("unit.c", b"int b;\r\n")])
        self.assertNotEqual(before, after)

    def test_a_lone_carriage_return_is_content(self):
        self.assertNotEqual(wii_build.input_sha256(self.write("a.c", b"a\rb\n")),
                            wii_build.input_sha256(self.write("b.c", b"a\nb\n")))

    def test_binary_inputs_are_hashed_as_they_are(self):
        self.assertNotEqual(wii_build.input_sha256(self.write("a.bin", b"\x00\r\n")),
                            wii_build.input_sha256(self.write("b.bin", b"\x00\n")))

    def test_build_id_is_sixteen_hex_digits(self):
        self.assertRegex(self.build_id([self.write("unit.c", b"int a;\n")]), r"^[0-9a-f]{16}$")


if __name__ == "__main__":
    unittest.main()
