#!/usr/bin/env python3
"""Copy an engine unit for the Wii build with MSVC integer-literal suffixes
spelled for GCC (HWI-015).

MSVC (and clang with -fms-extensions) accept the suffixes i64/ui64/i32/ui32
on integer constants; GCC rejects them as invalid suffixes. The suffix is
part of the preprocessing number, so no macro can change it. The copy
rewrites exactly those suffixes (ui64 -> ULL, i64 -> LL, ui32 -> U, i32 ->
nothing) and nothing else, and starts with a #line directive naming the
original file, so diagnostics and debug information name the source. A
unit that needs no change fails the rewrite: the list of rewritten units in
tools/wii/engine_build.py stays exact.
"""

import argparse
import re
import sys
from pathlib import Path

SUFFIX = re.compile(r"\b((?:0[xX][0-9a-fA-F]+)|(?:[0-9]+))(ui64|i64|ui32|i32)\b")
REPLACEMENT = {"ui64": "ULL", "i64": "LL", "ui32": "U", "i32": ""}
COMMENT_OR_STRING = re.compile(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)


def rewrite(text: str):
    """(rewritten text, number of literals changed); comments and string
    literals are left alone"""
    count = 0
    pieces = []
    position = 0

    def code(segment: str) -> str:
        nonlocal count

        def replace(match):
            nonlocal count
            count += 1
            return match.group(1) + REPLACEMENT[match.group(2)]
        return SUFFIX.sub(replace, segment)

    for match in COMMENT_OR_STRING.finditer(text):
        pieces.append(code(text[position:match.start()]))
        pieces.append(match.group(0))
        position = match.end()
    pieces.append(code(text[position:]))
    return "".join(pieces), count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    text = args.source.read_text(encoding="latin-1").replace("\r\n", "\n")
    result, count = rewrite(text)
    if not count:
        print(f"{args.source}: no MSVC integer suffix to rewrite", file=sys.stderr)
        return 1
    header = f'#line 1 "{args.source.as_posix()}"\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header + result, encoding="latin-1", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
