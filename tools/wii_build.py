"""Asset-free devkitPPC/libogc probe rules; no desktop generator dependencies."""

import hashlib
import json
import os
from pathlib import Path
import subprocess

from .wii.build import CHECKOUT_PREFIX, DEVKITPRO_PREFIX, ENGINE_SEMANTIC_FLAGS
from .wii.check_toolchain import inspect_toolchain, toolchain_inputs
from .wii.engine_build import engine_flags, engine_inputs, generate_engine_build

BUILD = Path("build/wii")
SOURCES = [Path("port/wii/probe/main.c"), Path("port/wii/abi/boundary.c"), Path("port/wii/abi/fixture.c")]
GX_SCENE_SOURCES = [Path("port/wii/gx_scene/main.c")]
GX_MATERIALS_SOURCES = [Path("port/wii/gx_materials/main.c")]
# HWI-008C/HWI-008E content loader self-test: the HWT1/HRA1/HWS1 and HWM1/HWL1/HWC1/HMA1/HWF1/HUS1
# runtime loaders and their DOL.
CONTENT_LOADER_SOURCES = [Path("port/wii/content") / name for name in (
    "main.c", "content_check.c", "content_common.c", "content_sections.c", "hwt_texture.c", "hra_animation.c",
    "hws_sound.c", "hwm_model.c", "hwl_lightmap.c", "hwc_collision.c", "hma_graph.c", "hwf_font.c",
    "hus_strings.c")]
CONTENT_LOADER_HEADERS = [Path("port/wii/content") / name for name in (
    "content_check.h", "content_common.h", "content_sections.h", "hwt_texture.h", "hra_animation.h",
    "hws_sound.h", "hwm_model.h", "hwl_lightmap.h", "hwc_collision.h", "hma_graph.h", "hwf_font.h",
    "hus_strings.h")]
GEOMETRY_VIEW_SOURCES = [Path("port/wii/geometry_view/main.c"),
                         *(Path("tools/wii") / name for name in (
                             "cache_arena_plan.c", "cache_stream_io.c", "cache_address_owned.c",
                             "cache_address_probe.c", "cache_bsp_probe.c", "cache_material_probe.c"))]
# HWI-007 memory strategy diagnostic: authored units, and the actual engine units
# source/memory/data.c and memory_pool.c built with an authored service shim.
MEMORY_STRATEGY_SOURCES = [Path("port/wii/memory_strategy/main.c"),
                           *(Path("tools/wii") / name for name in (
                               "cache_arena_plan.c", "cache_stream_io.c", "cache_address_owned.c",
                               "cache_address_probe.c", "cache_bsp_probe.c", "cache_schema_graph.c",
                               "cache_schema_tables.c", "game_state_image.c", "game_state_census_table.c"))]
MEMORY_STRATEGY_ENGINE_SOURCES = [Path("source/memory/data.c"), Path("source/memory/memory_pool.c")]
# HWI-008D movie and audio decoder benchmark: authored units plus the pinned
# vendored decoders (port/wii/third_party, compiled inside media_decoders.c).
MEDIA_BENCH_SOURCES = [Path("port/wii/media_bench") / name for name in ("main.c", "media_core.c", "media_decoders.c")]
MEDIA_BENCH_HEADERS = [Path("port/wii/media_bench/media_core.h"), Path("port/wii/third_party/pl_mpeg/pl_mpeg.h"),
                       Path("port/wii/third_party/pl_mpeg/pl_mpeg_wii.h"),
                       Path("port/wii/third_party/stb/stb_image.h")]
MEMORY_STRATEGY_HEADERS = [*(Path("tools/wii") / name for name in (
                               "cache_schema_graph.h", "cache_schema_tables.h", "game_state_image.h")),
                           *sorted(Path("port/wii/memory_strategy/engine_shim").glob("*.h")),
                           Path("source/memory/data.h"), Path("source/memory/memory_pool.h")]
MEMORY_STRATEGY_INCLUDES = ["-Iport/wii/memory_strategy/engine_shim", "-Isource/memory"]
# ADR-018: engine translation units use signed plain char; the engine code is verbatim, so its
# warnings are not errors here.
MEMORY_STRATEGY_ENGINE_FLAGS = ["-fsigned-char", "-w", *MEMORY_STRATEGY_INCLUDES]
MACHINE_FLAGS = ["-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float"]
CFLAGS = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", *MACHINE_FLAGS]
# ADR-018: engine translation units (HWI-015 onwards) compile with the
# shared engine semantics (tools/wii/build.py ENGINE_SEMANTIC_FLAGS) on top of
# CFLAGS, and link only objects that pass engine_wide_references(). The
# current targets are authored diagnostics, not engine units, and keep CFLAGS.
ENGINE_CFLAGS = [*CFLAGS, *ENGINE_SEMANTIC_FLAGS]
LIBRARIES = ("libfat.a", "libwiiuse.a", "libbte.a", "libogc.a", "libasnd.a")
# Archives linked as inputs (before -logc) by the targets that use them.
TARGET_ARCHIVES = {"wii_media_bench": ("libasnd.a",)}


def source_inputs():
    return [Path("tools/wii_build.py"), Path("tools/wii/build.py"),
            Path("tools/wii/check_toolchain.py"), *SOURCES, *GX_SCENE_SOURCES, *GX_MATERIALS_SOURCES,
            *GEOMETRY_VIEW_SOURCES, *CONTENT_LOADER_SOURCES, *CONTENT_LOADER_HEADERS,
            *MEMORY_STRATEGY_SOURCES, *MEMORY_STRATEGY_ENGINE_SOURCES,
            *MEMORY_STRATEGY_HEADERS, *MEDIA_BENCH_SOURCES, *MEDIA_BENCH_HEADERS,
            *sorted(Path("port/wii/abi").glob("*.h")),
            *(Path("tools/wii") / name for name in (
                "cache_arena_plan.h", "cache_stream_io.h", "cache_address_owned.h",
                "cache_address_probe.h", "cache_bsp_probe.h", "cache_material_probe.h")),
            Path("port/linux/include/halo_port_capacity.h"), *engine_inputs()]


def wii_configure_inputs(devkitpro=None):
    paths = source_inputs()
    if devkitpro is not None:
        paths.extend(toolchain_inputs(Path(devkitpro).resolve()))
        for name in ("HEAD", "index", "packed-refs"):
            path = Path(subprocess.check_output(["git", "rev-parse", "--git-path", name], text=True).strip())
            if path.exists():
                paths.append(path)
        branch = subprocess.run(["git", "symbolic-ref", "-q", "HEAD"], text=True, capture_output=True)
        if branch.returncode == 0:
            path = Path(subprocess.check_output(["git", "rev-parse", "--git-path", branch.stdout.strip()], text=True).strip())
            if path.exists():
                paths.append(path)
        elif branch.returncode != 1:
            raise ValueError(branch.stderr.strip())
    return paths


def write_if_changed(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def source_identity():
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--", "configure.py", "tools/wii_build.py", "tools/wii", "port/wii"], text=True))
    return commit, dirty


def generate_wii_build(n, sln):
    value = sln.wii_devkitpro or os.environ.get("DEVKITPRO")
    if not value:
        raise ValueError("Wii needs --wii-devkitpro <native path> or DEVKITPRO from the official shell")
    root = Path(value).resolve()
    if not 0 <= sln.wii_probe_frames <= 36000:
        raise ValueError("--wii-probe-frames must be between 0 and 36000")
    diagnostic_storage = getattr(sln, "wii_diagnostic_storage", "omit")
    inventory = inspect_toolchain(root)
    if inventory["errors"]:
        raise ValueError("Wii toolchain preflight failed:\n" + "\n".join(inventory["errors"]))
    libraries = [root / "libogc/lib/wii" / name for name in LIBRARIES]
    for library in libraries:
        if not library.is_file():
            raise ValueError(f"Missing official Wii development library: {library}")
    commit, dirty = source_identity()
    inputs = [Path("configure.py"), *source_inputs()]
    hashes = {path.as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    sdk = hashlib.sha256()
    for path in toolchain_inputs(root):
        sdk.update(path.relative_to(root).as_posix().encode() + b"\0")
        if path.is_file():
            sdk.update(hashlib.sha256(path.read_bytes()).digest())
    public = {
        "schema_version": 1, "scope": "asset_free_probe", "source_commit": commit,
        "source_dirty": dirty, "inputs_sha256": hashes,
        "compiler": inventory["tools"]["powerpc-eabi-gcc"]["version"],
        "compiler_target": inventory["tools"]["powerpc-eabi-gcc"]["target"],
        "binutils": inventory["tools"]["powerpc-eabi-ld"]["version"],
        "compile_flags": CFLAGS, "probe_auto_exit_frames": sln.wii_probe_frames,
        "memory_strategy_flags": {"authored": MEMORY_STRATEGY_INCLUDES, "engine": MEMORY_STRATEGY_ENGINE_FLAGS},
        # HWI-015: wii_engine compiles engine units with ENGINE_CFLAGS plus these
        "engine_flags": engine_flags(ENGINE_SEMANTIC_FLAGS, diagnostic_storage),
        # HWI-015D: the AI's debug records and the profiler's frame history
        # are left out (omit, the default) or compiled (keep)
        "engine_diagnostic_storage": diagnostic_storage,
        # build.py maps these build-machine roots in debug info and link maps.
        "path_prefix_map": {"checkout": CHECKOUT_PREFIX, "devkitpro": DEVKITPRO_PREFIX},
        "wii_rules_sha256": hashlib.sha256((root / "devkitPPC/wii_rules").read_bytes()).hexdigest(),
        "libogc_sha256": hashlib.sha256((root / "libogc/lib/wii/libogc.a").read_bytes()).hexdigest(),
        "libraries_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in libraries},
        "runtime_verified": False,
        "toolchain_sha256": sdk.hexdigest(),
    }
    public["build_id"] = hashlib.sha256(json.dumps(public, sort_keys=True).encode()).hexdigest()[:16]
    local = {"devkitpro": root.as_posix(), "tools": inventory["tools"], "public": public}
    write_if_changed(BUILD / "local-config.json", json.dumps(local, indent=2) + "\n")
    write_if_changed(BUILD / "build_id.h", f'#define WII_BUILD_ID "{public["build_id"]}"\n#define WII_PROBE_AUTO_EXIT_FRAMES {sln.wii_probe_frames}U\n')

    n.comment("Native Wii probe: flags derived from the installed official wii_rules")
    runner = "$python tools/wii/build.py"
    config = "--config build/wii/local-config.json"
    n.rule("wii_cc", f'{runner} compile {config} --source "$in" --output "$out"',
           description="PPC CC $in", depfile="$out.d", deps="gcc")
    n.rule("wii_cc_flags", f'{runner} compile {config} $extra --source "$in" --output "$out"',
           description="PPC CC $in", depfile="$out.d", deps="gcc")
    n.rule("wii_link", f'{runner} link {config} --output "$out" $in', description="PPC LINK $out")
    n.rule("wii_dol", f'{runner} convert {config} --source "$in" --output "$out"', description="ELF2DOL $out")
    n.rule("wii_manifest", f'{runner} manifest {config} --stem $stem --scope $scope --output "$out"',
           description="MANIFEST Wii $stem", restat=True)
    # The probe keeps its original object/artifact paths; the other targets
    # build their objects in subdirectories because every entry point is main.c.
    targets = (("wii_probe", "probe", "asset_free_probe", SOURCES, BUILD, BUILD / "build-info.json"),
               ("wii_gx_scene", "gx_scene", "asset_free_gx_scene", GX_SCENE_SOURCES, BUILD / "gx_scene",
                BUILD / "gx_scene-build-info.json"),
               ("wii_gx_materials", "gx_materials", "asset_free_gx_materials", GX_MATERIALS_SOURCES,
                BUILD / "gx_materials", BUILD / "gx_materials-build-info.json"),
               ("wii_geometry_view", "geometry_view", "owned_geometry_diagnostic_no_embedded_assets",
                GEOMETRY_VIEW_SOURCES, BUILD / "geometry_view", BUILD / "geometry_view-build-info.json"),
               ("wii_content_loaders", "content_loaders", "owned_content_loader_diagnostic_no_embedded_assets",
                CONTENT_LOADER_SOURCES, BUILD / "content_loaders", BUILD / "content_loaders-build-info.json"),
               ("wii_memory_strategy", "memory_strategy", "memory_strategy_diagnostic_no_embedded_assets",
                [*MEMORY_STRATEGY_SOURCES, *MEMORY_STRATEGY_ENGINE_SOURCES], BUILD / "memory_strategy",
                BUILD / "memory_strategy-build-info.json"),
               ("wii_media_bench", "media_bench", "media_decoder_benchmark_no_embedded_assets", MEDIA_BENCH_SOURCES,
                BUILD / "media_bench", BUILD / "media_bench-build-info.json"))
    for target, stem, scope, sources, object_dir, manifest in targets:
        objects = []
        for source in sources:
            obj = object_dir / (source.stem + ".o")
            implicit = [BUILD / "local-config.json", BUILD / "build_id.h"]
            if target == "wii_memory_strategy":
                engine = source in MEMORY_STRATEGY_ENGINE_SOURCES
                flags = MEMORY_STRATEGY_ENGINE_FLAGS if engine else MEMORY_STRATEGY_INCLUDES
                n.build(obj, "wii_cc_flags", source, implicit=implicit,
                        variables={"extra": " ".join(f"--extra-flag={flag}" for flag in flags)})
            else:
                n.build(obj, "wii_cc", source, implicit=implicit)
            objects.append(obj)
        elf, dol, link_map = BUILD / f"{stem}.elf", BUILD / f"{stem}.dol", BUILD / f"{stem}.map"
        archives = [root / "libogc/lib/wii" / name for name in TARGET_ARCHIVES.get(target, ())]
        n.build(elf, "wii_link", [*objects, *archives], implicit=[BUILD / "local-config.json", *libraries],
                implicit_outputs=link_map)
        n.build(dol, "wii_dol", elf, implicit=BUILD / "local-config.json")
        n.build(manifest, "wii_manifest", implicit=[elf, dol, link_map, BUILD / "local-config.json"],
                variables={"stem": stem, "scope": scope})
        n.build(target, "phony", [dol, manifest])
    n.newline()
    generate_engine_build(n, ENGINE_SEMANTIC_FLAGS, libraries, diagnostic_storage)
