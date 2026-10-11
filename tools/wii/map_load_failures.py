#!/usr/bin/env python3
"""Make the staged-map load failures the Wii's cache layer must refuse (HWI-016C).

From a directory of stagings (tools/wii/map_stage.py: <map>.wmap, <map>.le.wmap,
manifest.txt, manifest.le.txt) and a format-1 staging of the same map
(HWI-015B's map_stage.py), this writes, in both byte orders, files that stand
for what goes wrong on a card, each under its own map name:

    <map>trunc   the staging cut to half its length; the manifest lists the
                 whole (a copy that did not finish)              -> damaged
    <map>flip    the staging with one byte of its tag data changed; the
                 manifest lists the staging (the card's bytes went bad)
                                                                  -> damaged
    <map>old     the format-1 staging; the manifest lists the new one
                                                                  -> stale
    <map>mm      the staging; the manifest lists another staging's digest
                                                                  -> manifest_mismatch
    <bsp map>bsp a multi-BSP staging with one byte of its second structure
                 BSP changed: it loads, and switching to that BSP is refused,
                 the BSP before kept                              -> damaged

A map that is not staged and a wrong data root need no file. The output
directory's manifest.txt and manifest.le.txt list the source's maps and these;
nothing in the source directory changes. The files hold owned data: keep them
private.

    python tools/wii/map_load_failures.py --staged <dir> --map prisoner --old <format-1 dir> \\
        --bsp-map a10 --output <new dir>
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import map_stage  # noqa: E402

ORDERS = (("", ">", "manifest.txt"), (".le", "<", "manifest.le.txt"))


def segments(data):
    """[(kind, size, file offset)] of a format-2 staging's segments"""
    order = ">" if data[8:12] == b"\x01\x02\x03\x04" else "<"
    count = struct.unpack_from(order + "I", data, 0x0c)[0]
    result = []
    for index in range(count):
        kind, _, size, _, at, _, _, _ = struct.unpack_from(order + "8I", data, map_stage.TABLE_BYTES + 32 * index)
        result.append((kind, size, at))
    return result


def flip(data, kind, ordinal):
    """data with one byte changed in the middle of its ordinal-th segment of that kind"""
    chosen = [segment for segment in segments(data) if segment[0] == kind][ordinal]
    out = bytearray(data)
    at = chosen[2] + chosen[1] // 2
    out[at] ^= 0x5A
    return bytes(out)


def manifest_rows(path):
    rows = path.read_text(encoding="ascii").splitlines()
    if not rows or rows[0] != f"{map_stage.MANIFEST_MAGIC} {map_stage.FORMAT_VERSION}":
        raise SystemExit(f"{path.name} is not a format {map_stage.FORMAT_VERSION} manifest")
    return {row.split()[0]: row for row in rows[1:]}


def renamed(row, name, digest=None):
    words = row.split()
    words[0] = name
    if digest is not None:
        words[3] = f"{digest:08x}"
    return " ".join(words)


def make(staged, name, old, bsp_name, output):
    output.mkdir(parents=True, exist_ok=False)
    made = {}
    for suffix, order, manifest_name in ORDERS:
        rows = manifest_rows(staged / manifest_name)
        data = (staged / f"{name}{suffix}.wmap").read_bytes()
        files = {
            f"{name}trunc": (data[:len(data) // 2], rows[name]),
            f"{name}flip": (flip(data, map_stage.SEGMENT_TAGS, 0), rows[name]),
            f"{name}old": ((old / f"{name}{suffix}.wmap").read_bytes(), rows[name]),
            f"{name}mm": (data, renamed(rows[name], f"{name}mm", map_stage.table_digest(
                (staged / f"{bsp_name}{suffix}.wmap").read_bytes()))),
        }
        bsp_data = (staged / f"{bsp_name}{suffix}.wmap").read_bytes()
        files[f"{bsp_name}bsp"] = (flip(bsp_data, map_stage.SEGMENT_BSP, 1), rows[bsp_name])
        lines = dict(rows)
        for fixture, (content, row) in files.items():
            (output / f"{fixture}{suffix}.wmap").write_bytes(content)
            lines[fixture] = row if row.split()[0] == fixture else renamed(row, fixture)
            made.setdefault(fixture, {})[order] = hashlib.sha256(content).hexdigest()
        text = f"{map_stage.MANIFEST_MAGIC} {map_stage.FORMAT_VERSION}\n" + \
            "".join(lines[key] + "\n" for key in sorted(lines))
        (output / manifest_name).write_text(text, encoding="ascii", newline="\n")
    return made


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--staged", type=Path, required=True)
    parser.add_argument("--map", required=True)
    parser.add_argument("--old", type=Path, required=True, help="a format-1 staging of the same map")
    parser.add_argument("--bsp-map", required=True, help="a staged map with at least two structure BSPs")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    made = make(args.staged, args.map, args.old, args.bsp_map, args.output)
    for fixture in sorted(made):
        print(fixture, made[fixture][">"][:16], made[fixture]["<"][:16])
    return 0


if __name__ == "__main__":
    sys.exit(main())
