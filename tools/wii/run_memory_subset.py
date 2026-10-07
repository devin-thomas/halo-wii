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
    parser.add_argument("--compile-only", action="store_true", help="compile objects without execution")
    args = parser.parse_args()
    compiler = args.cc.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    record_path = args.output / "subset-info.json"
    record_path.write_text('{"scope":"actual_scalar_subset","state":"incomplete"}\n', encoding="utf-8")
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output([str(compiler), "-dumpmachine"], env=environment, text=True).strip()
    if target == "powerpc-eabi" and not args.compile_only:
        parser.error("powerpc-eabi requires --compile-only; target objects cannot run on the host")
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

    if args.compile_only:
        for source in sources:
            obj = args.output / (source.stem + ".o")
            compile_checked([str(compiler), *flags, "-c", str(source), "-o", str(obj)])
            data = obj.read_bytes()
            if target == "powerpc-eabi" and (data[:6] != b"\x7fELF\x01\x02" or
                                             int.from_bytes(data[18:20], "big") != 20):
                raise ValueError(f"Expected ELF32 big-endian PowerPC object: {obj}")
            artifacts[obj.name] = hashlib.sha256(data).hexdigest()
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
    inputs = [*SELECTED, "source/memory/byte_swapping.h", "source/memory/data_encoding.h",
              "source/cseries/cseries.h", str(HARNESS / "shim.h"), str(HARNESS / "fixture.c"),
              Path(__file__).resolve().relative_to(Path.cwd()).as_posix()]
    record = {
        "scope": "actual_scalar_subset", "state": "compile_pass" if args.compile_only else "execution_pass",
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain", "--", *map(str, inputs)], text=True)),
        "compiler": subprocess.check_output([str(compiler), "--version"], env=environment, text=True).splitlines()[0],
        "target": target, "flags": flags, "functions": functions, "macros": macros,
        "inputs_sha256": {Path(name).as_posix(): hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in inputs},
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "object_format": "ELF32 big-endian PowerPC" if target == "powerpc-eabi" else "native host",
        "artifacts_sha256": artifacts, "report": report, "compile_diagnostics": diagnostics,
        "limits": "scalar subset with authored assertion/memory services; no engine integration or alignment safety proof",
    }
    record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
