"""Native argument-vector build steps, keeping Windows and space-containing paths safe."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

# Published debug info and linker maps name the checkout and the devkitPro
# root by these host-independent prefixes instead of build-machine paths.
# /opt/devkitpro is the official installation prefix, which the official
# prebuilt libraries already record in their own debug info.
CHECKOUT_PREFIX = "."
DEVKITPRO_PREFIX = "/opt/devkitpro"

# ADR-018: the code-generation semantics every Wii *engine* translation unit
# compiles with, as the other native ports compile the game
# (tools/linux_build.py, tools/android_build.py). tools/wii_build.py composes
# them into ENGINE_CFLAGS. Authored Wii diagnostics that do not consume engine
# data keep the target defaults.
#  - -fsigned-char: plain char is signed on every other port; PowerPC EABI
#    makes it unsigned (recorded-animation char deltas and the players.h
#    char bit-field then read wrongly).
#  - -fshort-wchar: engine text is UTF-16 code units (tag data, profiles,
#    saves). Allowed only because no engine object calls a C-library wide
#    function (newlib's assume a 4-byte wchar_t): engine_wide_references()
#    is the gate, and the engine's wide calls go to 16-bit implementations
#    (port/linux/include/wchar.h and port/linux/src/msvc_wide.c, as on Linux
#    and Android), never to newlib's.
#  - -fno-builtin-<wide>: nor may the compiler synthesise such a call.
#  - the MSVC-tolerated-UB flags every port uses.
WIDE_BUILTINS = ("wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy", "wcsncpy",
                 "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy", "wmemmove", "wmemset")
ENGINE_SEMANTIC_FLAGS = ["-fsigned-char", "-fshort-wchar", "-fno-strict-aliasing", "-fwrapv",
                         "-fno-delete-null-pointer-checks", *(f"-fno-builtin-{name}" for name in WIDE_BUILTINS)]

# C-library functions whose behaviour depends on the size of wchar_t (C95
# <wchar.h>/<wctype.h>, the multibyte conversions, and the MSVC spellings the
# engine uses). An engine object compiled with -fshort-wchar must not
# reference any of them.
WIDE_LIBRARY_FUNCTIONS = frozenset((
    *WIDE_BUILTINS, "wcscoll", "wcsxfrm", "wcsstr", "wcsspn", "wcscspn", "wcspbrk", "wcstok", "wcstol",
    "wcstoul", "wcstoll", "wcstoull", "wcstod", "wcstof", "wcstold", "wcsdup", "wcsicmp", "wcsnicmp",
    "wcslwr", "wcsupr", "wcsftime", "wtoi", "wtol", "towlower", "towupper", "towctrans", "wctrans",
    "iswalpha", "iswupper", "iswlower", "iswdigit", "iswxdigit", "iswspace", "iswpunct", "iswalnum",
    "iswprint", "iswgraph", "iswcntrl", "iswblank", "iswascii", "iswctype", "wctype", "swprintf",
    "vswprintf", "snwprintf", "vsnwprintf", "wprintf", "vwprintf", "fwprintf", "vfwprintf", "swscanf",
    "vswscanf", "wscanf", "fwscanf", "fgetwc", "fputwc", "getwc", "putwc", "getwchar", "putwchar",
    "ungetwc", "fgetws", "fputws", "getws", "putws", "fwide", "wremove", "wtmpnam", "wctime", "wasctime",
    "wperror", "wcserror", "wfopen", "mbstowcs", "wcstombs", "mbtowc", "wctomb", "mbrtowc", "wcrtomb",
    "mbsrtowcs", "wcsrtombs", "btowc", "wctob", "mbrlen", "mbsinit"))


def wide_references_from_nm(text: str) -> list[str]:
    """The wide C-library functions among `nm -u` output's undefined symbols
    (MSVC spellings keep a leading underscore: _wcsicmp is wcsicmp)."""
    found = set()
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        if name in WIDE_LIBRARY_FUNCTIONS or name.lstrip("_") in WIDE_LIBRARY_FUNCTIONS:
            found.add(name)
    return sorted(found)


def engine_wide_references(nm: Path, objects) -> dict:
    """{object: [wide C-library functions it references]} for each engine
    object that references any; empty when the -fshort-wchar gate holds."""
    result = {}
    for obj in objects:
        listing = subprocess.run([str(nm), "-u", str(obj)], capture_output=True, text=True, check=True)
        names = wide_references_from_nm(listing.stdout)
        if names:
            result[Path(obj).name] = names
    return result


def prefix_map_flags(checkout: Path, root: Path) -> list[str]:
    return [f"-ffile-prefix-map={checkout}={CHECKOUT_PREFIX}",
            f"-ffile-prefix-map={root}={DEVKITPRO_PREFIX}"]


def normalize_link_map(path: Path, checkout: Path, root: Path) -> None:
    """Rewrite the build-machine roots in a GNU ld map like -ffile-prefix-map."""
    text = path.read_bytes().decode("utf-8", "surrogateescape")
    flags = re.IGNORECASE if os.name == "nt" else 0
    # Longest root first, so a toolchain inside the checkout keeps its prefix.
    for prefix, replacement in sorted(((root, DEVKITPRO_PREFIX), (checkout, CHECKOUT_PREFIX)),
                                      key=lambda item: -len(str(item[0]))):
        parts = [re.escape(part) for part in re.split(r"[\\/]+", str(prefix).rstrip("\\/")) if part]
        anchor = "" if os.name == "nt" else "/"
        pattern = r"(?<![\w.~-])" + anchor + r"[\\/]+".join(parts) + r"(?=[\\/]|$|\s|\()"
        text = re.sub(pattern, lambda _: replacement, text, flags=flags | re.MULTILINE)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(text.encode("utf-8", "surrogateescape"))
    temporary.replace(path)


def verify_elf(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x02\x01":
        raise ValueError("Expected a 32-bit big-endian ELF")
    kind, machine = struct.unpack_from(">HH", data, 16)
    if kind != 2 or machine != 20:
        raise ValueError("Expected an executable PowerPC ELF")
    entry, phoff = struct.unpack_from(">II", data, 24)
    phsize, phcount = struct.unpack_from(">HH", data, 42)
    if phsize != 32 or not phcount or phoff < 52 or phoff + phsize * phcount > len(data):
        raise ValueError("Invalid ELF program header table")
    executable_entry = False
    for index in range(phcount):
        kind, offset, address, _, filesz, memsz, flags, _ = struct.unpack_from(">8I", data, phoff + index * phsize)
        if kind != 1:
            continue
        if filesz > memsz or offset + filesz > len(data) or address + memsz > 0x100000000:
            raise ValueError("Invalid ELF load segment")
        if flags & 1 and filesz and address <= entry < address + filesz:
            executable_entry = True
    if not executable_entry:
        raise ValueError("ELF entry is outside executable load segments")
    return {"class": "ELF32", "endian": "big", "machine": "PowerPC",
            "entry_point": f"0x{struct.unpack_from('>I', data, 24)[0]:08x}"}


def verify_dol(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 0x100:
        raise ValueError("Truncated DOL header")
    offsets = struct.unpack_from(">18I", data, 0)
    addresses = struct.unpack_from(">18I", data, 0x48)
    sizes = struct.unpack_from(">18I", data, 0x90)
    spans = []
    memory_spans = []
    for offset, address, size in zip(offsets, addresses, sizes):
        if not size:
            continue
        if offset < 0x100 or offset + size > len(data):
            raise ValueError("DOL section falls outside the file")
        if not 0x80000000 <= address < address + size <= 0x81800000:
            raise ValueError("Probe DOL section is outside MEM1")
        if any(offset < end and start < offset + size for start, end in spans):
            raise ValueError("Overlapping DOL sections")
        if any(address < end and start < address + size for start, end in memory_spans):
            raise ValueError("Overlapping DOL memory sections")
        spans.append((offset, offset + size))
        memory_spans.append((address, address + size))
    bss, bss_size = struct.unpack_from(">II", data, 0xD8)
    if bss_size and not 0x80000000 <= bss < bss + bss_size <= 0x81800000:
        raise ValueError("Probe DOL BSS is outside MEM1")
    entry = struct.unpack_from(">I", data, 0xE0)[0]
    if not spans or not any(address <= entry < address + size for address, size in zip(addresses[:7], sizes[:7])):
        raise ValueError("DOL entry is outside its text sections")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("step", choices=["compile", "link", "convert", "manifest"])
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stem", default="probe", help="artifact stem for the manifest step")
    parser.add_argument("--scope", default="asset_free_probe", help="manifest scope label")
    parser.add_argument("--extra-flag", action="append", default=[], dest="extra_flags",
                        help="a compile flag after the configured ones (repeatable; spell it --extra-flag=-f...)")
    parser.add_argument("objects", nargs="*")
    # Intermixed parsing keeps trailing object files positional on Python < 3.12.
    args = parser.parse_intermixed_args()
    try:
        config = json.loads(args.config.read_text(encoding="utf-8"))
        root = Path(config["devkitpro"])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        compiler = config["tools"]["powerpc-eabi-gcc"]["path"]
        machine = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]
        if args.step == "compile":
            if args.source is None:
                parser.error("compile needs --source")
            subprocess.run([compiler, *config["public"]["compile_flags"], *args.extra_flags,
                            *prefix_map_flags(Path.cwd(), root),
                            "-I", str(root / "libogc/include"), "-I", "build/wii",
                            "-MMD", "-MF", str(args.output) + ".d", "-MT", args.output.as_posix(),
                            "-c", str(args.source), "-o", str(args.output)], check=True)
        elif args.step == "link":
            if not args.objects:
                parser.error("link needs object files")
            subprocess.run([compiler, *machine, "-g", "-Wl,-Map," + str(args.output.with_suffix(".map")),
                            *args.objects, "-L", str(root / "libogc/lib/wii"),
                            "-lfat", "-lwiiuse", "-lbte", "-logc", "-lm", "-o", str(args.output)], check=True)
            normalize_link_map(args.output.with_suffix(".map"), Path.cwd(), root)
            verify_elf(args.output)
        elif args.step == "convert":
            if args.source is None:
                parser.error("convert needs --source")
            verify_elf(args.source)
            subprocess.run([config["tools"]["elf2dol"]["path"], str(args.source), str(args.output)], check=True)
            verify_dol(args.output)
        else:
            build = args.output.parent
            record = dict(config["public"])
            record["scope"] = args.scope
            record["elf"] = verify_elf(build / f"{args.stem}.elf")
            verify_dol(build / f"{args.stem}.dol")
            record["artifacts"] = {
                name: {"sha256": hashlib.sha256((build / name).read_bytes()).hexdigest(),
                       "bytes": (build / name).stat().st_size}
                for name in (f"{args.stem}.elf", f"{args.stem}.dol", f"{args.stem}.map")
            }
            text = json.dumps(record, indent=2) + "\n"
            temporary = args.output.with_suffix(".json.tmp")
            temporary.write_text(text, encoding="utf-8")
            temporary.replace(args.output)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Wii {args.step} failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
