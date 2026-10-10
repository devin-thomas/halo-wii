#!/usr/bin/env python3
"""Stage a map's environment render resources for the Wii rasterizer (HWI-016B).

The Wii rasterizer's environment path (port/wii/render) reads, per structure
BSP, its converted geometry (HWL1) and the textures (HWT1) of its lightmap
pages and of its environment shaders' base maps, from the content
pipeline's outputs (tools/wii/content_convert.py, one generation):

    sd:/halo-wii-engine/data/render/<name>/lightmaps/<bsp tag:05>.hwl
    sd:/halo-wii-engine/data/render/<name>/textures/<bitmap tag:05>-<bitmap:03>.hwt

This tool chooses exactly the files the guest asks for, from the owner's map
(the tags the engine reads: each BSP material's shader, its type and base
map, the base map's bitmap count) and the generation's manifest, checks each
file's SHA-256 against the manifest, copies them under --out and writes
render-stage.json. Everything it writes is private (converted game data):
never commit or publish it. map_stage.py stages the map itself.

    python tools/wii/render_stage.py --generation <generation dir> --map <owner's .map> \\
        --name <map> --out <new private dir>

It prints one run_dolphin.py --stage argument per file (--stage-args).
"""
import argparse
import hashlib
import json
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import be_records as br  # noqa: E402
import content_geometry as cg  # noqa: E402
import halo_cache as hc  # noqa: E402

SD_ROOT = "sd:/halo-wii-engine/data/render"
# the engine's shader layout (source/shaders, rasterizer_xbox_environment.c)
SHADER_TYPE_OFFSET = 0x24
SHADER_ENVIRONMENT = 3
SHADER_BASE_MAP_INDEX = 0x88 + 12   # shader_environment diffuse.base_map.index
SHADER_ROOT_BYTES = 0x2D4
BITMAP_GROUP_BITMAPS = 0x60          # bitmap_group.bitmaps (tag_block count)
BITMAP_ROOT_BYTES = 0x6C
NONE = 0xFFFFFFFF


class StageError(ValueError):
    """A bounded failure; the message names no tag or file contents."""


def lightmap_path(name, bsp_tag):
    return f"render/{name}/lightmaps/{bsp_tag:05d}.hwl"


def texture_path(name, tag, bitmap):
    return f"render/{name}/textures/{tag:05d}-{bitmap:03d}.hwt"


def hwl_records(blob):
    """(lightmap bitmap datum, [(page, first material, count)], [(shader datum, permutation, surfaces,
    lightmap vertex count)]) of an HWL1 file"""
    sections = cg.unpack_lightmaps(blob)
    bsp = br.decode_records(sections[1][1], cg.BSP_FMT, 1, ">")
    lightmaps = br.decode_records(sections[2][1], cg.LIGHTMAP_FMT, sections[2][0], ">")
    width = len(struct.unpack("<" + cg.MATERIAL_FMT, bytes(cg.HWL_SPEC[3])))
    values = br.decode_records(sections[3][1], cg.MATERIAL_FMT, sections[3][0], ">")
    pages = [(lightmaps[i * 4], lightmaps[i * 4 + 2], lightmaps[i * 4 + 3]) for i in range(sections[2][0])]
    materials = []
    for i in range(sections[3][0]):
        row = values[i * width:(i + 1) * width]
        materials.append((row[1], row[2], row[5], row[-2]))
    return bsp[0], pages, materials


def select(name, bsp_tag, lightmap_datum, pages, materials, shader_info, bitmap_counts):
    """The files the guest asks for, as (relative path, kind, surfaces covered), in a stable order.

    shader_info: shader datum -> (shader type, base map datum or NONE);
    bitmap_counts: bitmap tag ordinal -> its bitmap count. Mirrors
    render_engine.c and render_gx.c: lightmap pages of materials with
    lightmap vertices; base maps of environment shaders, bitmap =
    permutation % count."""
    files = {lightmap_path(name, bsp_tag): ["geometry", 0]}
    lightmap_tag = lightmap_datum & 0xFFFF if lightmap_datum != NONE else None
    for page, first, count in pages:
        for index in range(first, first + count):
            shader, permutation, surfaces, lightmap_vertices = materials[index]
            if lightmap_tag is not None and page >= 0 and lightmap_vertices:
                entry = files.setdefault(texture_path(name, lightmap_tag, page), ["lightmap", 0])
                entry[1] += surfaces
            shader_type, base = shader_info[shader]
            if shader_type != SHADER_ENVIRONMENT or base == NONE:
                continue
            tag = base & 0xFFFF
            if bitmap_counts.get(tag, 0) <= 0:
                raise StageError("base map without bitmaps")
            entry = files.setdefault(texture_path(name, tag, permutation % bitmap_counts[tag]), ["base", 0])
            entry[1] += surfaces
    order = {"geometry": 0, "lightmap": 1, "base": 2}
    return sorted(((path, kind, surfaces) for path, (kind, surfaces) in files.items()),
                  key=lambda item: (order[item[1]], -item[2], item[0]))


def shader_facts(cache, datum):
    instance = cache.instances[datum & 0xFFFF]
    if instance["datum"] != datum:
        raise StageError("material shader datum does not name its tag")
    root = cache.root_offset(instance, SHADER_ROOT_BYTES)
    if root is None:
        raise StageError("shader root outside tag data")
    shader_type = cache.s16(root + SHADER_TYPE_OFFSET)
    base = cache.u32(root + SHADER_BASE_MAP_INDEX) if shader_type == SHADER_ENVIRONMENT else NONE
    return shader_type, base


def bitmap_count(cache, ordinal):
    instance = cache.instances[ordinal]
    if instance["group"] != cg.GROUP["bitm"]:
        raise StageError("base map reference is not a bitmap tag")
    root = cache.root_offset(instance, BITMAP_ROOT_BYTES)
    if root is None:
        raise StageError("bitmap root outside tag data")
    return cache.s32(root + BITMAP_GROUP_BITMAPS)


def manifest_index(generation):
    manifest = json.loads((generation / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("format") != "halo-wii-content-manifest":
        raise StageError("not a content pipeline manifest")
    return {output["path"]: output for output in manifest["outputs"]}


def source_path(relative):
    """render/<name>/lightmaps/x.hwl -> lightmaps/<name>/x.hwl (the generation's layout)"""
    parts = relative.split("/")
    return f"{parts[2]}/{parts[1]}/{parts[3]}"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage(generation, map_path, name, out):
    outputs = manifest_index(generation)
    cache = hc.open_map(map_path)
    selected = []
    for ordinal, _space, _root in cg.scenario_bsps(cache):
        relative = lightmap_path(name, ordinal)
        entry = outputs.get(source_path(relative))
        if entry is None:
            raise StageError("BSP geometry missing from the generation")
        blob = (generation / entry["path"]).read_bytes()
        lightmap_datum, pages, materials = hwl_records(blob)
        shaders = {datum: shader_facts(cache, datum) for datum, _, _, _ in materials}
        counts = {base & 0xFFFF: bitmap_count(cache, base & 0xFFFF)
                  for shader_type, base in shaders.values() if shader_type == SHADER_ENVIRONMENT and base != NONE}
        selected += select(name, ordinal, lightmap_datum, pages, materials, shaders, counts)
    out.mkdir(parents=True, exist_ok=False)
    files = []
    for relative, kind, surfaces in selected:
        entry = outputs.get(source_path(relative))
        if entry is None:
            raise StageError(f"{kind} file missing from the generation")
        source = generation / entry["path"]
        if sha256_file(source) != entry["sha256"] or source.stat().st_size != entry["bytes"]:
            raise StageError("generation file differs from its manifest")
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        files.append({"path": relative, "kind": kind, "surfaces": surfaces, "bytes": entry["bytes"],
                      "sha256": entry["sha256"], "gx_format": entry.get("gx", {}).get("format")})
    totals = {}
    for item in files:
        totals[item["kind"]] = totals.get(item["kind"], 0) + item["bytes"]
    record = {"schema": "halo-wii-render-stage-1", "name": name, "generation": generation.name,
              "map_sha256": cache.facts.get("source_sha256"), "files": files, "totals": totals}
    (out / "render-stage.json").write_text(json.dumps(record, indent=1, sort_keys=True) + "\n", encoding="utf-8",
                                           newline="\n")
    return record


def stage_arguments(out, record):
    return [f"{(out / item['path']).as_posix()}={SD_ROOT}/{item['path'][len('render/'):]}" for item in record["files"]]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--generation", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True, help="the owner's map (private)")
    parser.add_argument("--name", required=True)
    parser.add_argument("--out", type=Path, required=True, help="a new private directory")
    parser.add_argument("--stage-args", action="store_true", help="print run_dolphin.py --stage arguments")
    args = parser.parse_args(argv)
    record = stage(args.generation, args.map, args.name, args.out)
    if args.stage_args:
        for argument in stage_arguments(args.out, record):
            print(f"--stage={argument}")
    print(json.dumps({"files": len(record["files"]), "totals": record["totals"]}), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
