"""Wii routing and executable validation, without a toolchain or game assets."""

from pathlib import Path
import runpy
import struct
import sys
from unittest.mock import patch

import pytest

from tools.wii.build import verify_dol, verify_elf
from tools.wii_build import toolchain_inputs, wii_configure_inputs

CONFIGURE = Path(__file__).resolve().parent.parent / "configure.py"


def test_wii_configuration_never_calls_desktop_generators(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(sys, "argv", [str(CONFIGURE), "--wii"])
    with patch("tools.wii_build.generate_wii_build") as wii, patch(
        "tools.linux_build.generate_linux_build", side_effect=AssertionError("desktop dependency")
    ), patch("tools.android_build.generate_android_build", side_effect=AssertionError("Android dependency")), patch(
        "tools.windows_build.generate_windows_build", side_effect=AssertionError("SDL download")
    ):
        runpy.run_path(str(CONFIGURE), run_name="__main__")
    wii.assert_called_once()


def test_native_configuration_still_calls_original_generators(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(sys, "argv", [str(CONFIGURE)])
    with patch("tools.linux_build.generate_linux_build") as linux, patch(
        "tools.android_build.generate_android_build"
    ) as android, patch("tools.windows_build.generate_windows_build") as windows, patch(
        "tools.wii_build.generate_wii_build", side_effect=AssertionError("unexpected Wii")
    ):
        runpy.run_path(str(CONFIGURE), run_name="__main__")
    for generator in (linux, android, windows):
        generator.assert_called_once()


@pytest.mark.parametrize("machine", [3, 62, 183])
def test_non_ppc_executables_are_rejected(tmp_path, machine):
    data = bytearray(52)
    data[:7] = b"\x7fELF\x01\x02\x01"
    struct.pack_into(">HH", data, 16, 2, machine)
    path = tmp_path / "wrong.elf"
    path.write_bytes(data)
    with pytest.raises(ValueError, match="PowerPC"):
        verify_elf(path)


def test_dol_sections_cannot_escape_file_or_memory(tmp_path):
    data = bytearray(0x104)
    struct.pack_into(">I", data, 0, 0x100)
    struct.pack_into(">I", data, 0x48, 0x80004000)
    struct.pack_into(">I", data, 0x90, 5)
    struct.pack_into(">I", data, 0xE0, 0x80004000)
    path = tmp_path / "bad.dol"
    path.write_bytes(data)
    with pytest.raises(ValueError, match="outside the file"):
        verify_dol(path)
    struct.pack_into(">I", data, 0x90, 4)
    struct.pack_into(">I", data, 0x48, 0x81FFFFFC)
    path.write_bytes(data)
    with pytest.raises(ValueError, match="outside MEM1"):
        verify_dol(path)


def test_header_only_elf_is_rejected(tmp_path):
    data = bytearray(52)
    data[:7] = b"\x7fELF\x01\x02\x01"
    struct.pack_into(">HH", data, 16, 2, 20)
    path = tmp_path / "empty.elf"
    path.write_bytes(data)
    with pytest.raises(ValueError, match="program header"):
        verify_elf(path)


def test_sdk_and_git_identity_are_reconfigure_dependencies(tmp_path, monkeypatch):
    root = tmp_path / "sdk with spaces"
    archive = root / "libogc/lib/wii/libwiiuse.a"
    archive.parent.mkdir(parents=True)
    archive.write_bytes(b"archive")
    assert archive in toolchain_inputs(root)
    assert archive.parent in toolchain_inputs(root)
    monkeypatch.chdir(CONFIGURE.parent)
    dependencies = wii_configure_inputs(root)
    assert Path(".git/HEAD") in dependencies
    assert any(path.as_posix().endswith("refs/heads/wii") for path in dependencies)
