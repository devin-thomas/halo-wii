#!/usr/bin/env python3
"""Generate the Wii engine build's MSVC semantics header (HWI-015).

tools/linux_msvc_semantics.py writes the header the Linux build
force-includes: file-scope struct/union tags, and `#pragma weak` for every
inline function name (MSVC's pick-any COMDAT). The Wii prefix
(port/wii/engine/include/halo_wii_prefix.h) makes `__inline` plain GNU89
inline rather than static, so every inline definition is external and the
pragma makes the copies pick-any. GCC rejects `#pragma weak` on a function
that is defined `static`, so the names whose definition is explicitly
`static __inline` (and the like) are left out here; they stay static.
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable, Set

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.linux_msvc_semantics import (read, render, scan_inline_functions,  # noqa: E402
                                        scan_tags, source_files)

STATIC_INLINE = re.compile(
    r"\bstatic\s+(?:__inline|_inline|__forceinline|D3DINLINE|FORCEINLINE)\b"
    r"[^;{}()]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{"
)


# port/include/xdk/xdk_d3d8.h's D3DINLINE, which the Linux scan leaves to its
# `static __forceinline` default and the Wii prefix makes plain inline
D3D_INLINE = re.compile(r"\bD3DINLINE\b[^;{}()]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{")


def d3d_inline_functions(files: Iterable[Path]) -> Set[str]:
    names: Set[str] = set()
    for path in files:
        names.update(D3D_INLINE.findall(read(path)))
    return names


def static_inline_functions(files: Iterable[Path]) -> Set[str]:
    names: Set[str] = set()
    for path in files:
        names.update(STATIC_INLINE.findall(read(path)))
    return names


def generate(tag_roots, inline_roots, static_roots) -> str:
    inline_files = source_files(inline_roots)
    weak = scan_inline_functions(inline_files, True) | d3d_inline_functions(inline_files)
    weak -= static_inline_functions(source_files(static_roots))
    text = render(scan_tags(source_files(tag_roots)), weak)
    return text.replace("tools/linux_msvc_semantics.py", "tools/wii/engine_semantics.py")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tags", type=Path, action="append", default=[])
    parser.add_argument("--inlines", type=Path, action="append", default=[])
    parser.add_argument("--static-scan", type=Path, action="append", default=[],
                        help="directory scanned for explicitly static inline definitions")
    args = parser.parse_args()
    for root in args.tags + args.inlines + args.static_scan:
        if not root.is_dir():
            sys.exit(f"{root} is not a directory")
    text = generate(args.tags, args.inlines, args.static_scan or args.inlines)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
