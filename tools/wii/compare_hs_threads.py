#!/usr/bin/env python3
"""Compare the script threads' storage byte by byte across byte orders (HWI-033).

A boot launch with boot.txt dump_allocation=<the hs thread allocation's
index> writes the allocation's bytes at every sampled screen
(data/boot_dump_<frame>.bin, port/wii/render/render_ui.c); the host replay
(run_boot_host.py --dump) writes the same from the i686 build. This tool
proves, for every thread of every pair, whether the two hold the same values
in the same places:

- the thread's header, each live frame's header and each live frame's
  values (found from the top frame down, as hs_runtime.c lays them out) must
  split into cells of 1, 2 or 4 bytes each of which is the other's in the
  other byte order: then every value sits at the same offset on both ports,
  and only the order of its own bytes differs;
- a value that moved (written at another offset, or a byte of it outside
  its cell) fails that.

What bytes alone cannot tell: a 4-byte cell written as a long on one port
and as a short plus the bytes beside it (HS_CELL_SET_SHORT) on the other
holds each other's bytes reversed, as one long would. Which of the two a cell
is only the script function that allocated it knows; that the runtime reads
each cell the same way on both ports shows in its behaviour: the threads'
typed fields (sleep times, results, the frames' expressions) and the whole
game state compare equal screen by screen and tick by tick
(compare_boot_runs.py).

Popped frames' bytes above the top frame are reported apart: nothing reads
them (hs_stack_push zeroes a frame's values).

    python tools/wii/compare_hs_threads.py <Wii dump dir> <host dump dir>
"""
import argparse
import functools
import json
import struct
import sys
from pathlib import Path

DATA_ARRAY_BYTES = 56
THREAD_BYTES = 0x218
THREAD_HEADER_BYTES = 24
FRAME_HEADER_BYTES = 14


def cells_match(big, little):
    """whether big (big-endian storage) splits into 1-, 2- and 4-byte cells, each little's reversed"""
    length = len(big)
    if length != len(little):
        return False

    @functools.lru_cache(maxsize=None)
    def from_offset(k):
        if k == length:
            return True
        if k + 4 <= length and big[k:k + 4] == little[k:k + 4][::-1] and from_offset(k + 4):
            return True
        if k + 2 <= length and big[k:k + 2] == little[k:k + 2][::-1] and from_offset(k + 2):
            return True
        return big[k] == little[k] and from_offset(k + 1)

    return from_offset(0)


def thread_frames(datum, address):
    """(offset in the datum, value bytes) of each live frame, from the top down"""
    frames = []
    top = struct.unpack_from(">I", datum, 16)[0]
    frame = top
    while frame and len(frames) < 64:
        offset = frame - address
        if offset < THREAD_HEADER_BYTES or offset > THREAD_BYTES - FRAME_HEADER_BYTES:
            break
        previous, _, _, size = struct.unpack_from(">IiIh", datum, offset)
        if size < 0 or offset + FRAME_HEADER_BYTES + size > THREAD_BYTES:
            break
        frames.append((offset, size))
        frame = previous
    return frames


def compare_dump(big, little):
    """per thread: live storage the same values in the same places, and the dead bytes' state"""
    count = struct.unpack_from(">h", big, 46)[0]
    data = struct.unpack_from(">I", big, 52)[0]
    if struct.unpack_from(">h", big, 34)[0] != THREAD_BYTES:
        raise ValueError("not the hs thread array")
    threads = []
    for index in range(count):
        start = DATA_ARRAY_BYTES + index * THREAD_BYTES
        a, b = big[start:start + THREAD_BYTES], little[start:start + THREAD_BYTES]
        if a == b:
            threads.append({"index": index, "identical": True})
            continue
        frames = thread_frames(a, data + index * THREAD_BYTES)
        live_end = THREAD_HEADER_BYTES
        mismatched = []
        if not cells_match(a[:THREAD_HEADER_BYTES], b[:THREAD_HEADER_BYTES]):
            mismatched.append("thread header")
        for offset, size in frames:
            live_end = max(live_end, offset + FRAME_HEADER_BYTES + size)
            if not cells_match(a[offset:offset + FRAME_HEADER_BYTES], b[offset:offset + FRAME_HEADER_BYTES]):
                mismatched.append(f"frame header at {offset - THREAD_HEADER_BYTES}")
            values = slice(offset + FRAME_HEADER_BYTES, offset + FRAME_HEADER_BYTES + size)
            if not cells_match(a[values], b[values]):
                mismatched.append(f"frame values at {offset - THREAD_HEADER_BYTES}")
        threads.append({"index": index, "identical": False, "live_frames": len(frames),
                        "live_same_values_same_places": not mismatched, "mismatched": mismatched,
                        "dead_bytes": THREAD_BYTES - live_end,
                        "dead_same_values_same_places": cells_match(a[live_end:], b[live_end:])})
    return threads


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wii_dir", type=Path)
    parser.add_argument("host_dir", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    report = {}
    for path in sorted(args.wii_dir.glob("boot_dump_*.bin")):
        other = args.host_dir / path.name
        if not other.is_file():
            report[path.name] = "no host dump"
            continue
        report[path.name] = compare_dump(path.read_bytes(), other.read_bytes())
    ok = bool(report) and all(isinstance(v, list) and all(t.get("identical") or t["live_same_values_same_places"]
                                                          for t in v) for v in report.values())
    summary = {"dumps": len(report), "live_storage_same_values_same_places": ok,
               "byte_identical_dumps": sum(1 for v in report.values()
                                           if isinstance(v, list) and all(t.get("identical") for t in v))}
    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "dumps": report}, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(summary))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
