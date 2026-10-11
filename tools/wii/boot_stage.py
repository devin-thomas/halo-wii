#!/usr/bin/env python3
"""Stage the boot mode's content for the SD card (HWI-016D).

The boot mode (port/wii/engine/boot_main.c) runs the engine's own main
through the original menus. It reads, below sd:/halo-wii-engine/:

    data/maps/<name>.wmap                       maps staged by map_stage.py
    data/render/<name>/textures/<tag>-<bm>.hwt  the menus' bitmaps (the UI map's)
    data/render/<name>/...                      an environment's files (render_stage.py)
    manifest.txt                                what was staged, for the guest to check
    boot.txt                                    the boot configuration (not written here)

This tool copies them from private inputs into a new private directory laid
out as the card's sd:/halo-wii-engine/ folder:

- every map named, from a directory of map_stage.py outputs (<name>.wmap);
- every converted texture of the UI map, from one content pipeline
  generation, each checked against the generation's manifest;
- for each environment map named, render_stage.py's selection (its lightmap
  geometry and textures);
- manifest.txt: the source image's identity (the generation's source chain),
  the generation, the stager, and each map's bytes and SHA-256, which the
  guest hashes and compares at boot (a mismatch is refused, on screen).

Everything it writes is private (converted game data): never commit or
publish it. boot-stage.json records counts and hashes only.

    python tools/wii/boot_stage.py --generation <generation dir> --stage-dir <map_stage outputs> \\
        --ui ui --environment prisoner=<owner's prisoner.map> --out <new private dir>
"""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_FORMAT = "halo-wii-boot-manifest-1"


class StageError(ValueError):
    """A bounded failure; the message names no file contents."""


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_identity(manifest):
    """the source image's SHA-256 (the generation's source chain)"""
    chain = manifest.get("source_chain") or {}
    image = chain.get("image_sha256")
    if not isinstance(image, str) or len(image) != 64:
        raise StageError("the generation's manifest has no source image identity")
    return image


def stager_identity():
    """map_stage.py's own SHA-256: the stager that made the .wmap files"""
    return sha256_file(ROOT / "tools/wii/map_stage.py")


def ui_textures(manifest, name):
    """the generation's converted textures of a map, as (relative path, entry)"""
    prefix = f"textures/{name}/"
    return sorted(((entry["path"], entry) for entry in manifest["outputs"] if entry["path"].startswith(prefix)),
                  key=lambda item: item[0])


def manifest_text(source, generation, stager, maps, textures):
    """manifest.txt: one fact a line (boot_main.c reads it)"""
    lines = [f"# {MANIFEST_FORMAT}", f"source {source}", f"generation {generation}", f"stager {stager}"]
    for name, size, digest in maps:
        if not name.isidentifier() and not name.replace("_", "").isalnum():
            raise StageError("map names are letters, digits and underscores")
        lines.append(f"map {name} {size} {digest}")
    for name, count, size in textures:
        lines.append(f"textures {name} {count} {size}")
    return "\n".join(lines) + "\n"


def copy_map(stage_dir, out, name):
    source = stage_dir / f"{name}.wmap"
    if not source.is_file():
        raise StageError(f"no staged map {name}.wmap in the stage directory")
    target = out / "data/maps" / f"{name}.wmap"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return name, target.stat().st_size, sha256_file(target)


def stage(generation, stage_dir, ui, names, environments, out):
    """names: maps copied as they are; environments: name -> the owner's map, whose
    environment render files are staged too (and whose staged map is copied)"""
    manifest = json.loads((generation / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("format") != "halo-wii-content-manifest":
        raise StageError("not a content pipeline manifest")
    source = source_identity(manifest)
    out.mkdir(parents=True, exist_ok=False)
    maps = [copy_map(stage_dir, out, name) for name in dict.fromkeys([ui, *names, *environments])]
    copied = total = 0
    for relative, entry in ui_textures(manifest, ui):
        path = generation / relative
        if path.stat().st_size != entry["bytes"] or sha256_file(path) != entry["sha256"]:
            raise StageError("a generation file differs from its manifest")
        target = out / "data/render" / ui / "textures" / Path(relative).name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        copied += 1
        total += entry["bytes"]
    environment_records = {}
    if environments:
        import render_stage  # noqa: E402

        for name, map_path in environments.items():
            scratch = out / f".render-{name}"
            result = render_stage.stage(generation, map_path, name, scratch)
            for item in result["files"]:
                target = out / "data" / item["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(scratch / item["path"]), target)
            shutil.rmtree(scratch)
            environment_records[name] = {"files": len(result["files"]), "totals": result["totals"]}
    (out / "manifest.txt").write_text(manifest_text(source, generation.name, stager_identity(), maps,
                                                    [(ui, copied, total)]), encoding="ascii", newline="\n")
    record = {"schema": "halo-wii-boot-stage-1", "generation": generation.name, "source_image_sha256": source,
              "maps": [{"name": n, "bytes": b, "sha256": d} for n, b, d in maps],
              "ui_textures": {"files": copied, "bytes": total}, "environments": environment_records}
    (out / "boot-stage.json").write_text(json.dumps(record, indent=1, sort_keys=True) + "\n", encoding="utf-8",
                                         newline="\n")
    return record


def parse_environment(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("--environment needs NAME=<owner's .map>")
    name, path = value.split("=", 1)
    return name, Path(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--generation", type=Path, required=True)
    parser.add_argument("--stage-dir", type=Path, required=True, help="map_stage.py outputs (<name>.wmap)")
    parser.add_argument("--ui", default="ui", help="the UI map's name")
    parser.add_argument("--map", action="append", default=[], help="another staged map to copy (no textures)")
    parser.add_argument("--environment", type=parse_environment, action="append", default=[],
                        help="NAME=<owner's .map>: a map whose environment render files are staged too")
    parser.add_argument("--out", type=Path, required=True, help="a new private directory")
    args = parser.parse_args(argv)
    try:
        record = stage(args.generation, args.stage_dir, args.ui, args.map, dict(args.environment), args.out)
    except StageError as error:
        print(f"boot_stage: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"maps": len(record["maps"]), "ui_textures": record["ui_textures"]}), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
