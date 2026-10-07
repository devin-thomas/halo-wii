"""Synthetic preflight failures; these tests never establish a Wii compile pass."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from check_toolchain import inspect_toolchain


class ToolchainChecks(unittest.TestCase):
    def make_installation(self, root: Path) -> None:
        suffix = ".exe" if os.name == "nt" else ""
        for relative in (
            f"devkitPPC/bin/powerpc-eabi-gcc{suffix}",
            f"devkitPPC/bin/powerpc-eabi-ld{suffix}",
            f"tools/bin/elf2dol{suffix}",
            "devkitPPC/wii_rules", "libogc/include/gccore.h", "libogc/lib/wii/libogc.a",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

    def test_missing_root_does_not_use_unrelated_path_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "missing"
            with patch("check_toolchain.shutil.which", return_value=None):
                record = inspect_toolchain(root)
            self.assertFalse(record["ready_for_example_build"])
            self.assertNotIn("powerpc-eabi-gcc", record["tools"])
            self.assertTrue(any("directory does not exist" in item for item in record["errors"]))

    def test_wrong_architecture_is_rejected_even_with_complete_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "toolchain with spaces"
            self.make_installation(root)

            def output(command):
                if command[-1] == "-dumpmachine":
                    return "x86_64-w64-mingw32"
                return "synthetic version"

            with patch("check_toolchain.command_output", side_effect=output), patch(
                "check_toolchain.shutil.which", return_value="ninja"
            ):
                record = inspect_toolchain(root)
            self.assertFalse(record["ready_for_example_build"])
            self.assertFalse(record["compile_verified"])
            self.assertTrue(any("Wrong compiler target" in item for item in record["errors"]))

    def test_failing_version_command_is_not_a_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_installation(root)
            with patch(
                "check_toolchain.command_output",
                side_effect=subprocess.CalledProcessError(1, ["synthetic compiler"]),
            ), patch("check_toolchain.shutil.which", return_value="ninja"):
                record = inspect_toolchain(root)
            self.assertFalse(record["ready_for_example_build"])
            self.assertTrue(any("Cannot execute powerpc-eabi-gcc" in item for item in record["errors"]))


if __name__ == "__main__":
    unittest.main()
