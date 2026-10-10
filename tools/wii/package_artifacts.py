#!/usr/bin/env python3
"""Stage and inspect the reviewed build-only Wii artifact set.

stage   copies the allowlisted ELF/DOL/linker-map/build-info files from a
        build/wii directory into a new directory and writes MANIFEST.json,
        labeled build-only.
inspect fails unless a directory holds exactly that allowlist, every file has
        its expected structure, every hash agrees, and no file contains a
        build-machine path, user name, private network address, secret-like
        text or a recognised Xbox game-data signature.

A passing inspection says the artifacts compiled, linked, converted and are
publishable under this policy. It is not an emulator, gameplay, engine ABI,
host-lifecycle, persistence or hardware result. Findings name the file,
category and byte offset only, never the matched text, so a leaked secret is
not repeated into a public CI log.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import verify_dol, verify_elf  # noqa: E402

LABEL = "build-only"
MANIFEST = "MANIFEST.json"
# stem -> (build-info file name, expected build-info scope)
TARGETS = {
    "probe": ("build-info.json", "asset_free_probe"),
    "gx_scene": ("gx_scene-build-info.json", "asset_free_gx_scene"),
    "gx_materials": ("gx_materials-build-info.json", "asset_free_gx_materials"),
    "geometry_view": ("geometry_view-build-info.json", "owned_geometry_diagnostic_no_embedded_assets"),
    "memory_strategy": ("memory_strategy-build-info.json", "memory_strategy_diagnostic_no_embedded_assets"),
    "engine": ("engine-build-info.json", "engine_platform_runtime_no_embedded_assets"),
}
VERIFIED = ["compile", "link", "elf2dol conversion", "static ELF/DOL structure checks",
            "artifact allowlist and content inspection"]
NOT_VERIFIED = ["emulator execution", "gameplay", "full engine ABI", "host process lifecycle",
                "persistence", "physical Wii hardware"]
# The whole-engine ELF (HWI-015) is about 37 MB, most of it debug information,
# which the published artifact keeps so its addresses and layouts can be read.
MAX_FILE_BYTES = 48 * 1024 * 1024
MAX_TOTAL_BYTES = 128 * 1024 * 1024

# Absolute paths recorded by the official prebuilt devkitPro libraries (libogc,
# libfat, newlib) in their own debug info and assertion strings. They are the
# same public strings for every user of those packages, not this build host.
REVIEWED_PATH_PREFIXES = ("/opt/devkitpro/", "/home/davem/projects/devkitpro/pacman-packages/",
                          "/home/davem/projects/devkitpro/tool-packages/")
# Drive paths the engine's own public source spells (OpenCE and the port's
# files): the original build tree in its assertion messages
# (c:\halo\SOURCE\...), the Xbox's drives (d:\ the DVD, t:\ u:\ z:\ the hard
# disk, h:\ a Custom Edition install), the settings file's example
# (port/linux/src/port_config.c) and the Wii driver's own check file. They are
# reviewed in the engine's artifacts only (HWI-015); a build machine's own
# paths are still caught by the host-path checks.
REVIEWED_ENGINE_PATH_PREFIXES = ("c:\\halo\\SOURCE\\", "c:\\halo\\source\\", "d:\\", "t:\\", "u:\\", "z:\\", "h:\\",
                                 "C:\\Games\\Halo'", "Z:\\HWI015\\")
REVIEWED_ENGINE_STEMS = ("engine",)
GENERIC_NAMES = {"root", "runner", "home", "user", "users", "admin", "administrator",
                 "github", "build", "builder", "default", "public", "guest", "docker"}

_P = r"(?<![\w.~-])"
PATH_PATTERNS = (
    ("windows-drive-path", re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/]")),
    ("unc-path", re.compile(r"(?<![\w\\])\\\\[\w.$-]+\\[\w.$-]+")),
    ("posix-drive-path", re.compile(_P + r"/(?:cygdrive/[A-Za-z]|mnt/[A-Za-z]|[A-Za-z])/(?=[\w.~-])")),
    ("home-or-workspace-path", re.compile(
        _P + r"/(?:home|Users|root|github/home|__w|Volumes|var/folders|private/var)(?:/|(?![\w.~-]))")),
)
SECRET_PATTERNS = (
    ("private-key", re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")),
    ("github-token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{20,})")),
    ("aws-access-key", re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("slack-token", re.compile(r"\bxox[abprs]-[A-Za-z0-9-]{10,}")),
    ("google-api-key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}")),
    ("api-key", re.compile(r"\b(?:sk-[A-Za-z0-9_-]{20,}|glpat-[A-Za-z0-9_-]{20,}|npm_[A-Za-z0-9]{36})")),
    ("jwt", re.compile(r"\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.")),
    ("webhook-url", re.compile(r"(?i)discord(?:app)?\.com/api/webhooks/|hooks\.slack\.com/services/")),
    ("credential-assignment", re.compile(
        r"(?i)\b(?:password|passwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token)\b"
        r"\s*[:=]\s*['\"]?[^\s'\"%]{8,}")),
    ("private-network-address", re.compile(
        r"(?<![\d.])(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}(?![\d.])")),
)
# Byte signatures of Xbox game data that must never appear in a public artifact.
XISO_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XBE_MAGIC = b"XBEH"
CACHE_HEAD, CACHE_FOOT, CACHE_FOOT_OFFSET = b"daeh", b"toof", 0x7FC

ASCII_RUN = re.compile(rb"[\x20-\x7e\t\r\n]{4,}")
UTF16_RUN = re.compile(rb"(?:[\x20-\x7e]\x00){4,}")

# ELF section handling for the generic path shapes. Encoded debug information
# (DIE trees, line-number programs, location lists, ...) is binary data whose
# printable runs are encoding noise, and its exact bytes move whenever the code
# does, so the generic shapes skip it. The strings compilers and linkers record
# are still scanned: allocated sections, string tables, .comment, .debug_str,
# .debug_line_str and the directory/file tables of every .debug_line unit.
SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL, SHT_DYNSYM = 2, 4, 8, 9, 11
SHF_ALLOC, SHF_COMPRESSED = 0x2, 0x800
DEBUG_STRING_SECTIONS = (".debug_str", ".debug_line_str")
# DWARF forms that can appear in DWARF 5 line-table entry formats: form ->
# fixed size in bytes, or "uleb"/"sleb"/"block"/"string"/"offset".
DW_FORMS = {
    0x03: 2, 0x04: 4, 0x05: 2, 0x06: 4, 0x07: 8, 0x08: "string", 0x09: "block", 0x0a: "block1",
    0x0b: 1, 0x0d: "sleb", 0x0e: "offset", 0x0f: "uleb", 0x1a: "uleb", 0x1e: 16, 0x1f: "offset",
    0x25: 1, 0x26: 2, 0x27: 3, 0x28: 4,
}


def elf_sections(data: bytes) -> tuple[list[dict], list[tuple[int, int]]]:
    """Section headers of a 32/64-bit, either-endian ELF file, and the file
    ranges of its own headers. Raises ValueError on anything malformed."""
    if len(data) < 16 or data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    wide, order = {1: False, 2: True}.get(data[4]), {1: "<", 2: ">"}.get(data[5])
    if wide is None or order is None:
        raise ValueError("unknown ELF class or data encoding")
    head = order + ("16xHHIQQQIHHHHHH" if wide else "16xHHIIIIIHHHHHH")
    if len(data) < struct.calcsize(head):
        raise ValueError("truncated ELF header")
    (_, _, _, _, phoff, shoff, _, ehsize, phentsize, phnum,
     shentsize, shnum, shstrndx) = struct.unpack_from(head, data, 0)
    section = order + ("IIQQQQIIQQ" if wide else "IIIIIIIIII")
    if ehsize < struct.calcsize(head) or ehsize > len(data):
        raise ValueError("invalid ELF header size")
    headers = [(0, ehsize)]
    if phnum:
        if phoff + phentsize * phnum > len(data):
            raise ValueError("program header table falls outside the file")
        headers.append((phoff, phoff + phentsize * phnum))
    if not shoff:
        if shnum:
            raise ValueError("section count without a section header table")
        return [], headers
    if shentsize != struct.calcsize(section) or shoff + shentsize > len(data):
        raise ValueError("section header table falls outside the file")
    first = struct.unpack_from(section, data, shoff)
    shnum = shnum or first[5]  # extended numbering keeps the count in section 0
    shstrndx = first[6] if shstrndx == 0xFFFF else shstrndx
    if not shnum or shoff + shentsize * shnum > len(data) or shstrndx >= shnum:
        raise ValueError("section header table falls outside the file")
    headers.append((shoff, shoff + shentsize * shnum))
    sections = []
    for index in range(shnum):
        name, kind, flags, _, offset, size, *_ = struct.unpack_from(section, data, shoff + index * shentsize)
        if kind != SHT_NOBITS and offset + size > len(data):
            raise ValueError(f"section {index} falls outside the file")
        sections.append({"name": name, "type": kind, "flags": flags, "offset": offset,
                         "size": 0 if kind == SHT_NOBITS else size})
    names = sections[shstrndx]
    for entry in sections:
        if entry["name"] >= names["size"]:
            raise ValueError("section name outside the section name table")
        start = names["offset"] + entry["name"]
        end = data.find(b"\0", start, names["offset"] + names["size"])
        if end < 0:
            raise ValueError("unterminated section name")
        entry["name"] = data[start:end].decode("latin-1")
    return sections, headers


def debug_line_strings(data: bytes, start: int, end: int, order: str):
    """Yield (offset, end) of the NUL-terminated strings stored inline in the
    directory and file tables of each .debug_line unit (DWARF 2-5); the
    line-number programs themselves are skipped. Raises ValueError."""
    def uleb(position):
        value = shift = 0
        while True:
            if position >= header_end:
                raise ValueError("truncated .debug_line header")
            byte = data[position]
            value |= (byte & 0x7F) << shift
            position, shift = position + 1, shift + 7
            if byte < 0x80:
                return value, position

    def string(position):
        nul = data.find(b"\0", position, header_end)
        if nul < 0:
            raise ValueError("unterminated .debug_line string")
        return nul + 1

    def fixed(position, size):
        if position + size > header_end:
            raise ValueError("truncated .debug_line header")
        return int.from_bytes(data[position:position + size], "big" if order == ">" else "little"), position + size

    position = start
    while position < end:
        header_end = end
        length, position = fixed(position, 4)
        offset_size = 4
        if length == 0xFFFFFFFF:
            length, position = fixed(position, 8)
            offset_size = 8
        unit_end = position + length
        if unit_end > end:
            raise ValueError(".debug_line unit runs past its section")
        header_end = unit_end
        version, position = fixed(position, 2)
        if not 2 <= version <= 5:
            raise ValueError(f"unsupported .debug_line version {version}")
        if version >= 5:
            position += 2  # address_size, segment_selector_size
        header_length, position = fixed(position, offset_size)
        header_end = position + header_length
        if header_end > unit_end:
            raise ValueError(".debug_line header runs past its unit")
        position += 4 if version < 4 else 5  # instruction length .. line_range
        opcode_base, position = fixed(position, 1)
        position += max(opcode_base - 1, 0)
        if version < 5:
            for table in range(2):  # include_directories, then file_names
                while True:
                    if position >= header_end:
                        raise ValueError("truncated .debug_line header")
                    if data[position] == 0:
                        position += 1
                        break
                    text_end = string(position)
                    yield position, text_end
                    position = text_end
                    if table:
                        for _ in range(3):  # directory index, time, length
                            _, position = uleb(position)
        else:
            for _ in range(2):  # directories, then file names
                count, position = fixed(position, 1)
                forms = []
                for _ in range(count):
                    _, position = uleb(position)
                    form, position = uleb(position)
                    if form not in DW_FORMS:
                        raise ValueError(f"unsupported .debug_line form 0x{form:x}")
                    forms.append(DW_FORMS[form])
                entries, position = uleb(position)
                for _ in range(entries):
                    for form in forms:
                        if form == "string":
                            text_end = string(position)
                            yield position, text_end
                            position = text_end
                        elif form in ("uleb", "sleb"):
                            _, position = uleb(position)
                        elif form == "block":
                            size, position = uleb(position)
                            position += size
                        elif form == "block1":
                            size, position = fixed(position, 1)
                            position += size
                        else:
                            position += offset_size if form == "offset" else form
                if position > header_end:
                    raise ValueError("truncated .debug_line header")
        position = unit_end


def elf_path_regions(data: bytes) -> list[tuple[int, int]]:
    """File ranges of an ELF where the generic path shapes apply: everything
    except its own headers and encoded debug/symbol data. Raises ValueError."""
    sections, headers = elf_sections(data)
    order = {1: "<", 2: ">"}[data[5]]
    noise = bytearray(len(data))  # 1 = encoded data, skipped by the generic shapes
    for start, end in headers:
        noise[start:end] = b"\1" * (end - start)
    strings = []
    for entry in sections:
        name, start, end = entry["name"], entry["offset"], entry["offset"] + entry["size"]
        if entry["flags"] & SHF_COMPRESSED or name.startswith(".zdebug"):
            raise ValueError(f"compressed section {name} cannot be inspected")
        if entry["flags"] & SHF_ALLOC or name in DEBUG_STRING_SECTIONS:
            continue  # program data and string sections: scanned
        if name.startswith(".debug_") or entry["type"] in (SHT_SYMTAB, SHT_DYNSYM, SHT_REL, SHT_RELA):
            noise[start:end] = b"\1" * (end - start)
            if name == ".debug_line":
                strings += debug_line_strings(data, start, end, order)
        # anything else (.comment, .strtab, .shstrtab, unknown sections): scanned
    for start, end in strings:
        noise[start:end] = bytes(end - start)
    return [(match.start(), match.end()) for match in re.finditer(rb"\x00+", noise)]


def allowlist() -> list[str]:
    names = [MANIFEST]
    for stem, (info, _) in TARGETS.items():
        names += [f"{stem}.elf", f"{stem}.dol", f"{stem}.map", info]
    return sorted(names)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def kind_of(name: str) -> str:
    if name == MANIFEST:
        return "manifest"
    if name.endswith("build-info.json"):
        return "build-info"
    return {".elf": "elf", ".dol": "dol", ".map": "linker-map"}[Path(name).suffix]


def text_runs(data: bytes, start: int = 0, end: int | None = None):
    """Yield (offset, text, is_c_string, bytes_per_char) for printable runs in
    data[start:end]; a run is a C string when its NUL lies inside that range."""
    end = len(data) if end is None else end
    for match in ASCII_RUN.finditer(data, start, end):
        yield match.start(), match.group().decode("ascii"), data[match.end():min(match.end() + 1, end)] == b"\0", 1
    for match in UTF16_RUN.finditer(data, start, end):
        yield (match.start(), match.group().decode("utf-16-le"),
               data[match.end():min(match.end() + 2, end)] == b"\0\0", 2)


def path_pattern(path: str) -> re.Pattern | None:
    parts = [part for part in re.split(r"[\\/]+", path.strip()) if part]
    if len(parts) < 2:
        return None  # too short to be a meaningful build-machine root
    anchor = "" if re.fullmatch(r"[A-Za-z]:", parts[0]) else "/?"
    return re.compile(anchor + r"[\\/]+".join(map(re.escape, parts)) + r"(?![\w.~-])", re.IGNORECASE)


def host_identity(environ=os.environ) -> tuple[list[str], list[str]]:
    """Paths and user names of the machine running the inspection."""
    paths = [os.getcwd()]
    for key in ("HOME", "USERPROFILE", "TEMP", "TMP", "RUNNER_TEMP", "GITHUB_WORKSPACE"):
        if environ.get(key):
            paths.append(environ[key])
    names = set()
    for key in ("USER", "USERNAME", "LOGNAME"):
        if environ.get(key):
            names.add(environ[key])
    for key in ("HOME", "USERPROFILE"):
        if environ.get(key):
            names.add(re.split(r"[\\/]+", environ[key].rstrip("\\/"))[-1])
    return paths, sorted(name for name in names if len(name) >= 3 and name.lower() not in GENERIC_NAMES)


def scan_content(name: str, data: bytes, forbid_paths, forbid_texts, findings, reviewed,
                 binary: bool = False, path_regions=None) -> None:
    """Report forbidden content. In ELF/DOL files the generic path shapes are
    matched only in NUL-terminated strings, where compilers, linkers and C code
    store paths, and only inside path_regions (for an ELF, elf_path_regions:
    not its encoded debug data, whose printable runs such as "/K/0" or "q:/)"
    are noise). Specific host paths/names, secrets and Xbox data signatures
    match on every byte of every file."""
    def add(category, offset):
        findings.append({"file": name, "category": category, "offset": offset})

    path_res = [pattern for pattern in map(path_pattern, forbid_paths) if pattern]
    reviewed_prefixes = REVIEWED_PATH_PREFIXES + (
        REVIEWED_ENGINE_PATH_PREFIXES if Path(name).stem in REVIEWED_ENGINE_STEMS else ())
    text_res = [re.compile(r"(?<![A-Za-z0-9])" + re.escape(text) + r"(?![A-Za-z0-9])", re.IGNORECASE)
                for text in forbid_texts if text]
    for region_start, region_end in [(0, len(data))] if path_regions is None else path_regions:
        for start, text, c_string, width in text_runs(data, region_start, region_end):
            for category, pattern in (PATH_PATTERNS if c_string or not binary else ()):
                for match in pattern.finditer(text):
                    rest = text[match.start():]
                    if any(rest.startswith(prefix) for prefix in reviewed_prefixes):
                        reviewed[name] = reviewed.get(name, 0) + 1
                    else:
                        add(category, start + width * match.start())
    for start, text, _, width in text_runs(data):
        for pattern in path_res:
            for match in pattern.finditer(text):
                add("build-host-path", start + width * match.start())
        for pattern in text_res:
            for match in pattern.finditer(text):
                add("forbidden-text", start + width * match.start())
        for category, pattern in SECRET_PATTERNS:
            for match in pattern.finditer(text):
                add(category, start + width * match.start())
    for magic, category in ((XISO_MAGIC, "xbox-image-signature"), (XBE_MAGIC, "xbox-executable-signature")):
        offset = data.find(magic)
        if offset >= 0:
            add(category, offset)
    offset = data.find(CACHE_HEAD)
    while offset >= 0:
        if data[offset + CACHE_FOOT_OFFSET:offset + CACHE_FOOT_OFFSET + 4] == CACHE_FOOT:
            add("halo-cache-header-signature", offset)
            break
        offset = data.find(CACHE_HEAD, offset + 1)


def check_build_info(name, stem, scope, record, directory, require_clean, problems):
    if not isinstance(record, dict) or record.get("schema_version") != 1:
        problems.append(f"{name}: not a schema 1 build-info object")
        return
    if record.get("scope") != scope:
        problems.append(f"{name}: scope is not {scope}")
    if record.get("runtime_verified") is not False:
        problems.append(f"{name}: runtime_verified must be false for a build-only artifact")
    if not re.fullmatch(r"[0-9a-f]{40}", str(record.get("source_commit", ""))):
        problems.append(f"{name}: source_commit is not a full commit id")
    if not isinstance(record.get("source_dirty"), bool) or (require_clean and record["source_dirty"]):
        problems.append(f"{name}: source tree was not clean at configure time")
    if record.get("path_prefix_map") != {"checkout": ".", "devkitpro": "/opt/devkitpro"}:
        problems.append(f"{name}: missing the host-path prefix map")
    artifacts = record.get("artifacts")
    expected = {f"{stem}.elf", f"{stem}.dol", f"{stem}.map"}
    if not isinstance(artifacts, dict) or set(artifacts) != expected:
        problems.append(f"{name}: artifacts must list exactly {sorted(expected)}")
        return
    for artifact, entry in artifacts.items():
        path = directory / artifact
        if path.is_file() and (entry.get("sha256") != sha256(path) or entry.get("bytes") != path.stat().st_size):
            problems.append(f"{name}: recorded hash/size of {artifact} does not match the file")


def build_manifest(directory: Path, infos: dict, packages: dict | None) -> dict:
    first = infos["probe"]
    return {
        "schema_version": 1,
        "label": LABEL,
        "verified": VERIFIED,
        "not_verified": NOT_VERIFIED,
        "contains_owned_game_data": False,
        "source_commit": first["source_commit"],
        "source_dirty": first["source_dirty"],
        "build_id": first["build_id"],
        "compiler": first.get("compiler"),
        "binutils": first.get("binutils"),
        "toolchain_packages": packages or {},
        "reviewed_upstream_path_prefixes": list(REVIEWED_PATH_PREFIXES),
        "scopes": {stem: info["scope"] for stem, info in infos.items()},
        "files": {name: {"kind": kind_of(name), "sha256": sha256(directory / name),
                         "bytes": (directory / name).stat().st_size}
                  for name in allowlist() if name != MANIFEST},
    }


def check_manifest(record, directory, infos, problems):
    if not isinstance(record, dict) or record.get("schema_version") != 1:
        problems.append(f"{MANIFEST}: not a schema 1 manifest")
        return
    if record.get("label") != LABEL:
        problems.append(f"{MANIFEST}: label must be exactly {LABEL!r}")
    if record.get("verified") != VERIFIED or record.get("not_verified") != NOT_VERIFIED:
        problems.append(f"{MANIFEST}: verified/not_verified claims differ from the build-only policy")
    if record.get("contains_owned_game_data") is not False:
        problems.append(f"{MANIFEST}: contains_owned_game_data must be false")
    packages = record.get("toolchain_packages")
    if not isinstance(packages, dict) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in packages.items()):
        problems.append(f"{MANIFEST}: toolchain_packages must map names to versions")
    files = record.get("files")
    expected = set(allowlist()) - {MANIFEST}
    if not isinstance(files, dict) or set(files) != expected:
        problems.append(f"{MANIFEST}: files must list exactly the allowlist")
    else:
        for name, entry in files.items():
            path = directory / name
            if path.is_file() and (entry.get("sha256") != sha256(path) or entry.get("bytes") != path.stat().st_size
                                   or entry.get("kind") != kind_of(name)):
                problems.append(f"{MANIFEST}: recorded kind/hash/size of {name} does not match the file")
    for key in ("source_commit", "source_dirty", "build_id"):
        values = {json.dumps(info.get(key)) for info in infos.values()} | {json.dumps(record.get(key))}
        if len(values) != 1:
            problems.append(f"{MANIFEST}: {key} differs between the manifest and build-info files")


def inspect(directory: Path, forbid_paths=(), forbid_texts=(), require_clean=False) -> dict:
    problems, findings, reviewed = [], [], {}
    allowed = set(allowlist())
    present = []
    if not directory.is_dir() or directory.is_symlink():
        return {"pass": False, "problems": [f"{directory.name}: not a directory"], "findings": [], "reviewed": {}}
    for path in sorted(directory.iterdir(), key=lambda p: p.name):
        if path.is_symlink() or not path.is_file():
            problems.append(f"{path.name}: only regular files are allowed (no links or directories)")
        elif path.name not in allowed:
            problems.append(f"{path.name}: not on the artifact allowlist")
        else:
            present.append(path.name)
    for name in sorted(allowed - set(present)):
        problems.append(f"{name}: required allowlisted file is missing")
    total = 0
    infos, manifest = {}, None
    for name in present:
        path = directory / name
        size = path.stat().st_size
        total += size
        if size > MAX_FILE_BYTES:
            problems.append(f"{name}: {size} bytes exceeds the {MAX_FILE_BYTES}-byte cap")
            continue
        data = path.read_bytes()
        kind = kind_of(name)
        try:
            if kind == "elf":
                verify_elf(path)
            elif kind == "dol":
                verify_dol(path)
            elif kind == "linker-map":
                text = data.decode("utf-8")
                if "\0" in text or "Memory Configuration" not in text or \
                        "Linker script and memory map" not in text:
                    raise ValueError("not a GNU ld linker map")
            else:
                record = json.loads(data.decode("utf-8"))
                if kind == "manifest":
                    manifest = record
                else:
                    stem = next(s for s, (info, _) in TARGETS.items() if info == name)
                    infos[stem] = record
        except (ValueError, UnicodeError) as error:
            problems.append(f"{name}: invalid {kind}: {error}")
        regions = None  # the whole file
        if kind == "elf":
            try:
                regions = elf_path_regions(data)
            except (ValueError, struct.error) as error:
                problems.append(f"{name}: unreadable ELF sections, scanned whole: {error}")
        scan_content(name, data, forbid_paths, forbid_texts, findings, reviewed, kind in ("elf", "dol"), regions)
    if total > MAX_TOTAL_BYTES:
        problems.append(f"total {total} bytes exceeds the {MAX_TOTAL_BYTES}-byte cap")
    for stem, record in infos.items():
        info, scope = TARGETS[stem]
        check_build_info(info, stem, scope, record, directory, require_clean, problems)
    if manifest is not None:
        check_manifest(manifest, directory, infos, problems)
    return {"pass": not problems and not findings, "problems": problems, "findings": findings,
            "reviewed": reviewed}


def read_packages(path: Path | None) -> dict:
    """Parse `pacman -Q` output ("name version" lines); other lines are ignored."""
    if path is None:
        return {}
    packages = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) == 2 and re.fullmatch(r"[\w.+@-]+", fields[0]) and re.fullmatch(r"[\w.+:~-]+", fields[1]):
            packages[fields[0]] = fields[1]
    return packages


def stage(build: Path, output: Path, packages: dict) -> None:
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("output must be a new or empty directory")
    infos = {}
    for stem, (info, _) in TARGETS.items():
        for name in (f"{stem}.elf", f"{stem}.dol", f"{stem}.map", info):
            if not (build / name).is_file():
                raise ValueError(f"missing build output {name}; run ninja wii_probe wii_gx_scene wii_gx_materials wii_geometry_view wii_memory_strategy")
    output.mkdir(parents=True, exist_ok=True)
    for stem, (info, _) in TARGETS.items():
        for name in (f"{stem}.elf", f"{stem}.dol", f"{stem}.map", info):
            shutil.copyfile(build / name, output / name)
        infos[stem] = json.loads((output / info).read_text(encoding="utf-8"))
    record = build_manifest(output, infos, packages)
    (output / MANIFEST).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    staging = commands.add_parser("stage", help="copy the allowlisted build outputs and write MANIFEST.json")
    staging.add_argument("--build", type=Path, default=Path("build/wii"))
    staging.add_argument("--output", type=Path, required=True)
    staging.add_argument("--toolchain-packages", type=Path, help="`pacman -Q` output to record in the manifest")
    checking = commands.add_parser("inspect", help="fail unless the directory is a clean build-only set")
    checking.add_argument("directory", type=Path)
    checking.add_argument("--forbid-path", action="append", default=[],
                          help="another build-machine path that must not appear (repeatable)")
    checking.add_argument("--forbid-text", action="append", default=[],
                          help="a user/owner name that must not appear (repeatable)")
    checking.add_argument("--no-host-identity", action="store_true",
                          help="do not add this machine's working directory, home, temp and user names")
    checking.add_argument("--require-clean", action="store_true", help="reject a build from a dirty tree")
    checking.add_argument("--json", action="store_true", help="print the result as JSON")
    args = parser.parse_args()
    if args.command == "stage":
        try:
            stage(args.build, args.output, read_packages(args.toolchain_packages))
        except (OSError, ValueError) as error:
            print(f"stage failed: {error}", file=sys.stderr)
            return 1
        print(f"Staged {len(allowlist())} allowlisted files ({LABEL}); run inspect before publishing.")
        return 0
    paths, names = ([], []) if args.no_host_identity else host_identity()
    result = inspect(args.directory, [*paths, *args.forbid_path], [*names, *args.forbid_text], args.require_clean)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for problem in result["problems"]:
            print(f"PROBLEM {problem}")
        groups = {}
        for finding in result["findings"]:
            groups.setdefault((finding["file"], finding["category"]), []).append(finding["offset"])
        for (name, category), offsets in sorted(groups.items()):
            print(f"FINDING {name} {category}: {len(offsets)} at byte {offsets[0]}"
                  + (" and later" if len(offsets) > 1 else ""))
        for name, count in sorted(result["reviewed"].items()):
            print(f"reviewed upstream toolchain path strings: {name} {count}")
        print(f"{'PASS' if result['pass'] else 'FAIL'} {LABEL} artifact inspection: "
              f"{len(result['problems'])} problems, {len(result['findings'])} findings. "
              "Not an emulator, gameplay, ABI, host-lifecycle or hardware result.")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
