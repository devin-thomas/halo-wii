"""Synthetic checks that the build-only artifact inspection discriminates."""

import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

import package_artifacts as package

COMMIT = "0123456789abcdef0123456789abcdef01234567"
MAP_TEXT = ("Archive member included to satisfy reference by file (symbol)\n\n"
            "Memory Configuration\n\nName Origin Length\n\n"
            "Linker script and memory map\n\n .text 0x80004000 0x100 build/wii/main.o\n")


def synthetic_elf(extra: bytes = b"", sections: dict | None = None, wide=False, order=">") -> bytes:
    """An executable ELF with a .text load segment, `extra` as .rodata, the
    given extra sections (name -> contents) and a section header table."""
    head = order + ("16sHHIQQQIHHHHHH" if wide else "16sHHIIIIIHHHHHH")
    entry_format = order + ("IIQQQQIIQQ" if wide else "10I")
    program = (struct.pack(order + "IIQQQQQQ", 1, 5, 0x100, 0x80004000, 0x80004000, 0x100, 0x100, 4) if wide
               else struct.pack(order + "8I", 1, 0x100, 0x80004000, 0x80004000, 0x100, 0x100, 5, 4))
    body = bytearray(0x200)  # headers, then .text at 0x100
    names = bytearray(b"\0")
    table = [(0, 0, 0, 0, 0, 0)]  # name, type, flags, address, offset, size

    def add(name, kind, flags, address, offset, size):
        table.append((len(names), kind, flags, address, offset, size))
        names.extend(name.encode() + b"\0")

    add(".text", 1, 0x6, 0x80004000, 0x100, 0x100)
    for name, content in {".rodata": extra, **(sections or {})}.items():
        flags = 0x2 if name == ".rodata" else 0x30 if name in (".comment", ".debug_str", ".debug_line_str") else 0
        add(name, 1, flags, 0, len(body), len(content))
        body += content
    add(".shstrtab", 3, 0, 0, len(body), 0)
    table[-1] = table[-1][:5] + (len(names),)
    body += names + bytes(-len(body + names) % 8)
    shoff = len(body)
    for name, kind, flags, address, offset, size in table:
        body += struct.pack(entry_format, name, kind, flags, address, offset, size, 0, 0, 1, 0)
    ident = b"\x7fELF" + bytes([2 if wide else 1, 2 if order == ">" else 1, 1]) + bytes(9)
    struct.pack_into(head, body, 0, ident, 2, 20, 1, 0x80004000, struct.calcsize(head), shoff, 0,
                     struct.calcsize(head), len(program), 1, struct.calcsize(entry_format), len(table), len(table) - 1)
    body[struct.calcsize(head):struct.calcsize(head) + len(program)] = program
    return bytes(body)


def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte, value = value & 0x7F, value >> 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


STANDARD_OPCODE_LENGTHS = bytes([0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1])


def line_unit_v3(directories, files, program=b"") -> bytes:
    """A DWARF 3 .debug_line unit with inline directory and file tables."""
    tables = (b"".join(name + b"\0" for name in directories) + b"\0"
              + b"".join(name + b"\0" + uleb(1) + uleb(0) + uleb(0) for name in files) + b"\0")
    header = bytes([4, 1, 0xFB, 14, 13]) + STANDARD_OPCODE_LENGTHS + tables
    unit = struct.pack(">HI", 3, len(header)) + header + program
    return struct.pack(">I", len(unit)) + unit


def line_unit_v5(directory, md5, program=b"") -> bytes:
    """A DWARF 5 .debug_line unit: an inline (DW_FORM_string) directory, and a
    file named through .debug_line_str with a directory index and an MD5."""
    directories = bytes([1]) + uleb(1) + uleb(0x08) + uleb(1) + directory + b"\0"
    files = (bytes([3]) + uleb(1) + uleb(0x1F) + uleb(2) + uleb(0x0F) + uleb(5) + uleb(0x1E)
             + uleb(1) + struct.pack(">I", 0) + uleb(0) + md5)
    header = bytes([4, 1, 1, 0xFB, 14, 13]) + STANDARD_OPCODE_LENGTHS + directories + files
    unit = struct.pack(">HBBI", 5, 4, 0, len(header)) + header + program
    return struct.pack(">I", len(unit)) + unit


def synthetic_dol(extra: bytes = b"") -> bytes:
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0, 0x100)
    struct.pack_into(">I", header, 0x48, 0x80004000)
    struct.pack_into(">I", header, 0x90, 0x100)
    struct.pack_into(">I", header, 0xE0, 0x80004000)
    return bytes(header) + bytes(0x100) + extra


def make_build(root: Path, elf_extra=b"", dol_extra=b"", map_extra="", elf_sections=None, probe_elf=None,
               **info_overrides) -> Path:
    build = root / "build"
    build.mkdir()
    for stem, (info, scope) in package.TARGETS.items():
        elf = synthetic_elf(elf_extra, elf_sections) if stem == "probe" else synthetic_elf()
        (build / f"{stem}.elf").write_bytes(elf if probe_elf is None or stem != "probe" else probe_elf)
        (build / f"{stem}.dol").write_bytes(synthetic_dol(dol_extra if stem == "probe" else b""))
        (build / f"{stem}.map").write_text(MAP_TEXT + (map_extra if stem == "probe" else ""), encoding="utf-8")
        record = {
            "schema_version": 1, "scope": scope, "source_commit": COMMIT, "source_dirty": False,
            "build_id": "0123456789abcdef", "compiler": "powerpc-eabi-gcc (devkitPPC) 0.0",
            "binutils": "GNU ld 0.0", "runtime_verified": False,
            "path_prefix_map": {"checkout": ".", "devkitpro": "/opt/devkitpro"},
            "artifacts": {name: {"sha256": hashlib.sha256((build / name).read_bytes()).hexdigest(),
                                 "bytes": (build / name).stat().st_size}
                          for name in (f"{stem}.elf", f"{stem}.dol", f"{stem}.map")},
        }
        record.update(info_overrides)
        (build / info).write_text(json.dumps(record), encoding="utf-8")
    return build


class PackageArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def staged(self, **kwargs) -> Path:
        output = self.root / "dist"
        package.stage(make_build(self.root, **kwargs), output, {"devkitPPC": "r0-1"})
        return output

    def inspect(self, directory, **kwargs):
        return package.inspect(directory, **kwargs)

    def categories(self, result):
        return {finding["category"] for finding in result["findings"]}

    def test_clean_synthetic_set_passes_and_is_labeled_build_only(self):
        directory = self.staged()
        result = self.inspect(directory, require_clean=True)
        self.assertTrue(result["pass"], result)
        manifest = json.loads((directory / package.MANIFEST).read_text(encoding="utf-8"))
        self.assertEqual(manifest["label"], "build-only")
        self.assertIn("physical Wii hardware", manifest["not_verified"])
        self.assertIn("emulator execution", manifest["not_verified"])
        self.assertEqual(sorted(p.name for p in directory.iterdir()), package.allowlist())

    def test_stage_refuses_a_non_empty_output(self):
        build = make_build(self.root)
        output = self.root / "dist"
        output.mkdir()
        (output / "stale.bin").write_bytes(b"x")
        with self.assertRaises(ValueError):
            package.stage(build, output, {})

    def test_unlisted_file_subdirectory_and_missing_file_fail(self):
        directory = self.staged()
        (directory / "local-config.json").write_text("{}", encoding="utf-8")
        (directory / "extra").mkdir()
        (directory / "probe.dol").unlink()
        problems = "\n".join(self.inspect(directory)["problems"])
        self.assertIn("local-config.json: not on the artifact allowlist", problems)
        self.assertIn("extra: only regular files", problems)
        self.assertIn("probe.dol: required allowlisted file is missing", problems)

    def test_allowlist_covers_every_wii_target(self):
        names = package.allowlist()
        for stem in ("probe", "gx_scene", "gx_materials", "geometry_view", "memory_strategy", "engine"):
            for suffix in (".elf", ".dol", ".map"):
                self.assertIn(stem + suffix, names)
        self.assertIn("gx_materials-build-info.json", names)
        self.assertIn("memory_strategy-build-info.json", names)
        self.assertIn("engine-build-info.json", names)
        self.assertEqual(len(names), 6 * 4 + 1)

    def test_materials_scope_and_missing_artifact_are_checked(self):
        directory = self.staged()
        info = directory / "gx_materials-build-info.json"
        record = json.loads(info.read_text(encoding="utf-8"))
        record["scope"] = "asset_free_gx_scene"
        info.write_text(json.dumps(record), encoding="utf-8")
        (directory / "gx_materials.map").unlink()
        problems = "\n".join(self.inspect(directory)["problems"])
        self.assertIn("gx_materials-build-info.json: scope is not asset_free_gx_materials", problems)
        self.assertIn("gx_materials.map: required allowlisted file is missing", problems)

    def test_tampered_artifact_fails_hash_checks(self):
        directory = self.staged()
        (directory / "probe.dol").write_bytes(synthetic_dol(b"tampered"))
        problems = "\n".join(self.inspect(directory)["problems"])
        self.assertIn("build-info.json: recorded hash/size of probe.dol", problems)
        self.assertIn("MANIFEST.json: recorded kind/hash/size of probe.dol", problems)

    def test_windows_and_posix_drive_paths_fail(self):
        result = self.inspect(self.staged(elf_extra=b"C:\\Users\\someone\\halo\\main.c\0",
                                          map_extra="/d/work/checkout/build/wii/main.o\n"))
        self.assertFalse(result["pass"])
        self.assertTrue({"windows-drive-path", "posix-drive-path"} <= self.categories(result))

    def test_debug_line_noise_is_not_a_path(self):
        # Printable bytes from a line-number program, not NUL-terminated.
        self.assertTrue(self.inspect(self.staged(elf_extra=b"\x05/K/0gg\x05\x06x:/ab\x01"))["pass"])

    def test_drive_path_shape_in_encoded_debug_info_is_noise(self):
        # A NUL-terminated printable run inside a DIE tree, as in CI run 38071749042.
        result = self.inspect(self.staged(elf_sections={".debug_info": b"\x01\x07q:/)\0A:/K\0\x08"}))
        self.assertTrue(result["pass"], result)

    def test_drive_path_shape_in_rodata_fails(self):
        result = self.inspect(self.staged(elf_extra=b"\x01q:/)\0",
                                          elf_sections={".debug_info": b"\x01\x07q:/)\0"}))
        self.assertFalse(result["pass"])
        self.assertEqual([(f["category"]) for f in result["findings"]], ["windows-drive-path"])

    def test_debug_string_sections_and_comment_are_scanned(self):
        for section in (".debug_str", ".debug_line_str", ".comment", ".strtab"):
            with self.subTest(section=section):
                self.temporary.cleanup()
                self.temporary = tempfile.TemporaryDirectory()
                self.root = Path(self.temporary.name)
                result = self.inspect(self.staged(elf_sections={section: b"\0C:\\work\\halo\\main.c\0"}))
                self.assertEqual(self.categories(result), {"windows-drive-path"})

    def test_build_host_path_secret_and_owner_in_debug_info_still_fail(self):
        token = "ghp_" + "A1b2C3d4E5" * 4
        noise = b"\x01\x07/srv/build/checkout/src/main.c\0\x02 by alice \x03" + token.encode() + b"\x04"
        directory = self.staged(elf_sections={".debug_info": noise, ".debug_loclists": noise})
        result = self.inspect(directory, forbid_paths=["/srv/build/checkout"], forbid_texts=["alice"])
        self.assertEqual(self.categories(result), {"build-host-path", "forbidden-text", "github-token"})
        self.assertEqual(len(result["findings"]), 6)
        self.assertNotIn(token, json.dumps(result))

    def test_debug_line_directory_and_file_tables_are_scanned(self):
        program = b"\x00\x05\x02q:/)\0\x01"  # line-number program bytes: noise
        line = (line_unit_v3([b"src", b"C:\\Users\\bob\\halo"], [b"main.c", b"q:/x.c"], program)
                + line_unit_v5(b"D:\\build\\wii", b"q:/)\0" + bytes(11), program))
        result = self.inspect(self.staged(elf_sections={".debug_line": line}))
        self.assertEqual(self.categories(result), {"windows-drive-path"})
        self.assertEqual(len(result["findings"]), 3, result)  # C:\Users..., q:/x.c, D:\build...
        clean = line_unit_v3([b"src"], [b"main.c"], program) + line_unit_v5(b"src", b"q:/)\0" + bytes(11), program)
        self.temporary.cleanup()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.assertTrue(self.inspect(self.staged(elf_sections={".debug_line": clean}))["pass"])

    def test_malformed_debug_line_is_a_problem_and_scanned_whole(self):
        result = self.inspect(self.staged(elf_sections={".debug_line": b"\0\0\0\x08\0\x09\0\0\0\0\0\0",
                                                        ".debug_info": b"\x01q:/)\0"}))
        self.assertFalse(result["pass"])
        self.assertIn("probe.elf: unreadable ELF sections", "\n".join(result["problems"]))
        self.assertIn("windows-drive-path", self.categories(result))

    def test_truncated_or_malformed_elf_is_a_problem(self):
        good = synthetic_elf(sections={".debug_info": b"\x01q:/)\0"})
        past_end = bytearray(good)
        struct.pack_into(">I", past_end, 32, len(good) + 4096)  # e_shoff beyond the file
        bad_size = bytearray(good)
        struct.pack_into(">H", bad_size, 46, 64)  # ELF32 with 64-byte section entries
        for label, data in (("truncated", good[:40]), ("table past end", bytes(past_end)),
                            ("entry size", bytes(bad_size)), ("not elf", b"\0" * 600)):
            with self.subTest(label):
                self.temporary.cleanup()
                self.temporary = tempfile.TemporaryDirectory()
                self.root = Path(self.temporary.name)
                result = self.inspect(self.staged(probe_elf=data))
                self.assertFalse(result["pass"])
                self.assertIn("probe.elf: unreadable ELF sections", "\n".join(result["problems"]))

    def test_section_parser_is_generic_over_class_and_byte_order(self):
        for wide in (False, True):
            for order in ("<", ">"):
                with self.subTest(wide=wide, order=order):
                    data = synthetic_elf(b"\0q:/)\0", {".debug_info": b"\x01q:/)\0"}, wide, order)
                    sections, _ = package.elf_sections(data)
                    self.assertEqual([s["name"] for s in sections],
                                     ["", ".text", ".rodata", ".debug_info", ".shstrtab"])
                    rodata, debug = sections[2], sections[3]
                    regions = package.elf_path_regions(data)
                    covered = lambda offset: any(a <= offset < b for a, b in regions)  # noqa: E731
                    self.assertTrue(covered(rodata["offset"] + 1))
                    self.assertFalse(covered(debug["offset"] + 1))
                    self.assertFalse(covered(0))  # the ELF header itself

    def test_home_paths_fail_except_reviewed_upstream_prefixes(self):
        reviewed = self.inspect(self.staged(
            dol_extra=b"/home/davem/projects/devkitpro/pacman-packages/newlib/dtoa.c\0"))
        self.assertTrue(reviewed["pass"], reviewed)
        self.assertEqual(reviewed["reviewed"].get("probe.dol"), 1)
        self.temporary.cleanup()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        result = self.inspect(self.staged(dol_extra=b"/home/alice/src/main.c\0/__w/repo/repo/x.c\0"))
        self.assertEqual(self.categories(result), {"home-or-workspace-path"})
        self.assertEqual(len(result["findings"]), 2)

    def test_named_host_paths_and_user_names_fail(self):
        directory = self.staged(map_extra="/srv/build/checkout/build/wii/main.o by alice\n")
        result = self.inspect(directory, forbid_paths=["/srv/build/checkout"], forbid_texts=["alice"])
        self.assertEqual(self.categories(result), {"build-host-path", "forbidden-text"})
        self.assertTrue(self.inspect(directory)["pass"])

    def test_host_identity_skips_generic_names(self):
        paths, names = package.host_identity({"HOME": "/github/home", "USER": "root",
                                              "USERPROFILE": "C:\\Users\\alice"})
        self.assertIn("/github/home", paths)
        self.assertEqual(names, ["alice"])

    def test_secret_like_content_fails_without_echoing_it(self):
        token = "ghp_" + "A1b2C3d4E5" * 4
        # Assembled at run time so the source holds no literal secret shape.
        secrets = (token + "\n-----BEGIN OPENSSH " + "PRIVATE KEY-----\n" + "AKIA" + "ABCDEFGHIJKLMNOP\n"
                   "password = hunter2hunter2\nwii 192.168.1.50\n")
        result = self.inspect(self.staged(map_extra=secrets))
        self.assertTrue({"github-token", "private-key", "aws-access-key", "credential-assignment",
                         "private-network-address"} <= self.categories(result))
        self.assertNotIn(token, json.dumps(result))
        self.assertNotIn("hunter2", json.dumps(result))

    def test_xbox_game_data_signatures_fail(self):
        cache = b"daeh" + bytes(package.CACHE_FOOT_OFFSET - 4) + b"toof"
        # In encoded debug data: the signatures are checked on every byte.
        result = self.inspect(self.staged(elf_sections={".debug_info": cache + package.XISO_MAGIC + b"XBEH"}))
        self.assertTrue({"halo-cache-header-signature", "xbox-image-signature",
                         "xbox-executable-signature"} <= self.categories(result))

    def test_binary_named_as_linker_map_fails(self):
        directory = self.staged()
        data = b"daeh" + bytes(16)
        (directory / "gx_scene.map").write_bytes(data)
        problems = "\n".join(self.inspect(directory)["problems"])
        self.assertIn("gx_scene.map: invalid linker-map", problems)

    def test_runtime_or_hardware_claims_fail(self):
        directory = self.staged()
        manifest = json.loads((directory / package.MANIFEST).read_text(encoding="utf-8"))
        manifest["label"] = "pass"
        manifest["not_verified"] = [item for item in manifest["not_verified"] if item != "physical Wii hardware"]
        (directory / package.MANIFEST).write_text(json.dumps(manifest), encoding="utf-8")
        problems = "\n".join(self.inspect(directory)["problems"])
        self.assertIn("label must be exactly 'build-only'", problems)
        self.assertIn("verified/not_verified claims differ", problems)

    def test_runtime_verified_build_info_fails(self):
        problems = "\n".join(self.inspect(self.staged(runtime_verified=True))["problems"])
        self.assertIn("runtime_verified must be false", problems)

    def test_dirty_source_fails_only_when_clean_is_required(self):
        directory = self.staged(source_dirty=True)
        self.assertTrue(self.inspect(directory)["pass"])
        problems = "\n".join(self.inspect(directory, require_clean=True)["problems"])
        self.assertIn("source tree was not clean", problems)

    def test_oversized_file_fails(self):
        directory = self.staged()
        limit = package.MAX_FILE_BYTES
        package.MAX_FILE_BYTES = 300
        try:
            problems = "\n".join(self.inspect(directory)["problems"])
        finally:
            package.MAX_FILE_BYTES = limit
        self.assertIn("exceeds the 300-byte cap", problems)

    def test_package_list_parser_ignores_other_lines(self):
        listing = self.root / "packages.txt"
        listing.write_text("devkitPPC r50-1\nlibogc 3.1.0-1\nerror: something odd\n", encoding="utf-8")
        self.assertEqual(package.read_packages(listing), {"devkitPPC": "r50-1", "libogc": "3.1.0-1"})


class NormalizeLinkMapTest(unittest.TestCase):
    def test_build_machine_roots_become_public_prefixes(self):
        import os
        from build import normalize_link_map
        if os.name == "nt":
            root, checkout = Path("C:/kits/devkitpro"), Path("C:/work/halo-wii")
            text = ("C:/kits/devkitpro/libogc/lib/wii\\libogc.a(gx.o)\n"
                    "c:\\kits\\devkitpro/devkitPPC/bin/../lib/crtbegin.o\n"
                    "C:\\work\\halo-wii\\build/wii/main.o\nC:/kits/devkitpro-old/x.o\n")
        else:
            root, checkout = Path("/srv/kits/devkitpro"), Path("/srv/work/halo-wii")
            text = ("/srv/kits/devkitpro/libogc/lib/wii/libogc.a(gx.o)\n"
                    "/srv/kits/devkitpro/devkitPPC/bin/../lib/crtbegin.o\n"
                    "/srv/work/halo-wii/build/wii/main.o\n/srv/kits/devkitpro-old/x.o\n")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "probe.map"
            path.write_text(text, encoding="utf-8")
            normalize_link_map(path, checkout, root)
            lines = path.read_text(encoding="utf-8").splitlines()
        self.assertTrue(lines[0].startswith("/opt/devkitpro/libogc/lib/wii"))
        self.assertTrue(lines[1].startswith("/opt/devkitpro/devkitPPC/bin/../lib/crtbegin.o"))
        self.assertTrue(lines[2].startswith("./build/wii/main.o") or lines[2].startswith(".\\build"))
        self.assertIn("devkitpro-old", lines[3])  # a longer sibling name is not a prefix match
        self.assertFalse(lines[3].startswith("/opt/devkitpro"))


if __name__ == "__main__":
    unittest.main()
