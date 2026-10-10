"""Build the HWI-006 calling/bitfield/atomic/math semantics fixture.

Actual engine declarations and function bodies are extracted verbatim from the
source tree into generated translation units; only service macros, Win32
spellings and the report harness are authored (port/wii/abi/semantics). The
same sources build for the host (and execute) or for the Wii (ELF/DOL only;
run the DOL in Dolphin separately and compare with compare_abi_semantics.py).
No game code is edited and no game data is read.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from build import verify_dol, verify_elf
from check_toolchain import inspect_toolchain, toolchain_inputs


HARNESS = Path("port/wii/abi/semantics")
MUSL = Path("port/third_party/musl-math")

# (source, kind, selector). Kinds: line = one exact line; define = one #define;
# enum = the enum block containing the anchor text; aggregate = struct/union by
# tag; anonymous_union = the union block starting at the anchor (wrapped in a
# named authored struct); function = one definition; include = an actual header.
DECLARATIONS = [
    ("source/cseries/cseries.h", "line", "typedef unsigned char byte;"),
    ("source/cseries/cseries.h", "line", "typedef unsigned short word;"),
    ("source/cseries/cseries.h", "line", "typedef float real;"),
    ("source/cseries/cseries.h", "line", "typedef byte boolean;"),
    ("source/cseries/cseries.h", "enum", "\n\tTICKS_PER_SECOND = 30,"),
    ("source/cseries/cseries.h", "enum", "\n\t_x = 0,"),
    ("source/cseries/cseries.h", "define", "NONE"),
    ("source/cseries/cseries.h", "define", "MIN"),
    ("source/cseries/cseries.h", "define", "MAX"),
    ("source/cseries/cseries.h", "define", "FLOOR"),
    ("source/cseries/cseries.h", "define", "CEILING"),
    ("source/cseries/cseries.h", "define", "PIN"),
    ("source/cseries/cseries.h", "define", "FLAG"),
    ("source/cseries/cseries.h", "define", "TEST_FLAG"),
    ("source/cseries/cseries.h", "function", "fast_ftol"),
    ("source/bungie_net/common/message_header.h", "include", "bungie_net/common/message_header.h"),
    ("source/bungie_net/common/message_encryption.h", "include", "bungie_net/common/message_encryption.h"),
    ("source/bungie_net/common/message_encryption.c", "enum", "\n\tMESSAGE_ENCRYPTED_FLAG = FLAG(0),"),
    ("source/bungie_net/common/message_encryption.c", "aggregate", "union message_header_value"),
    ("source/cutscene/recorded_animation_playback.c", "enum", "\n\t_time_delta_zero,"),
    ("source/cutscene/recorded_animation_playback.c", "aggregate", "struct animation_event_header"),
    ("source/cutscene/recorded_animation_playback.c", "aggregate", "struct vector_char_difference_data"),
    ("source/game/players.h", "enum", "\n\t_local_player_triggered_switch_none = NONE,"),
    ("source/game/players.h", "anonymous_union", "union\n\t{\n\t\tbyte bsp_switch_state;"),
    ("source/interface/hud_nav_points.c", "aggregate", "struct hud_nav_point_datum"),
    ("source/math/real_math.h", "include", "math/real_math.h"),
    ("source/math/random_math.c", "enum", "\n\tRANDOM_A= 1664525L,"),
]

# File-private data placed ahead of the extracted bodies in abi_engine.c.
BODY_DECLARATIONS = [
    ("source/math/matrix_math.c", "line", "static short data_0030790c[7] = { 1, 2, 0, 0, 1, 2, 0 };"),
    ("source/math/matrix_math.c", "define", "matrix4x3_next"),
]

FUNCTIONS = {
    "source/bungie_net/common/message_header.c": ["build_message_header", "byte_swap_message_header"],
    "source/bungie_net/common/message_encryption.c": [
        "reversible_crypt", "tea_encipher", "tea_decipher", "message_encrypt", "message_decrypt"],
    "port/linux/src/xbox_kernel.c": [
        "halo_linux_InterlockedIncrement", "halo_linux_InterlockedDecrement",
        "halo_linux_InterlockedExchange", "halo_linux_InterlockedExchangeAdd",
        "halo_linux_InterlockedCompareExchange"],
    "source/math/real_math.c": [
        "perpendicular3d", "rotate_vector_about_axis", "quaternions_multiply", "quaternions_interpolate",
        "quaternion_transform_point", "quaternion_normalize", "angle_between_vectors3d"],
    "source/math/matrix_math.c": [
        "matrix4x3_rotation_from_quaternion", "matrix4x3_rotation_to_quaternion", "matrix4x3_inverse",
        "matrix4x3_transform_point", "matrix4x3_transform_vector", "matrix4x3_multiply"],
    "source/math/random_math.c": ["seed_random", "seed_random_range", "real_seed_random", "real_seed_random_range"],
}

AUTHORED = ["report.c", "calls.c", "semantics.c", "engine_probe.c", "bitfield_boundary.c", "math_corpus.c", "fpenv.c",
            "main.c"]
AUTHORED_HEADERS = ["report.h", "calls.h", "semantics.h", "engine_shim.h", "engine_probe.h", "bitfield_boundary.h",
                    "math_corpus.h", "fpenv.h"]

STRICT = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off"]
# As every other native port compiles the game (tools/linux_build.py,
# tools/android_build.py), minus x86-only switches.
ENGINE = ["-fno-strict-aliasing", "-fwrapv", "-fno-delete-null-pointer-checks"]
# Per-unit exceptions for the generated actual-engine units only, each tied to
# unchanged source text: comparisons of unsigned words with 0
# (message_encryption.c/message_header.c assertions) and unused parameters of
# extracted inline helpers.
ENGINE_EXCEPTIONS = ["-Wno-type-limits", "-Wno-unused-parameter"]
MUSL_FLAGS = ["-std=gnu11", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-fwrapv", "-w",
              "-I", str(MUSL / "include"), "-include", str(MUSL / "include/libm.h")]
WII_MACHINE = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]


def unique(pattern, text, what, flags=0):
    matches = list(re.finditer(pattern, text, flags))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {what}, found {len(matches)}")
    return matches[0]


def balanced(text, brace):
    """End index (exclusive) of the brace block opening at text[brace]."""
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[brace:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return brace + token.end()
    raise ValueError("unterminated block")


def extract_function(text, name):
    candidates = []
    for match in re.finditer(rf"(?m)^([^\n;{{}}#/]*?)\b{re.escape(name)}\s*\(", text):
        close = text.index(")", match.end())
        rest = text[close + 1:]
        stripped = rest.lstrip()
        if not stripped.startswith("{"):
            continue
        start = match.start()
        if not match.group(1).strip():
            # return type on the previous line ("void\nname(")
            previous = text.rfind("\n", 0, start - 1)
            line = text[previous + 1:start - 1]
            if line.strip() and not re.search(r"[;{}]\s*$|\*/\s*$|^\s*#", line):
                start = previous + 1
        brace = close + 1 + (len(rest) - len(stripped))
        candidates.append(text[start:balanced(text, brace)])
    if len(candidates) != 1:
        raise ValueError(f"expected one definition of {name}, found {len(candidates)}")
    return candidates[0]


def extract_enum(text, anchor):
    position = text.find(anchor)
    if position < 0 or text.find(anchor, position + 1) >= 0:
        raise ValueError(f"enum anchor must occur once: {anchor!r}")
    start = max(m.start() for m in re.finditer(r"(?m)^enum\b[^\n;]*\n\{", text[:position]))
    end = balanced(text, text.index("{", start))
    if not text[end:].startswith(";"):
        raise ValueError(f"enum block for {anchor!r} lacks its semicolon")
    return text[start:end + 1]


def extract_aggregate(text, tag):
    match = unique(rf"(?m)^{re.escape(tag)}\s*\n\{{", text, tag)
    end = balanced(text, text.index("{", match.start()))
    if not text[end:].startswith(";"):
        raise ValueError(f"{tag} lacks its semicolon")
    return text[match.start():end + 1]


def extract_anonymous_union(text, anchor):
    start = text.find(anchor)
    if start < 0 or text.find(anchor, start + 1) >= 0:
        raise ValueError("anonymous union anchor must occur once")
    end = balanced(text, text.index("{", start))
    if not text[end:].startswith(";"):
        raise ValueError("anonymous union lacks its semicolon")
    return text[start:end + 1]


def extract(path, kind, selector):
    text = Path(path).read_text(encoding="utf-8")
    if kind == "line":
        unique(rf"(?m)^{re.escape(selector)}$", text, selector)
        return selector
    if kind == "define":
        return unique(rf"(?m)^#define {re.escape(selector)}\b.*$", text, "#define " + selector).group()
    if kind == "enum":
        return extract_enum(text, selector)
    if kind == "aggregate":
        return extract_aggregate(text, selector)
    if kind == "anonymous_union":
        body = extract_anonymous_union(text, selector)
        return "struct wii_abi_players_bsp_switch_excerpt\n{\n" + body + "\n};"
    if kind == "function":
        return extract_function(text, selector)
    if kind == "include":
        return f'#include "{selector}"'
    raise ValueError(kind)


def generate(output):
    record = {}
    types = ["/* GENERATED by tools/wii/run_abi_semantics.py from actual engine sources. Do not edit. */",
             "#ifndef WII_ABI_ENGINE_TYPES_H", "#define WII_ABI_ENGINE_TYPES_H", '#include "engine_shim.h"']
    for path, kind, selector in DECLARATIONS:
        piece = extract(path, kind, selector)
        record.setdefault(path, []).append({"kind": kind, "selector": selector,
                                            "sha256": hashlib.sha256(piece.encode()).hexdigest()})
        types.append(f"/* {path}: {kind} */\n{piece}")
    types += ['#include "engine_shim_prototypes.h"', "#endif"]
    body = ["/* GENERATED by tools/wii/run_abi_semantics.py from actual engine sources. Do not edit. */",
            '#include "abi_engine_types.h"']
    for path, kind, selector in BODY_DECLARATIONS:
        piece = extract(path, kind, selector)
        record.setdefault(path, []).append({"kind": kind, "selector": selector,
                                            "sha256": hashlib.sha256(piece.encode()).hexdigest()})
        body.append(f"/* {path}: {kind} */\n{piece}")
    for path, names in FUNCTIONS.items():
        text = Path(path).read_text(encoding="utf-8")
        for name in names:
            piece = extract_function(text, name)
            record.setdefault(path, []).append({"kind": "function", "selector": name,
                                                "sha256": hashlib.sha256(piece.encode()).hexdigest()})
            body.append(f"/* {path}: {name} */\n{piece}")
    (output / "abi_engine_types.h").write_text("\n\n".join(types) + "\n", encoding="utf-8")
    (output / "abi_engine.c").write_text("\n\n".join(body) + "\n", encoding="utf-8")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wii-devkitpro", type=Path)
    args = parser.parse_args()
    root = Path.cwd().resolve()
    output = args.output.resolve()
    if not output.is_relative_to(root) or output == root:
        parser.error("output must be an ignored subdirectory of this checkout")
    if subprocess.run(["git", "check-ignore", "--quiet", str(output) + os.sep]).returncode != 0:
        parser.error("output must be ignored by Git")
    if output.exists():
        parser.error("new output directory required; existing evidence is preserved")
    compiler = args.cc.resolve(strict=True)
    environment = os.environ.copy()
    overrides = ("GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH",
                 "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES")
    for name in overrides:
        environment.pop(name, None)
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output([str(compiler), "-dumpmachine"], env=environment, text=True).strip()
    if (target == "powerpc-eabi") != bool(args.wii_devkitpro):
        parser.error("PowerPC requires --wii-devkitpro; the host compiler must omit it")
    musl_sources = sorted((MUSL / "src").glob("*.c"))
    inputs = [Path(__file__).resolve().relative_to(root),
              Path("tools/wii/build.py"), Path("tools/wii/check_toolchain.py"),
              *(HARNESS / n for n in AUTHORED + AUTHORED_HEADERS + ["engine_shim_prototypes.h"]),
              *sorted({Path(p) for p, _, _ in DECLARATIONS}), *map(Path, FUNCTIONS),
              Path("source/math/integer_math.h"), Path("port/include/halo_math.h"),
              *musl_sources, *sorted((MUSL / "include").glob("*.h")), *sorted((MUSL / "src").glob("*.h"))]
    inputs = sorted(set(Path(p.as_posix()) for p in inputs), key=lambda p: p.as_posix())
    hashes = {p.as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    source_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--", *map(str, inputs)], text=True))
    inventory = None
    sdk_identity = {}
    if args.wii_devkitpro:
        sdk = args.wii_devkitpro.resolve(strict=True)
        inventory = inspect_toolchain(sdk)
        if inventory["errors"]:
            raise ValueError("Wii preflight failed: " + "; ".join(inventory["errors"]))
        if compiler != Path(inventory["tools"]["powerpc-eabi-gcc"]["path"]).resolve():
            parser.error("compiler does not belong to the selected SDK")
        fingerprint = hashlib.sha256()
        for p in toolchain_inputs(sdk):
            fingerprint.update(p.relative_to(sdk).as_posix().encode() + b"\0")
            if p.is_file():
                fingerprint.update(hashlib.sha256(p.read_bytes()).digest())
        sdk_identity["toolchain_tree_sha256"] = fingerprint.hexdigest()
    machine = WII_MACHINE if inventory else []
    base_includes = ["-I", str(HARNESS), "-I", "port/include", "-I", "source"]
    record = {"scope": "hwi006_calls_varargs_bitfields_atomics_intrinsics_math_fixture",
              "source_commit": source_commit, "source_dirty": dirty, "inputs_sha256": hashes,
              "compiler": subprocess.check_output([str(compiler), "--version"], env=environment, text=True).splitlines()[0],
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "target": target,
              "sdk_identity": sdk_identity, "flags": {
                  "strict": STRICT + machine, "engine": STRICT + ENGINE + ENGINE_EXCEPTIONS + machine,
                  "candidate_signed_char": STRICT + ENGINE + ["-fsigned-char"] + machine,
                  "musl": MUSL_FLAGS + machine},
              "commands": [], "state": "incomplete"}
    record["build_id"] = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()[:16]
    output.mkdir(parents=True)
    record_path = output / "semantics-info.json"

    def save():
        record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    try:
        record["extracted"] = generate(output)
    except ValueError as error:
        record["state"] = "extraction_failed"
        record["error"] = str(error)
        save()
        raise
    for name in ("abi_engine_types.h", "abi_engine.c"):
        record.setdefault("generated_sha256", {})[name] = hashlib.sha256((output / name).read_bytes()).hexdigest()
    (output / "abi_semantics_build.h").write_text(f'#define WII_ABI_SEMANTICS_BUILD_ID "{record["build_id"]}"\n',
                                                  encoding="ascii")
    save()

    def run(argv, check=True):
        p = subprocess.run(argv, env=environment, capture_output=True, text=True)
        record["commands"].append({"arguments": [str(a) for a in argv], "exit_code": p.returncode,
                                   "stdout_lines": len(p.stdout.splitlines()), "stderr": p.stderr.splitlines()})
        save()
        if p.stderr:
            print(p.stderr, end="", file=sys.stderr)
        if check:
            p.check_returncode()
        return p
    objects = []
    includes = base_includes + ["-I", str(output)]
    if inventory:
        includes += ["-I", str(args.wii_devkitpro.resolve(strict=True) / "libogc/include")]
    engine_units = ("engine_probe.c", "math_corpus.c")
    units = [(HARNESS / n, STRICT + machine + includes, n[:-2]) for n in AUTHORED if n not in engine_units]
    units += [(HARNESS / "math_corpus.c", STRICT + ENGINE + machine + includes, "math_corpus"),
              (output / "abi_engine.c", STRICT + ENGINE + ENGINE_EXCEPTIONS + machine + includes, "abi_engine"),
              (HARNESS / "engine_probe.c", STRICT + ENGINE + machine + includes +
               ["-DWII_ABI_PROBE_VARIANT=baseline"], "engine_probe_baseline"),
              (HARNESS / "engine_probe.c", STRICT + ENGINE + ["-fsigned-char"] + machine + includes +
               ["-DWII_ABI_PROBE_VARIANT=signed_char", "-DWII_ABI_PROBE_CANDIDATE=1"], "engine_probe_signed_char")]
    units += [(s, MUSL_FLAGS + machine, "musl_" + s.stem) for s in musl_sources]
    for source, flags, stem in units:
        obj = output / (stem + ".o")
        run([str(compiler), *flags, "-c", str(source), "-o", str(obj)])
        objects.append(str(obj))
    if inventory:
        sdk = args.wii_devkitpro.resolve(strict=True)
        elf, dol = output / "abi_semantics.elf", output / "abi_semantics.dol"
        run([str(compiler), *objects, *WII_MACHINE[1:], "-Wl,-Map," + str(output / "abi_semantics.map"),
             "-L", str(sdk / "libogc/lib/wii"), "-lfat", "-logc", "-lm", "-o", str(elf)])
        record["elf"] = verify_elf(elf)
        run([inventory["tools"]["elf2dol"]["path"], str(elf), str(dol)])
        verify_dol(dol)
        record["state"] = "compile_pass"
    else:
        exe = output / ("abi_semantics.exe" if os.name == "nt" else "abi_semantics")
        run([str(compiler), *objects, "-o", str(exe)])
        executed = run([str(exe)], check=False)
        (output / "host-report.txt").write_text(executed.stdout.replace("\r\n", "\n"), encoding="ascii")
        record["host_exit_code"] = executed.returncode
        record["report_sha256"] = hashlib.sha256(executed.stdout.replace("\r\n", "\n").encode()).hexdigest()
        record["summary"] = [l for l in executed.stdout.splitlines() if " SUMMARY " in l]
        record["state"] = "execution_complete"
        for line in record["summary"]:
            print(line)
    record["artifacts_sha256"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.iterdir())
                                  if p.suffix in (".exe", ".elf", ".dol", ".map") or p.name == "abi_semantics"}
    save()
    print(f"ABI semantics build={record['build_id']} state={record['state']}")


if __name__ == "__main__":
    main()
