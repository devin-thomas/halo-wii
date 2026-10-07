"""Compile actual engine scalar functions in an isolated authored test harness.

Function bodies/actual memory headers are retained; the generated translation
unit replaces only MSVC ui64 literal suffixes with C ULL. No game code is edited.
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


SELECTED = {
    "source/memory/byte_swapping.c": ["byte_swap_memory"],
    "source/memory/data_encoding.c": [
        "data_encode_new", "data_encode_memory", "data_encode_integer",
        "data_decode_new", "data_decode_memory", "data_decode_byte",
        "data_decode_short", "data_decode_long", "data_decode_int64",
        "data_decode_integer",
    ],
}
HARNESS = Path("port/wii/abi/memory_subset")


def extract(text, name):
    pattern = rf"(?m)^(?:void|boolean|byte|short|long|__int64)[ \t]+(?:\*[ \t]*)?{re.escape(name)}\s*\("
    starts = list(re.finditer(pattern, text))
    if len(starts) != 1:
        raise ValueError(f"Expected one definition of {name}, found {len(starts)}")
    start = starts[0].start()
    brace = text.index("{", starts[0].end())
    # Ignore braces inside comments and quoted literals when finding body end.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         text[brace:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[start:brace + token.end()]
    raise ValueError(f"Unterminated definition of {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(".local/wii-memory-subset"))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--compile-only", action="store_true", help="compile objects without execution")
    mode.add_argument("--wii-devkitpro", type=Path, help="link an asset-free Wii ELF/DOL using this official SDK")
    args = parser.parse_args()
    compiler = args.cc.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    record_path = args.output / "subset-info.json"
    record_path.write_text('{"scope":"actual_scalar_subset","state":"incomplete"}\n', encoding="utf-8")
    environment = os.environ.copy()
    overrides = ("GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH", "CPATH",
                 "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                 "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES")
    for name in overrides:
        environment.pop(name, None)
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output([str(compiler), "-dumpmachine"], env=environment, text=True).strip()
    if target == "powerpc-eabi" and not (args.compile_only or args.wii_devkitpro):
        parser.error("powerpc-eabi requires --compile-only or --wii-devkitpro; target binaries cannot run on the host")
    if args.wii_devkitpro and target != "powerpc-eabi":
        parser.error("--wii-devkitpro requires a powerpc-eabi compiler")
    inputs = [*SELECTED, "source/memory/byte_swapping.h", "source/memory/data_encoding.h",
              "source/cseries/cseries.h", str(HARNESS / "shim.h"), str(HARNESS / "fixture.c"),
              str(HARNESS / "fixture.h"), Path(__file__).resolve().relative_to(Path.cwd()).as_posix()]
    sdk = None
    inventory = None
    sdk_identity = {}
    if args.wii_devkitpro:
        sdk = args.wii_devkitpro.resolve(strict=True)
        inventory = inspect_toolchain(sdk)
        if inventory["errors"]:
            raise ValueError("Wii preflight failed: " + "; ".join(inventory["errors"]))
        if compiler != Path(inventory["tools"]["powerpc-eabi-gcc"]["path"]).resolve():
            parser.error("Selected compiler does not belong to --wii-devkitpro")
        inputs += [str(HARNESS / "wii_main.c"), "tools/wii/build.py", "tools/wii/check_toolchain.py"]
        sdk_files = [sdk / "devkitPPC/wii_rules", compiler,
                     Path(inventory["tools"]["elf2dol"]["path"]),
                     sdk / "libogc/lib/wii/libfat.a", sdk / "libogc/lib/wii/libogc.a"]
        sdk_identity = {p.relative_to(sdk).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sdk_files}
        fingerprint = hashlib.sha256()
        for path in toolchain_inputs(sdk):
            fingerprint.update(path.relative_to(sdk).as_posix().encode() + b"\0")
            if path.is_file():
                fingerprint.update(hashlib.sha256(path.read_bytes()).digest())
        sdk_identity["toolchain_tree_sha256"] = fingerprint.hexdigest()
    input_hashes = {Path(name).as_posix(): hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in inputs}
    source_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    source_dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--", *map(str, inputs)], text=True))
    flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-multichar",
             "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-I", str(HARNESS), "-I", "source"]
    if target == "powerpc-eabi":
        flags += ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]
    elif target == "x86_64-w64-mingw32":
        # LLP64 size_t is 64 bits while long remains 32. Retain/report GCC's
        # warnings for the engine's -sizeof selectors rather than rewriting it.
        flags += ["-Wno-error=overflow"]
    pieces = ['#include "shim.h"\n']
    functions = {}
    macros = {}
    for name, selected in SELECTED.items():
        text = Path(name).read_text(encoding="utf-8")
        if name == "source/memory/byte_swapping.c":
            match = re.search(r"(?ms)^#define SWAP8\(q\).*?(?=\n\n)", text)
            if match is None:
                raise ValueError("Actual byte swapping source lost its SWAP8 macro")
            macro = match.group()
            macros["SWAP8"] = hashlib.sha256(macro.encode()).hexdigest()
            pieces.append(re.sub(r"\b(0x[0-9a-fA-F]+|[0-9]+)ui64\b", r"\1ULL", macro))
        for function in selected:
            body = extract(text, function)
            functions[function] = {"source": name, "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
            pieces.append(re.sub(r"\b(0x[0-9a-fA-F]+|[0-9]+)ui64\b", r"\1ULL", body))
    generated = args.output / "engine_scalars.c"
    generated.write_text("\n\n".join(pieces) + "\n", encoding="utf-8")
    sources = [generated, HARNESS / "fixture.c"]
    build_id = None
    if sdk is not None:
        build_id = hashlib.sha256(json.dumps({"source_commit": source_commit, "source_dirty": source_dirty,
                                               "inputs": input_hashes, "sdk": sdk_identity, "flags": flags},
                                              sort_keys=True).encode()).hexdigest()[:16]
        (args.output / "subset_build_id.h").write_text(f'#define WII_MEMORY_BUILD_ID "{build_id}"\n', encoding="utf-8")
        flags += ["-DWII_MEMORY_PPC", "-I", str(sdk / "libogc/include"), "-I", str(args.output)]
        sources.append(HARNESS / "wii_main.c")
    artifacts = {}
    report = []
    diagnostics = []

    def compile_checked(arguments):
        result = subprocess.run(arguments, env=environment, capture_output=True, text=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
            diagnostics.extend(result.stderr.splitlines())
        result.check_returncode()

    elf_record = None
    link_flags = []
    if args.compile_only or sdk is not None:
        objects = []
        for source in sources:
            obj = args.output / (source.stem + ".o")
            compile_checked([str(compiler), *flags, "-c", str(source), "-o", str(obj)])
            data = obj.read_bytes()
            if target == "powerpc-eabi" and (data[:6] != b"\x7fELF\x01\x02" or
                                             int.from_bytes(data[18:20], "big") != 20):
                raise ValueError(f"Expected ELF32 big-endian PowerPC object: {obj}")
            artifacts[obj.name] = hashlib.sha256(data).hexdigest()
            objects.append(str(obj))
        if sdk is not None:
            elf = args.output / "memory_subset.elf"
            dol = args.output / "memory_subset.dol"
            map_path = args.output / "memory_subset.map"
            link_flags = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float",
                          "-Wl,-Map," + str(map_path), "-L", str(sdk / "libogc/lib/wii"),
                          "-lfat", "-logc", "-lm"]
            compile_checked([str(compiler), *objects, *link_flags, "-o", str(elf)])
            elf_record = verify_elf(elf)
            compile_checked([inventory["tools"]["elf2dol"]["path"], str(elf), str(dol)])
            verify_dol(dol)
            for artifact in (elf, dol, map_path):
                artifacts[artifact.name] = hashlib.sha256(artifact.read_bytes()).hexdigest()
    else:
        executable = (args.output / ("fixture.exe" if os.name == "nt" else "fixture")).resolve()
        compile_checked([str(compiler), *flags, *map(str, sources), "-o", str(executable)])
        artifacts[executable.name] = hashlib.sha256(executable.read_bytes()).hexdigest()
        result = subprocess.run([str(executable)], env=environment, text=True, capture_output=True)
        report = (result.stdout + result.stderr).splitlines()
        print("\n".join(report))
        (args.output / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
        if result.returncode != 0:
            record_path.write_text(json.dumps({"scope": "actual_scalar_subset", "state": "execution_failed",
                                               "exit_code": result.returncode, "report": report}) + "\n", encoding="utf-8")
            raise SystemExit(result.returncode)
    record = {
        "scope": "actual_scalar_subset", "state": "compile_pass" if (args.compile_only or sdk) else "execution_pass",
        "source_commit": source_commit, "source_dirty": source_dirty,
        "compiler": subprocess.check_output([str(compiler), "--version"], env=environment, text=True).splitlines()[0],
        "target": target, "flags": flags, "functions": functions, "macros": macros,
        "cleared_child_environment_overrides": list(overrides),
        "inputs_sha256": input_hashes,
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "object_format": "ELF32 big-endian PowerPC" if target == "powerpc-eabi" else "native host",
        "artifacts_sha256": artifacts, "report": report, "compile_diagnostics": diagnostics,
        "limits": "scalar subset with authored assertion/memory services; no engine integration or alignment safety proof",
    }
    if sdk is not None:
        # Native SDK paths stay local; publishable records use root-relative paths.
        record["flags"] = [flag.replace(str(sdk), "<DEVKITPRO>") for flag in flags]
        record["link_flags"] = [flag.replace(str(sdk), "<DEVKITPRO>") for flag in link_flags]
        record.update({"build_id": build_id, "sdk_inputs_sha256": sdk_identity,
                       "elf": elf_record, "runtime_verified": False, "collect_failures": True})
    record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
