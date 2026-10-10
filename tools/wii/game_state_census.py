"""Measure the upstream game state's allocations with the compiler (HWI-007).

Every game-state allocation site in the engine (game_state_data_new,
game_state_malloc, game_state_gpu_malloc, game_state_memory_pool_new and
game_state_lruv_cache_new) is listed in SITES with the count and element-size
expressions the source passes. Each site's expressions are evaluated by the
compiler inside a generated translation unit that includes the site's actual
source file, so every constant and sizeof is the engine's own. Nothing is
executed: the values are read back from a data section of the object file.

The default compiler is clang for i686 Linux with the upstream native ABI flags
(tools/linux_build.py), run in WSL. `--check-sites` only verifies that SITES
matches the call sites in the source (no compiler), for CI.

Sizes are allocation requests, in the order-independent sum the bump allocator
charges (game_state_malloc asserts 4-byte multiples and adds no padding).
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CALL = re.compile(r"game_state_(data_new|malloc|gpu_malloc|memory_pool_new|lruv_cache_new)\s*\(")

# (label, source, kind, count expression, size expression, multiplicity, call-site source or None)
# kind: data (count elements of size + struct data_array), bytes (size), gpu (size, GPU part),
# pool (size + struct memory_pool), lruv (lruv_allocation_size(count)).
SITES = [
    ("header", "source/saved games/game_state.c", "bytes", "1", "sizeof(struct game_state_header)", 1, None),
    ("actor", "source/ai/actors.c", "data", "MAXIMUM_ACTORS", "sizeof(struct actor_datum)", 1, "source/ai/actors.c"),
    ("swarm", "source/ai/actors.c", "data", "MAXIMUM_SWARMS", "sizeof(struct swarm_datum)", 1, "source/ai/actors.c"),
    ("swarm component", "source/ai/actors.c", "data", "MAXIMUM_SWARM_COMPONENTS", "SWARM_COMPONENT_DATUM_SIZE", 1,
     "source/ai/actors.c"),
    ("ai globals", "source/ai/ai.c", "bytes", "1", "sizeof(struct ai_globals)", 1, "source/ai/ai.c"),
    # 16 bytes per table row before the table's sentinel row (the last one in the source)
    ("ai communication dialogue", "source/ai/ai_communication.c", "bytes", "1", "16 * (NUMBER_OF_DIALOGUE_USAGES - 1)", 1,
     "source/ai/ai_communication.c"),
    ("ai communication replies", "source/ai/ai_communication.c", "bytes", "1", "16 * (NUMBER_OF_REPLY_USAGES - 1)", 1,
     "source/ai/ai_communication.c"),
    ("ai conversation", "source/ai/ai_communication.c", "data", "8", "100", 1, "source/ai/ai_communication.c"),
    ("encounter", "source/ai/encounters.c", "data", "MAXIMUM_ENCOUNTERS", "sizeof(struct encounter_datum)", 1,
     "source/ai/encounters.c"),
    ("squad", "source/ai/encounters.c", "bytes", "1", "MAXIMUM_SQUADS_PER_MAP * sizeof(struct squad_datum)", 1,
     "source/ai/encounters.c"),
    ("platoon", "source/ai/encounters.c", "bytes", "1", "MAXIMUM_PLATOONS_PER_MAP * sizeof(struct platoon_datum)", 1,
     "source/ai/encounters.c"),
    ("ai pursuit", "source/ai/encounters.c", "data", "MAXIMUM_EXAMINED_PURSUIT_POSITIONS_PER_MAP",
     "sizeof(struct pursuit_datum)", 1, "source/ai/encounters.c"),
    ("prop", "source/ai/props.c", "data", "HALO_PORT_MAXIMUM_PROPS", "sizeof(struct prop_datum)", 1, "source/ai/props.c"),
    ("director scripting", "source/camera/director.c", "bytes", "1", "4", 1, "source/camera/director.c"),
    ("cinematic globals", "source/cutscene/cinematics.c", "bytes", "1", "sizeof(*cinematic_globals)", 1,
     "source/cutscene/cinematics.c"),
    ("recorded animations", "source/cutscene/recorded_animations.c", "data", "64", "sizeof(struct animation_thread)", 1,
     "source/cutscene/recorded_animations.c"),
    ("device groups", "source/devices/devices.c", "data", "1024", "sizeof(struct device_group_datum)", 1,
     "source/devices/devices.c"),
    ("contrail", "source/effects/contrails.c", "data", "HALO_PORT_MAXIMUM_CONTRAILS", "0x44", 1,
     "source/effects/contrails.c"),
    ("contrail point", "source/effects/contrails.c", "data", "MAXIMUM_CONTRAIL_POINTS", "0x38", 1,
     "source/effects/contrails.c"),
    ("decals", "source/effects/decals.c", "data", "MAXIMUM_DECALS_PER_MAP", "sizeof(struct decal_datum)", 1,
     "source/effects/decals.c"),
    ("decal globals", "source/effects/decals.c", "bytes", "1", "sizeof(struct decal_globals)", 1,
     "source/effects/decals.c"),
    ("effect", "source/effects/effects.c", "data", "HALO_PORT_MAXIMUM_EFFECTS", "0xFC", 1, "source/effects/effects.c"),
    ("effect location", "source/effects/effects.c", "data", "HALO_PORT_MAXIMUM_EFFECT_LOCATIONS", "0x3C", 1,
     "source/effects/effects.c"),
    ("particle systems", "source/effects/particle_systems.c", "data", "MAXIMUM_PARTICLE_SYSTEMS",
     "PARTICLE_SYSTEM_DATUM_SIZE", 1, "source/effects/particle_systems.c"),
    ("particle system particles", "source/effects/particle_systems.c", "data", "MAXIMUM_SYSTEM_PARTICLES",
     "SYSTEM_PARTICLE_DATUM_SIZE", 1, "source/effects/particle_systems.c"),
    ("particle", "source/effects/particles.c", "data", "HALO_PORT_MAXIMUM_PARTICLES", "0x70", 1,
     "source/effects/particles.c"),
    ("player effects", "source/effects/player_effects.c", "bytes", "1",
     "sizeof(struct player_effect_globals_definition)", 1, "source/effects/player_effects.c"),
    ("game globals", "source/game/game.c", "bytes", "1", "sizeof(*game_globals)", 1, "source/game/game.c"),
    ("game allegiance globals", "source/game/game_allegiance.c", "bytes", "1", "sizeof(*game_allegiance_globals)", 1,
     "source/game/game_allegiance.c"),
    ("game time globals", "source/game/game_time.c", "bytes", "1", "sizeof(*game_time_globals)", 1,
     "source/game/game_time.c"),
    ("player control globals", "source/game/player_control.c", "bytes", "1", "sizeof(*player_control_globals)", 1,
     "source/game/player_control.c"),
    ("rumble", "source/game/player_rumble.c", "bytes", "1", "sizeof(*rumble_globals)", 1,
     "source/game/player_rumble.c"),
    ("players", "source/game/players.c", "data", "NETWORK_GAME_MAXIMUM_PLAYER_COUNT", "sizeof(struct player_datum)", 1,
     "source/game/players.c"),
    ("teams", "source/game/players.c", "data", "16", "0x40", 1, "source/game/players.c"),
    ("players globals", "source/game/players.c", "bytes", "1", "sizeof(struct players_globals)", 1,
     "source/game/players.c"),
    ("hs thread", "source/hs/hs_runtime.c", "data", "0x100", "0x218", 1, "source/hs/hs_runtime.c"),
    ("hs globals", "source/hs/hs_runtime.c", "data", "0x400", "8", 1, "source/hs/hs_runtime.c"),
    ("object list header", "source/hs/object_lists.c", "data", "MAXIMUM_OBJECT_LISTS_PER_MAP",
     "sizeof(struct object_list_header_datum)", 1, "source/hs/object_lists.c"),
    # reference_list_new (source/objects/reference_lists.h) from object_lists.c
    ("list object reference", "source/hs/object_lists.c", "data", "MAXIMUM_LISTED_OBJECTS_PER_MAP",
     "sizeof(struct data_reference)", 1, "source/objects/reference_lists.h"),
    ("first person weapons", "source/interface/first_person_weapons.c", "bytes", "1",
     "sizeof(*first_person_weapons) * MAXIMUM_NUMBER_OF_LOCAL_PLAYERS", 1, "source/interface/first_person_weapons.c"),
    ("hud scripted globals", "source/interface/hud.c", "bytes", "1", "sizeof(*hud_scripted_globals)", 1,
     "source/interface/hud.c"),
    ("hud messaging", "source/interface/hud_messaging.c", "bytes", "1", "sizeof(*hud_messaging_globals)", 1,
     "source/interface/hud_messaging.c"),
    ("hud nav points", "source/interface/hud_nav_points.c", "bytes", "1", "0xC0", 1,
     "source/interface/hud_nav_points.c"),
    ("hud unit interface", "source/interface/hud_unit.c", "bytes", "1", "sizeof(*unit_hud_globals)", 1,
     "source/interface/hud_unit.c"),
    ("hud weapon interface", "source/interface/hud_weapon.c", "bytes", "1", "sizeof(*weapon_hud_globals)", 1,
     "source/interface/hud_weapon.c"),
    ("motion sensor (radar)", "source/interface/motion_sensor.c", "bytes", "1", "5544", 1,
     "source/interface/motion_sensor.c"),
    ("lights", "source/objects/object_lights.c", "data", "MAXIMUM_LIGHTS_PER_MAP", "sizeof(struct light_datum)", 1,
     "source/objects/object_lights.c"),
    ("lights globals", "source/objects/object_lights.c", "bytes", "1", "sizeof(struct lights_game_globals)", 1,
     "source/objects/object_lights.c"),
    ("object", "source/objects/objects.c", "data", "MAXIMUM_OBJECTS_PER_MAP", "sizeof(struct object_header_datum)", 1,
     "source/objects/objects.c"),
    ("objects (memory pool)", "source/objects/objects.c", "pool", "1", "OBJECT_MEMORY_POOL_SIZE", 1,
     "source/objects/objects.c"),
    ("object globals", "source/objects/objects.c", "bytes", "1", "sizeof(*object_globals)", 1,
     "source/objects/objects.c"),
    ("object name list", "source/objects/objects.c", "bytes", "1",
     "MAXIMUM_OBJECT_NAMES_PER_SCENARIO * sizeof(*object_name_list)", 1, "source/objects/objects.c"),
    # cluster_partition_new: collideable object, noncollideable object (objects.c) and light (object_lights.c)
    ("cluster partition first references", "source/structures/cluster_partitions.c", "bytes", "1",
     "MAXIMUM_CLUSTERS_PER_STRUCTURE * sizeof(*((struct cluster_partition *)0)->cluster_first_data_references)", 3,
     "source/structures/cluster_partitions.c"),
    ("cluster partition data references", "source/structures/cluster_partitions.c", "data",
     "HALO_PORT_MAXIMUM_CLUSTER_REFERENCES", "sizeof(struct data_reference)", 3, None),
    ("cluster partition cluster references", "source/structures/cluster_partitions.c", "data",
     "HALO_PORT_MAXIMUM_CLUSTER_REFERENCES", "sizeof(struct data_reference)", 3, None),
    ("antenna", "source/objects/widgets/antenna.c", "data", "MAXIMUM_ANTENNAS", "sizeof(struct antenna_datum)", 1,
     "source/objects/widgets/antenna.c"),
    ("flag", "source/objects/widgets/flags.c", "data", "2", "0x16BC", 1, "source/objects/widgets/flags.c"),
    ("glow", "source/objects/widgets/glow.c", "data", "MAXIMUM_GLOWS", "sizeof(struct glow_datum)", 1,
     "source/objects/widgets/glow.c"),
    ("glow particles", "source/objects/widgets/glow.c", "data", "MAXIMUM_GLOW_PARTICLES", "sizeof(struct glow_particle)",
     1, "source/objects/widgets/glow.c"),
    ("light volumes", "source/objects/widgets/light_volumes.c", "data", "256", "8", 1,
     "source/objects/widgets/light_volumes.c"),
    ("lightnings", "source/objects/widgets/lightning.c", "data", "256", "8", 1, "source/objects/widgets/lightning.c"),
    ("widget", "source/objects/widgets/widgets.c", "data", "MAXIMUM_WIDGETS_PER_MAP", "sizeof(struct widget_datum)", 1,
     "source/objects/widgets/widgets.c"),
    ("breakable surface globals", "source/physics/breakable_surfaces.c", "bytes", "1",
     "sizeof(struct breakable_surface_globals)", 1, "source/physics/breakable_surfaces.c"),
    ("rasterizer model ambient reflection tint", "source/rasterizer/rasterizer.c", "bytes", "1",
     "sizeof(*global_rasterizer_model_ambient_reflection_tint)", 1, "source/rasterizer/rasterizer.c"),
    ("screen effect filth", "source/rasterizer/rasterizer_cinematics.c", "bytes", "1",
     "sizeof(*cinematic_screen_effect_globals)", 1, "source/rasterizer/rasterizer_cinematics.c"),
    ("decal vertices", "source/rasterizer/xbox/rasterizer_xbox_decals.c", "gpu", "1", "DECAL_VERTEX_CACHE_SIZE", 1,
     "source/rasterizer/xbox/rasterizer_xbox_decals.c"),
    ("decal vertex cache", "source/rasterizer/xbox/rasterizer_xbox_decals.c", "lruv", "MAXIMUM_DECALS_PER_MAP",
     "sizeof(struct lruv_cache_block)", 1, "source/rasterizer/xbox/rasterizer_xbox_decals.c"),
    ("cached object render states", "source/render/render_objects.c", "data", "MAXIMUM_CACHED_OBJECT_RENDER_STATES",
     "sizeof(struct object_render_state)", 1, "source/render/render_objects.c"),
    ("scenario globals", "source/scenario/scenario.c", "bytes", "1", "sizeof(*scenario_globals)", 1,
     "source/scenario/scenario.c"),
    ("object looping sounds", "source/sound/game_sound.c", "data", "MAXIMUM_GAME_LOOPING_SOUNDS",
     "sizeof(struct game_looping_sound_datum)", 1, "source/sound/game_sound.c"),
    ("game sound globals", "source/sound/game_sound.c", "bytes", "1", "sizeof(*game_sound_globals)", 1,
     "source/sound/game_sound.c"),
    ("sound classes", "source/sound/sound_classes.c", "bytes", "1", "0x264", 1, "source/sound/sound_classes.c"),
    ("structure detail objects", "source/structures/structure_detail_objects.c", "bytes", "1",
     "sizeof(*detail_object_global_runtime_data)", 1, "source/structures/structure_detail_objects.c"),
    ("structure decals", "source/structures/structure_runtime_decals.c", "bytes", "1",
     "sizeof(*structure_decals_globals)", 1, "source/structures/structure_runtime_decals.c"),
    ("unit globals", "source/units/units.c", "bytes", "1", "sizeof(*unit_globals)", 1, "source/units/units.c"),
]

# Measured alongside the sites: the headers each kind charges, and the reservation.
FIXED = [
    ("sizeof(struct data_array)", "source/memory/data.c", "sizeof(struct data_array)"),
    ("sizeof(struct memory_pool)", "source/memory/memory_pool.c", "sizeof(struct memory_pool)"),
    ("lruv_allocation_size(0)", "source/memory/lruv_cache.c", "sizeof(struct lruv_cache) + sizeof(struct data_array)"),
    ("GAME_STATE_CPU_SIZE", "source/saved games/game_state.c", "GAME_STATE_CPU_SIZE"),
    ("GAME_STATE_GPU_SIZE", "source/saved games/game_state.c", "GAME_STATE_GPU_SIZE"),
]

UPSTREAM_ABI = ["--target=i686-linux-gnu", "-m32", "-fms-extensions", "-fshort-wchar", "-malign-double", "-fcommon",
                "-std=gnu89", "-D__STRICT_ANSI__", "-w", "-Wno-error=incompatible-pointer-types",
                "-Wno-error=incompatible-function-pointer-types", "-Wno-error=int-conversion",
                "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int", "-Wno-error=return-type",
                "-DDEBUG", "-Dxbox", "-fsyntax-only"]


def source_call_sites(root=ROOT):
    """(file, call kind) for every game-state allocation call outside game_state.c, in source order."""
    found = []
    for path in sorted((root / "source").rglob("*.[ch]")):
        relative = path.relative_to(root).as_posix()
        if relative.startswith("source/saved games/game_state"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        found.extend((relative, match.group(1)) for match in CALL.finditer(text))
    return found


def check_sites(root=ROOT):
    """Every source call site has a SITES row naming it (kind for kind), and no row names a missing one."""
    kinds = {"data": "data_new", "bytes": "malloc", "gpu": "gpu_malloc", "pool": "memory_pool_new",
             "lruv": "lruv_cache_new"}
    expected = sorted(source_call_sites(root))
    listed = sorted((site[6], kinds[site[2]]) for site in SITES if site[6])
    errors = []
    if expected != listed:
        missing = [item for item in expected if item not in listed or expected.count(item) > listed.count(item)]
        extra = [item for item in listed if item not in expected or listed.count(item) > expected.count(item)]
        errors.append(f"SITES differ from the source: missing {sorted(set(missing))}, extra {sorted(set(extra))}")
    return errors


def translation_unit(source, index, expressions):
    # One array keeps the values in order in the section.
    values = ",\n\t".join(f"(long)({expression})" for expression in expressions)
    return (f'#include "{source}"\n'
            f"__attribute__((used, section(\".hwi_census\"))) static const long hwi_census_{index}[] =\n"
            f"{{\n\t{values}\n}};\n")


def wsl_path(path):
    path = Path(path).resolve()
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]


def include_flags(root):
    config = json.loads((root / "port/linux/port.json").read_text())
    return [f"-I{wsl_path(root / directory)}" for directory in config["game"]["include_dirs"]]


def semantics_header(root, work):
    """The upstream Linux build's force-included MSVC semantics header (file-scope struct tags)."""
    output = work / "halo_msvc_semantics.h"
    subprocess.run([sys.executable, str(root / "tools/linux_msvc_semantics.py"), "--output", str(output),
                    "--all-inlines", "--tags", "source", "--inlines", "source", "--inlines", "port/include/xdk"],
                   cwd=root, check=True)
    return output


def measure(root, work):
    """Compile one unit per source and read each expression's value from its object."""
    work.mkdir(parents=True, exist_ok=True)
    semantics = semantics_header(root, work)
    groups = {}
    for index, site in enumerate(SITES):
        expressions = [site[3], site[4]]
        groups.setdefault(site[1], []).append((("site", index), expressions))
    for index, (label, source, expression) in enumerate(FIXED):
        groups.setdefault(source, []).append((("fixed", index), [expression]))
    flags = ["clang", *[flag for flag in UPSTREAM_ABI if flag != "-fsyntax-only"], "-c",
             "-include", wsl_path(root / "port/linux/include/halo_linux_prefix.h"),
             "-include", wsl_path(semantics),
             f"-I{wsl_path(root / 'port/linux/include')}", f"-I{wsl_path(root / 'port/include/xdk')}",
             f"-iquote{wsl_path(root / 'port/linux/game')}", *include_flags(root)]
    values = {}
    commands = []
    for unit_number, (source, entries) in enumerate(sorted(groups.items())):
        expressions = []
        keys = []
        for key, items in entries:
            for item in items:
                keys.append(key)
                expressions.append(item)
        unit = work / f"census_{unit_number}.c"
        unit.write_text(translation_unit(wsl_path(root / source), unit_number, expressions), encoding="ascii",
                        newline="\n")
        obj = work / f"census_{unit_number}.o"
        blob = work / f"census_{unit_number}.bin"
        script = (f"set -e; {' '.join(repr_sh(flag) for flag in flags)} {repr_sh(wsl_path(unit))} -o {repr_sh(wsl_path(obj))}; "
                  f"objcopy -O binary --only-section=.hwi_census {repr_sh(wsl_path(obj))} {repr_sh(wsl_path(blob))}")
        run = subprocess.run(["wsl", "-d", "Debian", "--", "sh", "-c", script], capture_output=True, text=True)
        commands.append({"unit": unit.name, "source": source, "exit_code": run.returncode,
                         "stderr_tail": run.stderr.strip().splitlines()[-3:]})
        if run.returncode != 0:
            continue
        data = blob.read_bytes()
        if len(data) != 4 * len(expressions):
            raise ValueError(f"{unit.name}: {len(data)} census bytes for {len(expressions)} values")
        words = [int.from_bytes(data[i:i + 4], "little", signed=True) for i in range(0, len(data), 4)]
        for key, word in zip(keys, words):
            values.setdefault(key, []).append(word)
    return values, commands, flags


def repr_sh(text):
    return "'" + str(text).replace("'", "'\"'\"'") + "'"


def compiler_identity():
    run = subprocess.run(["wsl", "-d", "Debian", "--", "clang", "--version"], capture_output=True, text=True)
    return run.stdout.splitlines()[0] if run.returncode == 0 else None


def report(values, commands, flags, root=ROOT):
    fixed = {}
    for index, (label, _, _) in enumerate(FIXED):
        got = values.get(("fixed", index))
        fixed[label] = got[0] if got else None
    rows = []
    totals = {"cpu": 0, "gpu": 0}
    unmeasured = []
    for index, (label, source, kind, count_expr, size_expr, multiplicity, _) in enumerate(SITES):
        got = values.get(("site", index))
        row = {"label": label, "source": source, "kind": kind, "count_expression": count_expr,
               "size_expression": size_expr, "multiplicity": multiplicity}
        if not got:
            row["measured"] = False
            unmeasured.append(label)
            rows.append(row)
            continue
        count, size = got
        if kind == "data":
            each = count * size + fixed["sizeof(struct data_array)"]
        elif kind == "pool":
            each = size + fixed["sizeof(struct memory_pool)"]
        elif kind == "lruv":
            each = fixed["lruv_allocation_size(0)"] + count * size
        else:
            each = count * size
        row.update(measured=True, count=count, element_size=size, bytes_each=each, bytes=each * multiplicity)
        totals["gpu" if kind == "gpu" else "cpu"] += each * multiplicity
        rows.append(row)
    cpu_size, gpu_size = fixed["GAME_STATE_CPU_SIZE"], fixed["GAME_STATE_GPU_SIZE"]
    source_hash = hashlib.sha256()
    for path in sorted({site[1] for site in SITES} | {item[1] for item in FIXED}):
        source_hash.update(path.encode() + b"\0" + hashlib.sha256((root / path).read_bytes()).digest())
    return {
        "schema_version": 1,
        "scope": "upstream game-state allocation census: compile-time values of each site's own expressions",
        "compiler": compiler_identity(), "abi_flags": UPSTREAM_ABI[:-1],
        "sources_sha256": source_hash.hexdigest(),
        "fixed": fixed, "sites": rows, "unmeasured": unmeasured,
        "cpu_requested": totals["cpu"], "gpu_requested": totals["gpu"],
        "cpu_reservation": cpu_size, "gpu_reservation": gpu_size,
        "cpu_unused": None if cpu_size is None else cpu_size - totals["cpu"],
        "compiles": commands,
    }


KIND_CODES = {"data": 1, "pool": 2, "bytes": 3, "gpu": 4, "lruv": 5}


def c_table(result):
    """The measured allocations as C data for the PPC game-state prototype (tools/wii/game_state_image.c)."""
    lines = ["/* Generated by tools/wii/game_state_census.py --emit-c from the upstream game-state census",
             f" * (sources SHA-256 {result['sources_sha256']}; {result['compiler']}, upstream i686 ABI).",
             " * Do not edit. Each row: one allocation in census order, repeated multiplicity times. */",
             '#include "game_state_image.h"', "",
             "const struct gs_census_row gs_census_rows[] = {"]
    for row in result["sites"]:
        name = row["label"].replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'\t{{"{name}", {KIND_CODES[row["kind"]]}, {row["count"]}, {row["element_size"]}, '
                     f'{row["bytes_each"]}, {row["multiplicity"]}}},')
    lines += ["};", "", f"const unsigned gs_census_row_count = {len(result['sites'])}u;",
              f"const unsigned long gs_census_cpu_requested = {result['cpu_requested']}ul;",
              f"const unsigned long gs_census_gpu_requested = {result['gpu_requested']}ul;",
              f"const unsigned long gs_census_data_array_header = {result['fixed']['sizeof(struct data_array)']}ul;",
              f"const unsigned long gs_census_memory_pool_header = {result['fixed']['sizeof(struct memory_pool)']}ul;",
              ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-sites", action="store_true", help="only check SITES against the source")
    parser.add_argument("--work", type=Path, default=ROOT / ".local/game-state-census")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--emit-c", type=Path, help="also write the measured rows as C data")
    args = parser.parse_args()
    errors = check_sites()
    if errors or args.check_sites:
        print("\n".join(errors) if errors else f"{len(SITES)} census rows cover {len(source_call_sites())} call sites")
        return 1 if errors else 0
    values, commands, flags = measure(ROOT, args.work)
    result = report(values, commands, flags)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    if args.emit_c and not result["unmeasured"]:
        args.emit_c.write_text(c_table(result), encoding="utf-8", newline="\n")
    print(json.dumps({key: result[key] for key in ("cpu_requested", "gpu_requested", "cpu_reservation",
                                                   "gpu_reservation", "cpu_unused", "unmeasured")}))
    return 0 if not result["unmeasured"] else 2


if __name__ == "__main__":
    sys.exit(main())
