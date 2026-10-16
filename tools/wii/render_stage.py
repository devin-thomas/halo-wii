#!/usr/bin/env python3
"""Stage a map's environment render resources for the Wii rasterizer (HWI-016B).

The Wii rasterizer's environment path (port/wii/render) reads, per structure
BSP, its converted geometry (HWL1) and the textures (HWT1) of its lightmap
pages and of its environment shaders' base maps, from the content
pipeline's outputs (tools/wii/content_convert.py, one generation):

    sd:/halo-wii-engine/data/render/<name>/lightmaps/<bsp tag:05>.hwl
    sd:/halo-wii-engine/data/render/<name>/textures/<bitmap tag:05>-<bitmap:03>.hwt

HWI-087: and the map's HUD bitmaps, which the boot mode's gameplay HUD draws
from the same directory: every bitmap of each bitmap tag named ui\\hud\\...
(the HUD's meters, panels, digits, reticles and overlays). A HUD bitmap the
generation lacks is counted (hud_missing), not staged; the guest counts what
it cannot draw.

HWI-086: and the base maps of the shader_model shaders the map's models name
(every bitmap of each: an object's forced shader permutation picks any), the
files the models' material reads from the same textures directory.

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
# HWI-086: shader_model (rasterizer_xbox_models.c shader_model_definition,
# render_objects_engine.c asserts the same offsets) and the model tag's
# shaders block (content_geometry.py: geometries at 208, shaders after them)
SHADER_MODEL = 4
SHADER_MODEL_BASE_MAP_INDEX = 0xB0   # shader_model base_map.index
SHADER_MODEL_ROOT_BYTES = 0x174      # through reflection_cube_map.index
SHADER_HEADER_BYTES = 0x28
MODEL_SHADERS = cg.MODEL_GEOMETRIES + 12
MODEL_SHADER_REFERENCE_BYTES = 0x20
MODEL_SHADER_DATUM = 12              # the reference's tag index
MAX_MODEL_SHADERS = 256


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
    order = {"geometry": 0, "lightmap": 1, "base": 2, "model_base": 3}
    return sorted(((path, kind, surfaces) for path, (kind, surfaces) in files.items()),
                  key=lambda item: (order[item[1]], -item[2], item[0]))


HUD_TAG_PREFIX = "ui\\hud\\"


def select_hud(name, bitmap_tags):
    """HWI-087: the HUD's textures, as (relative path, "hud", 0) in tag and bitmap order. bitmap_tags:
    (tag ordinal, tag name, bitmap count) of the map's bitmap tags; those named ui\\hud\\... are the HUD's."""
    files = []
    for ordinal, tag_name, count in sorted(bitmap_tags):
        if tag_name.lower().startswith(HUD_TAG_PREFIX):
            files += [(texture_path(name, ordinal, index), "hud", 0) for index in range(max(count, 0))]
    return files


def hud_bitmap_tags(cache):
    return [(instance["ordinal"], cache.name(instance), bitmap_count(cache, instance["ordinal"]))
            for instance in cache.by_group("bitm")]

def select_models(name, references, shader_info, bitmap_counts):
    """HWI-086: the model base maps the guest may ask for, as (relative path, "model_base", references).

    references: the shader datum of every model shader reference (repeats
    count); shader_info: datum -> (shader type, base map datum or NONE);
    bitmap_counts: bitmap tag ordinal -> its bitmap count. Mirrors
    render_objects_engine.c: a shader_model with a base map of bitmaps draws
    bitmap permutation % count, and the permutation is the reference's or the
    object's forced one, so every bitmap of the group is staged. A base map of
    NONE or no bitmaps binds the Xbox's default texture: nothing to stage (the
    guest counts it)."""
    files = {}
    for datum in references:
        shader_type, base = shader_info[datum]
        if shader_type != SHADER_MODEL or base == NONE:
            continue
        tag = base & 0xFFFF
        for bitmap in range(max(bitmap_counts.get(tag, 0), 0)):
            entry = files.setdefault(texture_path(name, tag, bitmap), ["model_base", 0])
            entry[1] += 1
    return sorted(((path, kind, count) for path, (kind, count) in files.items()), key=lambda item: (-item[2], item[0]))


def model_shader_references(cache):
    """the shader datum of every shader reference of every model tag, in tag order"""
    references = []
    for instance in cache.by_group("mode"):
        root = cache.root_offset(instance, cg.MODEL_ROOT_BYTES)
        if root is None:
            raise StageError("model root outside tag data")
        try:
            blocks = cache.block(root + MODEL_SHADERS, MODEL_SHADER_REFERENCE_BYTES, MAX_MODEL_SHADERS)
        except hc.CacheError:
            raise StageError("model shaders block outside bounds") from None
        for reference in blocks:
            datum = cache.u32(reference + MODEL_SHADER_DATUM)
            if datum != NONE:
                references.append(datum)
    return references


def model_shader_facts(cache, datum):
    """(shader type, base map datum or NONE) of a shader a model names"""
    instance = cache.instances[datum & 0xFFFF]
    if instance["datum"] != datum:
        raise StageError("model shader datum does not name its tag")
    root = cache.root_offset(instance, SHADER_HEADER_BYTES)
    if root is None:
        raise StageError("shader root outside tag data")
    shader_type = cache.s16(root + SHADER_TYPE_OFFSET)
    if shader_type != SHADER_MODEL:
        return shader_type, NONE
    if cache.root_offset(instance, SHADER_MODEL_ROOT_BYTES) is None:
        raise StageError("shader root outside tag data")
    return shader_type, cache.u32(root + SHADER_MODEL_BASE_MAP_INDEX)


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


def stage(generation, map_path, name, out, hud=True):
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
    hud_missing = 0
    if hud:
        chosen = {relative for relative, _, _ in selected}
        for relative, kind, surfaces in select_hud(name, hud_bitmap_tags(cache)):
            if relative in chosen:
                continue
            if outputs.get(source_path(relative)) is None:
                hud_missing += 1
                continue
            selected.append((relative, kind, surfaces))

    # HWI-086: the models' base maps (a file the environment stages already serves both)
    references = model_shader_references(cache)
    model_shaders = {datum: model_shader_facts(cache, datum) for datum in sorted(set(references))}
    model_counts = {base & 0xFFFF: bitmap_count(cache, base & 0xFFFF)
                    for shader_type, base in model_shaders.values() if shader_type == SHADER_MODEL and base != NONE}
    seen = {path for path, _, _ in selected}
    selected += [item for item in select_models(name, references, model_shaders, model_counts) if item[0] not in seen]
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
              "map_sha256": cache.facts.get("source_sha256"), "files": files, "totals": totals,
              "hud_missing": hud_missing}
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
    parser.add_argument("--no-hud", action="store_true", help="HWI-087: leave out the map's HUD bitmaps")
    args = parser.parse_args(argv)
    record = stage(args.generation, args.map, args.name, args.out, hud=not args.no_hud)
    if args.stage_args:
        for argument in stage_arguments(args.out, record):
            print(f"--stage={argument}")
    print(json.dumps({"files": len(record["files"]), "totals": record["totals"]}), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
