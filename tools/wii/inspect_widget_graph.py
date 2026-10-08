"""Generate numeric goldens for the isolated DeLa reader, without exporting assets.

This checks the same bounded child/column-description graph as cache_widget_probe.c.
It does not render widgets or qualify other tag bodies, scripts, or engine behavior.
Input and output files must be outside this checkout. Output creation is exclusive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib


BASE = 0x803A6000
MAX_BYTES = 0x01600000
NONE = 0xFFFFFFFF
GROUP = 0x44654C61
ROOT_BYTES = 1004
MAX_DEPTH = 32
MAX_WORK = 1_000_000
MAX_NAME_SCAN = 64 * 1024 * 1024
BLOCK_OFFSETS = (72, 84, 96, 724, 992)
BLOCK_STRIDES = (36, 72, 34, 80, 80)
BLOCK_LIMITS = (64, 32, 32, 32, 32)
REFERENCE_OFFSETS = (56, 236, 252, 340, 356, 420)
REFERENCE_GROUPS = (0x6269746D, 0x75737472, 0x666F6E74,
                    0x6269746D, 0x6269746D, GROUP)
CHECKOUT = Path(__file__).resolve().parents[2]


class InspectionError(ValueError):
    """A bounded diagnostic failure containing no input text or file path."""


class Reader:
    def __init__(self, blob):
        if not isinstance(blob, bytes):
            raise InspectionError("input must be immutable bytes")
        if not 36 <= len(blob) <= MAX_BYTES:
            raise InspectionError("input size outside bounded tag region")
        self.blob = blob
        self.work = 0
        self.scan_bytes = 0
        self.names = {}
        self.claims = bytearray(len(blob))
        words = struct.unpack_from("<9I", blob)
        self.header = dict(zip(("instances_address", "scenario_datum", "checksum_word",
                               "tag_count", "vertex_count", "vertex_address",
                               "index_count", "index_address", "signature"), words))
        if words[8] != 0x74616773:
            raise InspectionError("header signature")
        count, vertices, indices = (self.s32(x) for x in (12, 16, 24))
        if not 1 <= count <= 65535 or vertices < 0 or indices < 0:
            raise InspectionError("header count")
        self.table = self.resolve(words[0], count, 32)
        self.resolve(words[5], vertices, 12)
        self.resolve(words[7], indices, 12)

    def tick(self):
        self.work += 1
        if self.work > MAX_WORK:
            raise InspectionError("inspection workload limit")

    def u32(self, offset):
        return struct.unpack_from("<I", self.blob, offset)[0]

    def s32(self, offset):
        return struct.unpack_from("<i", self.blob, offset)[0]

    def s16(self, offset):
        return struct.unpack_from("<h", self.blob, offset)[0]

    def resolve(self, address, count, stride):
        if count == 0:
            return (0, 0)
        offset, length = address - BASE, count * stride
        if count < 0 or offset < 0 or offset > len(self.blob) or length > len(self.blob) - offset:
            raise InspectionError("addressed span outside input")
        return (offset, length)

    def fixed_string(self, offset, length):
        end = self.blob.find(b"\0", offset, offset + length)
        if end < 0:
            raise InspectionError("unterminated bounded string")
        return {"offset": offset, "length": end - offset + 1}

    def name(self, address):
        self.tick()
        offset, _ = self.resolve(address, 1, 1)
        if offset not in self.names:
            remaining = MAX_NAME_SCAN - self.scan_bytes
            end = self.blob.find(b"\0", offset, min(len(self.blob), offset + remaining))
            if end < 0:
                raise InspectionError("unterminated name or name scan limit")
            length = end - offset + 1
            self.scan_bytes += length
            self.names[offset] = {"offset": offset, "length": length}
        return self.names[offset]

    def instance(self, datum, group):
        self.tick()
        ordinal = datum & 0xFFFF
        if datum == NONE or ordinal >= self.header["tag_count"]:
            raise InspectionError("datum ordinal")
        words = struct.unpack_from("<8I", self.blob, self.table[0] + ordinal * 32)
        if words[3] != datum:
            raise InspectionError("full datum identity")
        if words[0] != group:
            raise InspectionError("instance group")
        return {"group": words[0], "parent_groups": list(words[1:3]), "datum": words[3],
                "name_address": words[4], "root_address": words[5],
                "unused_words": list(words[6:8])}

    def reference(self, offset, expected_group):
        words = struct.unpack_from("<4I", self.blob, offset)
        result = dict(zip(("group", "name_address", "name_length_word", "datum"), words))
        if words[3] != NONE:
            if words[0] != expected_group:
                raise InspectionError("reference group")
            target = self.instance(words[3], expected_group)
            result["name_span"] = self.name(words[1])
            result["target_name_span"] = self.name(target["name_address"])
        return result

    def span_metadata(self, span):
        offset, length = span
        raw = memoryview(self.blob)[offset:offset + length]
        return {"offset": offset, "length": length,
                "sha256": hashlib.sha256(raw).hexdigest(), "crc32": zlib.crc32(raw)}

    def claim(self, span):
        start, length = span
        if not length:
            return
        end = start + length
        if self.claims.find(b"\1", start, end) >= 0:
            raise InspectionError("visited root/block overlap")
        self.claims[start:end] = b"\1" * length

    def element(self, kind, offset):
        self.tick()
        result = {"offset": offset}
        if kind == 0:
            result["function"] = self.s16(offset)
        elif kind == 1:
            result.update(flags=self.u32(offset), event_type=self.s16(offset + 4),
                          function=self.s16(offset + 6),
                          widget=self.reference(offset + 8, GROUP),
                          sound=self.reference(offset + 24, 0x736E6421),
                          text_span=self.fixed_string(offset + 40, 32))
        elif kind == 2:
            result.update(text_span=self.fixed_string(offset, 32), function=self.s16(offset + 32))
        else:
            result.update(widget=self.reference(offset, GROUP),
                          text_span=self.fixed_string(offset + 16, 32),
                          flags=self.u32(offset + 48), controller=self.s16(offset + 52))
            if kind == 4:
                result.update(vertical_offset=self.s16(offset + 54),
                              horizontal_offset=self.s16(offset + 56))
        return result

    def root(self, datum):
        self.tick()
        instance = self.instance(datum, GROUP)
        name = self.name(instance["name_address"])
        span = self.resolve(instance["root_address"], 1, ROOT_BYTES)
        offset = span[0]
        kind, controller, justification = (self.s16(offset + x) for x in (0, 2, 284))
        if not (0 <= kind < 7 and 0 <= controller < 5 and 0 <= justification < 3):
            raise InspectionError("root enum")
        refs = [self.reference(offset + x, g) for x, g in zip(REFERENCE_OFFSETS, REFERENCE_GROUPS)]
        node = {"datum": datum, "instance": instance, "instance_name_span": name,
                "raw": self.span_metadata(span), "type": kind, "controller": controller,
                "justification": justification, "name_span": self.fixed_string(offset + 4, 32),
                "bounds": [self.s16(offset + 36 + x * 2) for x in range(4)],
                "text_color_bits": [self.u32(offset + 268 + x * 4) for x in range(4)],
                "header_bounds": [self.s16(offset + 372 + x * 2) for x in range(4)],
                "footer_bounds": [self.s16(offset + 380 + x * 2) for x in range(4)],
                "flags": self.u32(offset + 44), "auto_close_word": self.u32(offset + 48),
                "fade_word": self.u32(offset + 52), "text_box_flags": struct.unpack_from("<H", self.blob, offset + 286)[0],
                "string_list_index": self.s16(offset + 302), "horizontal_offset": self.s16(offset + 304),
                "vertical_offset": self.s16(offset + 306), "list_flags": self.u32(offset + 336),
                "references": refs, "blocks": []}
        self.claim(span)
        for kind, (field, stride, limit) in enumerate(zip(BLOCK_OFFSETS, BLOCK_STRIDES, BLOCK_LIMITS)):
            count = self.s32(offset + field)
            address, definition = (self.u32(offset + field + x) for x in (4, 8))
            if not 0 <= count <= limit:
                raise InspectionError("block count")
            block_span = self.resolve(address, count, stride)
            self.claim(block_span)
            elements = [self.element(kind, block_span[0] + i * stride) for i in range(count)]
            node["blocks"].append({"count": count, "address": address, "definition_word": definition,
                                   "stride": stride, "maximum": limit,
                                   "raw": self.span_metadata(block_span), "elements": elements})
        return node


def inspect_bytes(blob, root_ordinal, node_capacity):
    """Inspect immutable bytes; only visited roots/blocks contribute serialized goldens.

    Limits are diagnostic resource policies. Identity validation for an unvisited
    reference does not validate its body. NONE metadata is preserved, never followed.
    """
    if (type(root_ordinal) is not int or type(node_capacity) is not int or
            not 1 <= node_capacity <= 65535):
        raise InspectionError("ordinal/capacity argument")
    reader = Reader(blob)
    if not 0 <= root_ordinal < reader.header["tag_count"]:
        raise InspectionError("root ordinal")
    datum = reader.u32(reader.table[0] + root_ordinal * 32 + 12)
    if datum & 0xFFFF != root_ordinal:
        raise InspectionError("selected ordinal/full datum identity")
    nodes = []
    states = {}
    heights = {}
    frames = []

    def discover(value):
        node = reader.root(value)
        nodes.append(node)
        states[value] = 1
        edges = [item["widget"]["datum"] for item in node["blocks"][4]["elements"]]
        if node["type"] == 3:
            edges.append(node["references"][5]["datum"])
        frames.append([value, edges, 0, 0])

    discover(datum)
    while frames:
        frame = frames[-1]
        value, edges, cursor, height = frame
        if cursor == len(edges):
            states[value] = 2
            heights[value] = height
            frames.pop()
            if frames:
                frames[-1][3] = max(frames[-1][3], height + 1)
            continue
        reader.tick()
        target = edges[cursor]
        frame[2] += 1
        if target == NONE:
            continue
        if states.get(target) == 1:
            raise InspectionError("graph cycle")
        if len(frames) + heights.get(target, 0) > MAX_DEPTH:
            raise InspectionError("graph depth")
        if states.get(target) == 2:
            frame[3] = max(frame[3], heights[target] + 1)
        else:
            if len(nodes) == node_capacity:
                raise InspectionError("node capacity")
            discover(target)
    digest = hashlib.sha256()
    crc = total = 0
    for node in nodes:
        for span in [node["raw"]] + [block["raw"] for block in node["blocks"]]:
            raw = memoryview(blob)[span["offset"]:span["offset"] + span["length"]]
            digest.update(raw)
            crc = zlib.crc32(raw, crc)
            total += len(raw)
    return {"format_version": 1, "scope": "bounded DeLa child/column-description numeric inspection",
            "input_sha256": hashlib.sha256(blob).hexdigest(), "input_bytes": len(blob),
            "goldens": {"root_datum": datum, "node_count": len(nodes), "serialized_bytes": total,
                        "serialized_crc32": crc, "serialized_sha256": digest.hexdigest()},
            "header": reader.header, "nodes": nodes,
            "limits": {"max_input_bytes": MAX_BYTES, "node_capacity": node_capacity,
                       "max_depth": MAX_DEPTH, "max_work": MAX_WORK, "max_name_scan_bytes": MAX_NAME_SCAN},
            "work": {"operations": reader.work, "distinct_name_scan_bytes": reader.scan_bytes}}


def external_path(path):
    resolved = Path(path).resolve()
    if resolved.is_relative_to(CHECKOUT):
        raise InspectionError("input/output must be outside the checkout")
    return resolved


def inspect_file(path, root_ordinal, node_capacity):
    path = external_path(path)
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not 36 <= before.st_size <= MAX_BYTES:
            raise InspectionError("input size outside bounded tag region")
        blob = stream.read(MAX_BYTES + 1)
        after = os.fstat(stream.fileno())
    current = path.stat()
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
    # Windows path stat and descriptor fstat can report different ctime semantics.
    # Compare ctime only between snapshots of the same open descriptor.
    if (identity(before) != identity(after) or before.st_ctime_ns != after.st_ctime_ns or
            identity(after) != identity(current) or len(blob) != before.st_size):
        raise InspectionError("input changed while reading")
    result = inspect_bytes(blob, root_ordinal, node_capacity)
    result["source_hashes"] = {name: hashlib.sha256((CHECKOUT / name).read_bytes()).hexdigest()
                               for name in ("tools/wii/inspect_widget_graph.py",
                                            "tools/wii/cache_widget_probe.c",
                                            "tools/wii/cache_widget_probe.h")}
    return result


def write_exclusive(path, result):
    """Publish a complete temporary file through an exclusive same-directory link.

    An existing destination is never replaced. Filesystems without hard-link
    support fail explicitly rather than weakening atomic/exclusive publication.
    """
    path = external_path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", newline="\n",
                                         dir=path.parent, prefix=".widget-goldens-", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(result, stream, indent=2, sort_keys=True, ensure_ascii=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blob", type=Path, help="external raw Xbox tag-region file")
    parser.add_argument("--root-ordinal", type=int, required=True)
    parser.add_argument("--node-capacity", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new external numeric JSON file")
    args = parser.parse_args(argv)
    try:
        output = external_path(args.output)
        if output.exists():
            raise InspectionError("output already exists")
        result = inspect_file(args.blob, args.root_ordinal, args.node_capacity)
        write_exclusive(output, result)
    except InspectionError as error:
        print("Widget inspection failed: " + str(error), file=sys.stderr)
        return 1
    except OSError as error:
        print("Widget inspection file operation failed (errno %s)." % error.errno, file=sys.stderr)
        return 1
    print("Widget numeric goldens: %d nodes, %d serialized bytes." %
          (result["goldens"]["node_count"], result["goldens"]["serialized_bytes"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
