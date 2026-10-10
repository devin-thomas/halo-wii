"""Ninja rules for wii_engine (HWI-015): the Halo engine on Wii platform services.

The target compiles every game unit the native ports compile (port/linux/port.json
"game", plus port/linux/game) for big-endian PowerPC with the ADR-018 engine
semantics, links it against explicit Wii implementations of the platform
services the engine calls (port/wii/engine), and converts it to a DOL.

- Engine units: tools/wii_build.py ENGINE_CFLAGS, plus the MSVC dialect the
  source is written in (GNU89, tentative definitions, MS extensions), the
  Wii prefix header port/wii/engine/include/halo_wii_prefix.h and the
  generated semantics header (tools/wii/engine_semantics.py).
  - source/cache/physical_memory_map.c is replaced by
    port/wii/engine/physical_memory_map_wii.c: the upstream unit places the
    game state and tag cache at fixed Xbox addresses, which ADR-015/016
    reject on Wii; the replacement takes them from the run-time MEM2 arena
    plan.
  - source/memory/byte_swapping.c is compiled from a copy with its MSVC
    integer suffixes respelled (tools/wii/engine_rewrite.py).
- Platform units: the Linux platform layer's files that only need a C
  library are reused unchanged (REUSED_PLATFORM); the services that differ
  on the Wii (kernel objects, threads, time, logging, memory, file system
  metadata) are Wii implementations (WII_PLATFORM); every other SDK or port
  entry point the engine references reports itself as unsupported
  (port/wii/engine/unsupported_*.c).
- Gates before the link (tools/wii/build.py): no engine object references a
  newlib wide-character function (ADR-018), and no object uses thread-local
  storage (libogc has no thread pointer).
- Diagnostic storage (HWI-015D, configure.py --wii-diagnostic-storage): by
  default the engine units compile without the AI's debug records
  (HALO_AI_DEBUG_RECORDS 0, source/ai/ai_debug.h) and the profiler's frame
  history (HALO_PROFILE_FRAME_HISTORY 0, source/cseries/profile.c), which the
  audit (docs/wii/evidence/2026-10-10-diagnostics-storage-audit.md) shows
  nothing in the game reads; ai_debug_initialize then runs as upstream's.
  "keep" compiles them as the other ports do, and ai_debug_initialize, whose
  records then need 30.36 MB of heap, is reported unsupported.
"""

import json
from pathlib import Path
from typing import Dict, List, Tuple

ENGINE_DIR = Path("port/wii/engine")
BUILD = Path("build/wii")
ENGINE_BUILD = BUILD / "engine"
PORT_CONFIG = Path("port/linux/port.json")
LINUX_GAME = Path("port/linux/game")
LINUX_PLATFORM = Path("port/linux/src")
XDK_INCLUDE = Path("port/include/xdk")
PREFIX = ENGINE_DIR / "include/halo_wii_prefix.h"
SEMANTICS = ENGINE_BUILD / "halo_wii_semantics.h"
PLATFORM_SEMANTICS = ENGINE_BUILD / "halo_wii_platform_semantics.h"

# engine units with a Wii replacement, and units compiled from a rewritten copy
REPLACED = {Path("source/cache/physical_memory_map.c"): ENGINE_DIR / "physical_memory_map_wii.c",
            # HWI-015B: maps staged on the SD card in place of the Xbox's hard-disk cache
            Path("source/cache/cache_files_windows.c"): ENGINE_DIR / "cache_files_wii.c"}
REWRITTEN = (Path("source/memory/byte_swapping.c"),)
# Authored units compiled as engine units (the engine's headers and flags):
# the driver's engine-side half and the controlled fixed-step scenario.
ENGINE_AUTHORED = (ENGINE_DIR / "engine_hooks.c", ENGINE_DIR / "fixed_step_scenario.c",
                   ENGINE_DIR / "real_map_scenario.c")

# Linux platform files whose code only needs a C library: reused unchanged.
REUSED_PLATFORM = tuple(LINUX_PLATFORM / name for name in (
    "xbox_files.c", "xbox_xapi.c", "msvc_crt.c", "msvc_wide.c", "halo_linker_common.c", "port_config.c"))
# The Wii's own services.
WII_PLATFORM = tuple(ENGINE_DIR / name for name in (
    "wii_log.c", "wii_kernel.c", "wii_memory.c", "wii_posix_files.c", "wii_sdl_files.c",
    "wii_platform_checks.c", "unsupported_sdk.c", "unsupported_port.c", "engine_map_run.c",
    "wii_engine_main.c")) + (
    Path("port/wii/runtime/runtime_start.c"), Path("tools/wii/cache_arena_plan.c"))
WII_HEADERS = tuple(sorted(ENGINE_DIR.glob("*.h"))) + (PREFIX, ENGINE_DIR / "sdl/SDL3/SDL.h")

MUSL_MATH = Path("port/third_party/musl-math")
ZLIB = Path("port/third_party/zlib")
ZLIB_SOURCES = ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c")
ZLIB_DEFINES = ["-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg"]
TOML = Path("port/third_party/tomlc17")

# Every unit: a section per function and object, so the link drops what
# nothing references; and no unwind tables (.eh_frame), which C code without
# exceptions never reads (the engine's stack walker is the Windows one,
# unsupported here) and which would take 0.49 MB of MEM1.
SECTION_FLAGS = ["-ffunction-sections", "-fdata-sections", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables"]
# The dialect the game is written in (tools/linux_build.py GAME_FLAGS and
# LINUX_ABI_FLAGS): C89 with MSVC's tentative definitions and anonymous
# members, and no warnings for verbatim upstream code. Sections per function
# and object let the link drop what nothing references.
ENGINE_DIALECT_FLAGS = ["-std=gnu89", "-w", "-fcommon", "-fms-extensions", "-D__STRICT_ANSI__",
                        *SECTION_FLAGS]
PLATFORM_DIALECT_FLAGS = ["-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-fms-extensions",
                          *SECTION_FLAGS]
# Wrapped engine entry points (port/wii/engine/wii_engine_main.c):
# halt_and_catch_fire draws its report with the rasterizer forever, which the
# Wii build does not have; game_tick/game_frame run the controlled
# fixed-step scenario while no map is loaded, else the engine's own.
WRAPPED = ("main", "halt_and_catch_fire", "game_tick", "game_frame", "update_client_get_maximum_possible_server_time",
           "update_client_local_ticks", "rasterizer_decals_initialize", "rasterizer_decals_dispose",
           # HWI-015B: a real map's scripted input, and the game state's
           # allocations, recorded for its digests (real_map_scenario.c)
           "update_client_handle_server_update", "game_state_malloc", "game_state_gpu_malloc",
           "game_state_data_new", "game_state_memory_pool_new", "game_state_lruv_cache_new",
           # HWI-015B: the texture cache rasterizer_initialize would have made
           "texture_cache_open", "texture_cache_close", "sound_cache_open", "sound_cache_close",
           "predicted_resources_precache", "_rasterizer_decals_dispose_from_old_map")
# Diagnostic storage (HWI-015D): "omit" (the default) compiles the engine
# without the AI's debug records and the profiler's frame history; "keep"
# compiles them as the other ports do, and then wraps ai_debug_initialize
# (port/wii/engine/engine_hooks.c), whose records do not fit.
DIAGNOSTIC_STORAGE = ("omit", "keep")
DIAGNOSTIC_STORAGE_OMITTED = ["-DHALO_AI_DEBUG_RECORDS=0", "-DHALO_PROFILE_FRAME_HISTORY=0"]


def wrapped(diagnostic_storage: str = "omit") -> Tuple[str, ...]:
    return WRAPPED + (("ai_debug_initialize",) if diagnostic_storage == "keep" else ())


def link_flags(diagnostic_storage: str = "omit") -> List[str]:
    return ["-Wl,--gc-sections", *(f"-Wl,--wrap={name}" for name in wrapped(diagnostic_storage))]


LINK_FLAGS = link_flags()


def _port_config() -> dict:
    return json.loads(PORT_CONFIG.read_text(encoding="utf-8"))


def game_units() -> List[Path]:
    """the engine units of the native ports, before Wii replacements"""
    game = _port_config()["game"]
    excluded = set(game.get("exclude", []))
    units = sorted(path for path in Path(game["root"]).rglob("*.c") if path.as_posix() not in excluded)
    return units + sorted(LINUX_GAME.glob("*.c"))


def engine_units() -> List[Tuple[Path, Path]]:
    """(original unit, file compiled) for every engine unit of the Wii build"""
    result = []
    for unit in game_units():
        if unit in REPLACED:
            result.append((unit, REPLACED[unit]))
        elif unit in REWRITTEN:
            result.append((unit, rewritten_path(unit)))
        else:
            result.append((unit, unit))
    return result + [(unit, unit) for unit in ENGINE_AUTHORED]


def rewritten_path(unit: Path) -> Path:
    return ENGINE_BUILD / "rewritten" / unit


def object_path(unit: Path) -> Path:
    """objects named after the original unit, without spaces (ninja and the
    linker response file stay simple)"""
    return ENGINE_BUILD / "obj" / Path(unit.as_posix().replace(" ", "_")).with_suffix(".o")


def engine_flags(semantic_flags: List[str], diagnostic_storage: str = "omit") -> List[str]:
    if diagnostic_storage not in DIAGNOSTIC_STORAGE:
        raise ValueError(f"diagnostic storage must be one of {DIAGNOSTIC_STORAGE}")
    game = _port_config()["game"]
    omitted = DIAGNOSTIC_STORAGE_OMITTED if diagnostic_storage == "omit" else []
    return [*semantic_flags, *ENGINE_DIALECT_FLAGS, *omitted,
            "-include", PREFIX.as_posix(), "-include", SEMANTICS.as_posix(),
            "-Iport/linux/include", "-iquote", LINUX_GAME.as_posix(),
            *(f"-D{define}" for define in game.get("defines", [])),
            *(f"-I{directory}" for directory in game.get("include_dirs", [])),
            "-idirafter", XDK_INCLUDE.as_posix()]


def platform_flags(semantic_flags: List[str]) -> List[str]:
    return [*semantic_flags, *PLATFORM_DIALECT_FLAGS,
            "-include", PREFIX.as_posix(), "-include", PLATFORM_SEMANTICS.as_posix(),
            f"-I{ENGINE_DIR.as_posix()}", f"-I{(ENGINE_DIR / 'sdl').as_posix()}", f"-I{LINUX_PLATFORM.as_posix()}",
            "-Iport/linux/include", f"-I{TOML.as_posix()}", f"-I{ZLIB.as_posix()}", "-Iport/include",
            "-Isource", "-Isource/cseries", "-idirafter", XDK_INCLUDE.as_posix()]


def flag_sets(semantic_flags: List[str], diagnostic_storage: str = "omit") -> Dict[str, List[str]]:
    platform = platform_flags(semantic_flags)
    return {
        "engine": engine_flags(semantic_flags, diagnostic_storage),
        # the Linux layer is written for -Wall without -Werror
        "platform_reused": [*platform, "-w"],
        # msvc_wide.c keeps wcstok's position in a __thread variable; libogc
        # has no thread-local storage, so it is one variable (the engine
        # tokenizes on its main thread only)
        "platform_reused_wide": [*platform, "-w", "-D__thread="],
        "platform_wii": [*platform, "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                         "-Wno-sign-compare", "-Wno-unknown-pragmas", "-Wno-missing-braces",
                         "-Wno-unused-function", "-Wno-pointer-sign", "-Wno-implicit-fallthrough"],
        # posix.h's file helpers: newlib's own headers, without the MSVC
        # headers of port/linux/include (as port/linux/src/posix_*.c are built)
        "platform_posix": [*semantic_flags, "-std=gnu11", "-D_GNU_SOURCE", f"-I{LINUX_PLATFORM.as_posix()}",
                           *SECTION_FLAGS],
        # the driver and the arena planner: libogc's headers, not the SDK's
        # (the target's CFLAGS, and POSIX names)
        "driver": [*SECTION_FLAGS, "-D_GNU_SOURCE", f"-I{ENGINE_DIR.as_posix()}"],
        "musl_math": [*semantic_flags, "-std=gnu11", "-w", f"-I{MUSL_MATH.as_posix()}/include",
                      "-include", f"{MUSL_MATH.as_posix()}/include/libm.h", *SECTION_FLAGS],
        "zlib": [*semantic_flags, "-std=gnu11", "-w", *ZLIB_DEFINES, *SECTION_FLAGS],
        "toml": [*semantic_flags, "-std=gnu11", "-w", *SECTION_FLAGS],
    }


def platform_units() -> List[Tuple[Path, str]]:
    units = [(path, "platform_reused_wide" if path.name == "msvc_wide.c" else "platform_reused")
             for path in REUSED_PLATFORM]
    driver = ("wii_engine_main.c", "runtime_start.c", "cache_arena_plan.c", "engine_map_run.c")
    units += [(path, "platform_posix" if path.name == "wii_posix_files.c" else
               "driver" if path.name in driver else "platform_wii") for path in WII_PLATFORM]
    units += [(path, "musl_math") for path in sorted((MUSL_MATH / "src").glob("*.c"))]
    units += [(ZLIB / name, "zlib") for name in ZLIB_SOURCES]
    units += [(TOML / "tomlc17.c", "toml")]
    return units


def engine_inputs() -> List[Path]:
    """sources whose change rebuilds the configuration (hashed into the build ID)"""
    if not PORT_CONFIG.is_file():
        # not a checkout (a test fixture's directory): nothing of the engine
        return [Path("tools/wii/engine_build.py")]
    # every source and header the engine units can include, so the build ID
    # identifies the engine source too
    headers = sorted(path for root in (Path("source"), LINUX_GAME, Path("port/linux/include"), XDK_INCLUDE,
                                       LINUX_PLATFORM, MUSL_MATH, ZLIB, TOML, Path("port/include"))
                     for path in root.rglob("*") if path.suffix in (".h", ".inc"))
    return [Path("tools/wii/engine_build.py"), Path("tools/wii/engine_semantics.py"),
            Path("tools/wii/engine_rewrite.py"), Path("tools/linux_msvc_semantics.py"), PORT_CONFIG,
            *WII_HEADERS, *game_units(), *ENGINE_AUTHORED, *(path for path, _ in platform_units()),
            *REPLACED.values(), *sorted(set(headers))]


def generate_engine_build(n, semantic_flags: List[str], libraries: List[Path],
                          diagnostic_storage: str = "omit") -> None:
    """the wii_engine target; tools/wii_build.py has written local-config.json
    and build_id.h and defined the wii_cc_flags/wii_link/wii_dol/wii_manifest rules"""
    flag_files = {}
    for name, flags in flag_sets(semantic_flags, diagnostic_storage).items():
        path = ENGINE_BUILD / f"flags-{name}.json"
        text = json.dumps(flags, indent=1) + "\n"
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8", newline="\n")
        flag_files[name] = path

    n.comment("Wii engine (HWI-015): the game on Wii platform services")
    n.rule("wii_engine_semantics",
           "$python tools/wii/engine_semantics.py --output $out $scan",
           description="WII SEMANTICS $out", restat=True)
    n.rule("wii_engine_rewrite", '$python tools/wii/engine_rewrite.py --source "$in" --output "$out"',
           description="WII REWRITE $in")
    n.rule("wii_cc_file", '$python tools/wii/build.py compile --config build/wii/local-config.json '
                          '--extra-flags-file $flags --source $in --output $out',
           description="PPC CC $in", depfile="$out.d", deps="gcc")
    n.rule("wii_engine_link", "$python tools/wii/build.py link --config build/wii/local-config.json "
                              "--output $out --objects-file $objects --engine-objects-file $engine_objects "
                              "$link_flags",
           description="PPC LINK $out")

    scan_inputs = [Path("tools/wii/engine_semantics.py"), Path("tools/linux_msvc_semantics.py")]
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    xdk = sorted(XDK_INCLUDE.glob("*.h"))
    linux_game = sorted(p for p in LINUX_GAME.glob("*") if p.suffix in (".c", ".h", ".inc"))
    n.build(SEMANTICS, "wii_engine_semantics", implicit=[*scan_inputs, *game_headers, *xdk, *linux_game],
            variables={"scan": "--tags source --inlines source --inlines port/include/xdk --static-scan source "
                                "--static-scan port/include/xdk --static-scan port/linux/game"})
    n.build(PLATFORM_SEMANTICS, "wii_engine_semantics", implicit=[*scan_inputs, *xdk],
            variables={"scan": "--inlines port/include/xdk"})
    for unit in REWRITTEN:
        n.build(rewritten_path(unit), "wii_engine_rewrite", unit, implicit=[Path("tools/wii/engine_rewrite.py")])

    common = [BUILD / "local-config.json", BUILD / "build_id.h", PREFIX]
    engine_objects, objects = [], []
    for unit, compiled in engine_units():
        obj = object_path(unit)
        n.build(obj, "wii_cc_file", compiled, implicit=[*common, SEMANTICS, flag_files["engine"]],
                variables={"flags": flag_files["engine"].as_posix()})
        engine_objects.append(obj)
    for unit, flags in platform_units():
        obj = object_path(unit)
        n.build(obj, "wii_cc_file", unit, implicit=[*common, PLATFORM_SEMANTICS, flag_files[flags]],
                variables={"flags": flag_files[flags].as_posix()})
        objects.append(obj)

    engine_list = ENGINE_BUILD / "engine-objects.txt"
    all_list = ENGINE_BUILD / "link-objects.txt"
    for path, items in ((engine_list, engine_objects), (all_list, [*engine_objects, *objects])):
        text = "".join(item.as_posix() + "\n" for item in items)
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8", newline="\n")

    elf, dol, link_map = BUILD / "engine.elf", BUILD / "engine.dol", BUILD / "engine.map"
    n.build(elf, "wii_engine_link", implicit=[*engine_objects, *objects, engine_list, all_list,
                                              BUILD / "local-config.json", *libraries],
            implicit_outputs=link_map,
            variables={"objects": all_list.as_posix(), "engine_objects": engine_list.as_posix(),
                       "link_flags": " ".join(f"--extra-link-flag={flag}"
                                              for flag in link_flags(diagnostic_storage))})
    n.build(dol, "wii_dol", elf, implicit=BUILD / "local-config.json")
    manifest = BUILD / "engine-build-info.json"
    n.build(manifest, "wii_manifest", implicit=[elf, dol, link_map, BUILD / "local-config.json"],
            variables={"stem": "engine", "scope": "engine_platform_runtime_no_embedded_assets"})
    n.build("wii_engine", "phony", [dol, manifest])
    n.newline()
