"""HWI-085: the typed game-state image's production schema and codec (source/saved games/game_state_schema.c),
compiled for the host and checked against wire bytes specified by hand (game_state_schema_check.c), and the
image's dispatch around it (game_state_image.c, game_state.c): version 2 is written and read as it was, a
version-3 image is written only when the completeness gate passes and refused before anything is taken."""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAVED = ROOT / "source" / "saved games"
IMAGE = SAVED / "game_state_image.c"
IMAGE_HEADER = SAVED / "game_state_image.h"
GAME_STATE = SAVED / "game_state.c"
SCHEMA = SAVED / "game_state_schema.c"
CLASSES = SAVED / "game_state_schema_classes.c"
# the cluster partitions (cluster_partition_new's names: objects.c, object_lights.c), and the allocations each
# census row of them stands for
CLUSTER_PARTITIONS = ("collideable object", "noncollideable object", "light")
CLUSTER_PARTITION_ROWS = {
    "cluster partition first references": list(CLUSTER_PARTITIONS),
    "cluster partition data references": ["cluster " + name + " reference" for name in CLUSTER_PARTITIONS],
    "cluster partition cluster references": [name + " cluster reference" for name in CLUSTER_PARTITIONS],
}


def compiler():
    for name in ("gcc", "cc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


def function_body(text, name):
    start = re.search(r"^\w[\w *]*\b" + name + r"\(\n", text, re.M)
    if not start:
        raise AssertionError(name)
    end = text.index("\n}\n", start.end())
    return text[start.end():end]


@unittest.skipUnless(compiler(), "no host C compiler")
class CodecFixtureTests(unittest.TestCase):
    def test_production_codec_against_specified_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            name = "game_state_schema_check.exe" if sys.platform == "win32" else "game_state_schema_check"
            exe = Path(temp) / name
            build = subprocess.run([compiler(), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(ROOT / "source"), "-o", str(exe),
                                    str(ROOT / "tools" / "wii" / "game_state_schema_check.c")],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-4000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            match = re.fullmatch(r"ok (\d+) cases\s*", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            # (127 before the globals of scalars: game_state_schema_check.c's check_named_globals; 249 before
            # the sound state: check_sound_state; 298 before the reference lists: check_reference_lists)
            self.assertGreaterEqual(int(match.group(1)), 360)


class DispatchTests(unittest.TestCase):
    def setUp(self):
        self.image = IMAGE.read_text(encoding="utf-8")
        self.header = IMAGE_HEADER.read_text(encoding="utf-8")
        self.game_state = GAME_STATE.read_text(encoding="utf-8")

    def test_saves_stay_version_2(self):
        self.assertIn("GAME_STATE_IMAGE_VERSION = 2,", self.header)
        self.assertIn("GAME_STATE_IMAGE_TYPED_VERSION = 3,", self.header)
        self.assertRegex(self.header, r"#define GAME_STATE_IMAGE_WRITE_VERSION GAME_STATE_IMAGE_VERSION\n")
        body = function_body(self.game_state, "game_state_port_image_write")
        self.assertIn("GAME_STATE_IMAGE_WRITE_VERSION", body)
        # version 2 goes through the unchanged writer
        body = function_body(self.image, "game_state_image_write_version")
        self.assertRegex(body, r"if \(version == GAME_STATE_IMAGE_VERSION\)\n\t\treturn game_state_image_write\(")
        self.assertIn("image_write_or_measure(layout, header, file, digest, GAME_STATE_IMAGE_VERSION)",
                      function_body(self.image, "game_state_image_write"))
        self.assertIn("image_write_or_measure(layout, header, NULL, bytes, GAME_STATE_IMAGE_VERSION)",
                      function_body(self.image, "game_state_image_digest"))

    def test_typed_write_is_gated_before_anything_is_written(self):
        body = function_body(self.image, "image_write_or_measure")
        gate = body.index("game_state_schema_coverage(&typed_state, &coverage)")
        refused = body.index("return FALSE;", gate)
        self.assertIn("if (!coverage.complete)", body[gate:refused])
        self.assertIn("_game_state_image_refusal_schema", body[gate:refused])
        self.assertLess(refused, body.index("file->write("))
        self.assertLess(refused, body.index("image_allocate("))

    def test_version_2_reading_is_retained(self):
        body = function_body(self.image, "reader_pass")
        typed = body.index("if (reader->typed)")
        version_2 = body.index("else", typed)
        order = body.index("_game_state_image_refusal_byte_order", version_2)
        # the byte-order refusal of version 2, as it was
        self.assertIn("image_port_big_endian() ? IMAGE_BIG_ENDIAN : IMAGE_LITTLE_ENDIAN", body[version_2:order])
        self.assertIn("IMAGE_ENCODING_WORDS", body[order:order + 300])
        # version 2's walk of the allocations, then the rest of the CPU and GPU parts
        self.assertIn("reader_data_array(reader, allocation", body)
        self.assertIn("reader_memory_pool(reader, allocation)", body)
        self.assertIn("!reader->typed &&", body)
        self.assertIn("reader_take(reader, layout->size - layout->gpu_used, layout->gpu_used)", body)

    def test_typed_reading_is_gated_before_the_payload(self):
        body = function_body(self.image, "reader_pass")
        gate = body.index("game_state_schema_coverage(&typed_state, &coverage)")
        self.assertIn("_game_state_image_refusal_schema", body[gate:gate + 800])
        self.assertIn("HEADER_SCHEMA_IDENTITY", body[gate:gate + 1200])
        self.assertLess(gate, body.index("reader_typed(reader, &typed_state)"))
        self.assertLess(gate, body.index("reader->chunk_checksums = image_allocate"))
        # the load still checks the whole image before it takes it, and a
        # failure while taking is reported, never retried here
        read = self.image[self.image.index("static short image_read("):self.image.index("/* ---------- public code */")]
        first = read.index("reader_pass(&reader")
        self.assertLess(first, read.index("before_taking();"))
        self.assertLess(read.index("before_taking();"), read.index("reader_pass(&reader", first + 1))
        self.assertIn("result = _game_state_image_failed_while_taking;", read)

    def test_header_accepts_only_versions_2_and_3(self):
        body = function_body(self.image, "image_header_check")
        self.assertIn("GAME_STATE_IMAGE_VERSION", body)
        self.assertIn("GAME_STATE_IMAGE_TYPED_VERSION", body)
        self.assertRegex(self.image, r"\tHEADER_SCHEMA_IDENTITY = 412,")
        self.assertRegex(self.image, r"\tHEADER_TYPED_BYTES = 416,")

    def test_refusal_names_follow_the_enum(self):
        names = re.search(r"names\[NUMBER_OF_GAME_STATE_IMAGE_REFUSALS\] =\s*\{(.*?)\};", self.image, re.S).group(1)
        enum = re.search(r"enum game_state_image_refusal\s*\{(.*?)\};", self.header, re.S).group(1)
        values = re.findall(r"_game_state_image_refusal_(\w+),", enum)
        self.assertEqual(re.findall(r'"(\w+)"', names), values)
        self.assertEqual(values[-1], "schema")


class SchemaSourceTests(unittest.TestCase):
    def test_registrations_of_the_first_classes(self):
        classes = CLASSES.read_text(encoding="utf-8")
        for name in ('"header"', '"object name list"', '"lightnings"', '"light volumes"'):
            self.assertIn("{ " + name + ",", classes)
        game_time = (ROOT / "source/game/game_time.c").read_text(encoding="utf-8")
        self.assertIn("game_state_port_schema(game_time_globals, &game_time_globals_schema);", game_time)
        hs = (ROOT / "source/hs/hs_runtime.c").read_text(encoding="utf-8")
        self.assertIn("game_state_port_schema(hs_thread_data, &hs_thread_schema);", hs)
        self.assertIn("game_state_port_schema(hs_global_data, &hs_global_schema);", hs)
        # HS cells stay unresolved: never typed as an integer of a width
        self.assertRegex(hs, r'"value", _game_state_schema_hs_cell')
        self.assertRegex(hs, r'"result", _game_state_schema_hs_cell')
        self.assertRegex(hs, r'"stack_data", _game_state_schema_unresolved')

    def test_classes_are_found_by_the_whole_allocation_name(self):
        """a region keeps its name cut to 31 characters: its class is looked up by the name it was allocated
        under (one longer: "rasterizer model ambient reflection tint")"""
        text = GAME_STATE.read_text(encoding="utf-8")
        # (the definition, not the declaration before it)
        start = text.index("static void game_state_region_record(\n\tconst char *name,\n\tvoid const *address,\n"
                           "\tlong size,\n\tboolean gpu)\n{")
        record = text[start:text.index("\n}\n", start)]
        self.assertIn("game_state_schema_class_find(name, _game_state_schema_region_global, size)", record)
        self.assertIn("game_state_schema_class_find(name, _game_state_schema_region_data_array, size)",
                      function_body(text, "game_state_data_new"))
        self.assertNotIn("game_state_schema_class_find(region->name", text)

    def test_globals_of_scalars_are_the_engines_allocations(self):
        """each global class's name is an allocation's (game_state_malloc's first argument) and the census's"""
        classes = CLASSES.read_text(encoding="utf-8")
        named = re.findall(r'\{ "([^"]+)", _game_state_schema_region_global, &\w+ \}', classes)
        for name in ("director scripting", "hud scripted globals", "rasterizer model ambient reflection tint",
                     "lights globals", "structure decals", "decal globals", "breakable surface globals",
                     "game sound globals", "sound classes"):
            self.assertIn(name, named)
        allocated = set()
        for path in (ROOT / "source").rglob("*.c"):
            allocated.update(re.findall(r'game_state_malloc\(\s*"([^"]+)"',
                                        path.read_text(encoding="utf-8", errors="replace")))
        census = (ROOT / "tools/wii/game_state_census.py").read_text(encoding="utf-8")
        for name in named:
            # (a cluster partition's first references are allocated under the partition's name, given at run time:
            # test_reference_lists_are_the_engines)
            if name == "header" or name in CLUSTER_PARTITIONS:
                continue
            self.assertIn(name, allocated)
            self.assertIn('("' + name + '", ', census)

    def test_private_definitions_are_checked_against_their_schemas(self):
        checks = {
            "source/effects/decals.c": ["sizeof(struct decal_globals) == 0x280C",
                                        "offsetof(struct decal_globals, first_disconnected_decal_index) == 0x2800"],
            "source/physics/breakable_surfaces.c": [
                "sizeof(struct breakable_surface_globals) == 0x4204",
                "offsetof(struct breakable_surface_globals, breakable_surface_flags) == 0x1",
                "breakable_surface_flags[0]) == 8 * sizeof(long)",
                "offsetof(struct breakable_surface_globals, breakable_surfaces) == 0x204"],
            "source/objects/object_lights.c": ["offsetof(struct lights_game_globals, reserved01) == 0x1"],
            "source/structures/structure_runtime_decals.c": [
                "sizeof(struct structure_decals_globals) == 0x4",
                "offsetof(struct structure_decals_globals, pad) == 0x1"],
            "source/sound/game_sound.c": [
                "sizeof(struct game_looping_sound_datum) == 0x34",
                "offsetof(struct game_looping_sound_datum, flags) == 0x4",
                "sizeof(((struct game_looping_sound_datum *)0)->flags) == 4",
                "offsetof(struct game_looping_sound_datum, definition_index) == 0xC",
                "offsetof(struct game_looping_sound_datum, object_index) == 0x10",
                "offsetof(struct game_looping_sound_datum, forward) == 0x28",
                "sizeof(struct game_sound_globals) == 0x8",
                "offsetof(struct game_sound_globals, music_looping_sound_index) == 0x4"],
            "source/sound/sound_classes.c": [
                "sizeof(struct sound_class_datum) == 0xC",
                "NUMBER_OF_SOUND_CLASSES * sizeof(struct sound_class_datum) == 0x264",
                "offsetof(struct sound_class_datum, ticks) == 0x8"],
            "source/structures/cluster_partitions.c": [
                "sizeof(struct data_reference) == 0xC",
                "offsetof(struct data_reference, pad) == 0x2",
                "offsetof(struct data_reference, datum_index) == 0x4",
                "offsetof(struct data_reference, next_reference_index) == 0x8",
                "MAXIMUM_CLUSTERS_PER_STRUCTURE * sizeof(*((struct cluster_partition *)0)->cluster_first_data_references) == 0x800"],
            "source/hs/object_lists.c": [
                "sizeof(struct object_list_header_datum) == 0xC",
                "offsetof(struct object_list_header_datum, reference_count) == 0x4",
                "offsetof(struct object_list_header_datum, count) == 0x6",
                "offsetof(struct object_list_header_datum, first_reference_index) == 0x8"],
        }
        for path, needles in checks.items():
            text = (ROOT / path).read_text(encoding="utf-8")
            self.assertIn("game_state_schema_classes.c's", text, path)
            for needle in needles:
                self.assertIn(needle, text, path)

    def test_padding_is_bytes_no_code_reads(self):
        """the pad bytes the new schemas drop are never named by code, and the director's allocation is only
        ever its first byte"""
        sources = {path: path.read_text(encoding="utf-8", errors="replace")
                   for path in (ROOT / "source").rglob("*.[ch]")}
        for pattern in (r"structure_decals_globals->pad\b", r"hud_scripted_globals->pad\b",
                        r"lights_game_globals->reserved01\b", r"director_camera_scripted\s*\[",
                        r"director_camera_scripted\s*\+"):
            hits = [str(path) for path, text in sources.items() if re.search(pattern, text)]
            self.assertEqual(hits, [], pattern)

    def test_breakable_surface_flags_are_read_as_longs(self):
        """the flags are typed as 8 longs a bsp: nothing indexes their bytes, and every bit test or set of them is
        through a long pointer"""
        for path in (ROOT / "source").rglob("*.[ch]"):
            text = path.read_text(encoding="utf-8", errors="replace")
            # (but the member's declaration: byte breakable_surface_flags[...][32])
            for match in re.finditer(r"(?<!byte )breakable_surface_flags(?:_scratch)?\s*\[(\w*)\](\s*\[)?", text):
                self.assertEqual(path.name, "breakable_surfaces.c", match.group(0))
                self.assertIn(match.group(1), ("bsp_index", "global_structure_bsp_index", "0"), match.group(0))
                self.assertIsNone(match.group(2), match.group(0))
            for match in re.finditer(r"BIT_VECTOR_\w+\(\s*([^,]+),", text):
                argument = " ".join(match.group(1).split())
                if "breakable_surface_flags" not in argument:
                    continue
                if argument.startswith(("(long *)", "(long const *)")):
                    continue
                # (a variable already a long pointer)
                self.assertRegex(text, r"long const \*" + re.escape(argument) + r"\s*=", f"{path.name}: {argument}")

    def test_sound_state_is_the_engines(self):
        """HWI-080: the looping sounds are game_sound.c's data array of its datum, found by its whole name; a sound
        class's 2 pad bytes are named by no code (nothing copies, compares or indexes a whole datum); the looping
        sound's flags are only ever tested and set as the long they are"""
        classes = CLASSES.read_text(encoding="utf-8")
        self.assertIn('{ "object looping sounds", _game_state_schema_region_data_array, &game_looping_sound_schema },',
                      classes)
        game_sound = (ROOT / "source/sound/game_sound.c").read_text(encoding="utf-8")
        self.assertRegex(game_sound, r'game_state_data_new\(\s*"object looping sounds",\s*MAXIMUM_GAME_LOOPING_SOUNDS,'
                                     r'\s*sizeof\(struct game_looping_sound_datum\)\)')
        self.assertIn("game_state_schema_classes.c's", game_sound)
        for match in re.finditer(r"\b(?:sound|looping_sound)->flags\b", game_sound):
            line = game_sound[game_sound.rindex("\n", 0, match.start()):game_sound.index("\n", match.end())]
            context = game_sound[max(0, match.start() - 40):match.start()]
            self.assertTrue(re.search(r"(TEST_FLAG|SET_FLAG)\(\s*$", context) or "flags = 0;" in line, line)
        sound_classes = (ROOT / "source/sound/sound_classes.c").read_text(encoding="utf-8")
        self.assertRegex(sound_classes, r'game_state_malloc\(\s*"sound classes",\s*NULL,\s*0x264\)')
        self.assertIn("NUMBER_OF_SOUND_CLASSES = 51", sound_classes)
        self.assertEqual(set(re.findall(r"\bsound_class->(\w+)", sound_classes)), {"desired_gain", "gain", "ticks"})
        # (a datum is had by sound_class_datum_get, into a struct sound_class_datum *sound_class, or its gain read)
        self.assertEqual(set(re.findall(r"sound_class_datum_get\(\w+\)(->\w+|;)", sound_classes)), {"->gain", ";"})
        self.assertEqual(len(re.findall(r"= sound_class_datum_get\(", sound_classes)),
                         len(re.findall(r"struct sound_class_datum \*sound_class = sound_class_datum_get\(", sound_classes)))
        # (and the one indexing of the classes, the accessor's)
        self.assertEqual(re.findall(r".*sound_class_data\s*\[.*", sound_classes), ["\treturn &sound_class_data[index];"])
        whole = re.search(r"(?<!struct sound_class_datum )\*\s*sound_class\b(?!_)|memcpy|memcmp|memset", sound_classes)
        self.assertIsNone(whole, whole and whole.group(0))
        for path in (ROOT / "source").rglob("*.[ch]"):
            if path.name != "sound_classes.c":
                self.assertNotIn("sound_class_data", path.read_text(encoding="utf-8", errors="replace"), path)

    def test_unclaimed_classes_are_the_listed_ones(self):
        """the census's allocations with no schema yet are exactly these (the gate counts them unclaimed: no
        version-3 write); one schema'd from here on is taken off the list, and a new allocation is not silently
        left off it"""
        import sys
        sys.path.insert(0, str(ROOT / "tools" / "wii"))
        import game_state_census
        classes = CLASSES.read_text(encoding="utf-8")
        schemad = set(re.findall(r'\{ "([^"]+)", _game_state_schema_region_\w+, &\w+ \}', classes))
        for path in (ROOT / "source").rglob("*.c"):
            text = path.read_text(encoding="utf-8", errors="replace")
            for variable in re.findall(r"game_state_port_schema\((\w+),", text):
                allocation = re.search(r"\b" + variable + r'\s*=\s*(?:\([^)]*\)\s*)?game_state_(?:malloc|data_new)\(\s*"([^"]+)"',
                                       text)
                self.assertIsNotNone(allocation, f"{path.name}: {variable}")
                schemad.add(allocation.group(1))
        # (an lruv cache is its header and its blocks' data array: the engine's own containers' schemas; a census
        # row of the cluster partitions is claimed when each partition's allocation of it is)
        unclaimed = [site[0] for site in game_state_census.SITES
                     if not set(CLUSTER_PARTITION_ROWS.get(site[0], [site[0]])) <= schemad and site[2] != "lruv"]
        self.assertEqual(unclaimed, [
            "actor", "swarm", "swarm component", "ai globals", "ai communication dialogue", "ai communication replies",
            "ai conversation", "encounter", "squad", "platoon", "ai pursuit", "prop", "cinematic globals",
            "recorded animations", "device groups", "contrail", "contrail point", "decals", "effect", "effect location",
            "particle systems", "particle system particles", "particle", "player effects", "game globals",
            "player control globals", "players", "teams", "players globals", "first person weapons", "hud messaging",
            "hud nav points", "hud unit interface", "hud weapon interface", "motion sensor (radar)", "lights", "object",
            "objects (memory pool)",  # (its header typed; its blocks' contents, the objects, are not)
            "object globals", "antenna", "flag", "glow", "glow particles", "widget", "screen effect filth",
            "decal vertices", "cached object render states", "scenario globals", "structure detail objects",
            "unit globals"])
        for name in ("object looping sounds", "game sound globals", "sound classes", "object list header",
                     "list object reference"):
            self.assertIn(name, schemad)
        for names in CLUSTER_PARTITION_ROWS.values():
            self.assertLessEqual(set(names), schemad)

    def test_reference_lists_are_the_engines(self):
        """HWI-080: the object lists' two data arrays and each cluster partition's three allocations are found by
        the whole names the engine makes for them; a reference's and an object list's pad word is named by no code,
        and a datum of either is only ever copied whole (reference_list_copy) from another of its array"""
        classes = CLASSES.read_text(encoding="utf-8")
        object_lists = (ROOT / "source/hs/object_lists.c").read_text(encoding="utf-8")
        self.assertIn('game_state_data_new("object list header", MAXIMUM_OBJECT_LISTS_PER_MAP, '
                      'sizeof(struct object_list_header_datum))', object_lists)
        self.assertIn('reference_list_new("list object", MAXIMUM_LISTED_OBJECTS_PER_MAP)', object_lists)
        reference_lists = (ROOT / "source/objects/reference_lists.h").read_text(encoding="utf-8")
        self.assertIn('sprintf(buffer, "%s reference", name);\n\treturn game_state_data_new(buffer, maximum_count, '
                      'sizeof(struct data_reference));', reference_lists)
        partitions = (ROOT / "source/structures/cluster_partitions.c").read_text(encoding="utf-8")
        new = function_body(partitions, "cluster_partition_new")
        self.assertRegex(new, r"game_state_malloc\(\s*name,\s*\"cluster references\",\s*MAXIMUM_CLUSTERS_PER_STRUCTURE \* "
                              r"sizeof\(\*partition->cluster_first_data_references\)\)")
        data = new.index('sprintf(cluster_name, "cluster %s", name);')
        cluster = new.index('sprintf(cluster_name, "%s cluster", name);', data)
        self.assertIn("partition->data_reference_data = reference_list_new(cluster_name,", new[data:cluster])
        self.assertIn("partition->cluster_reference_data = reference_list_new(cluster_name,", new[cluster:cluster + 200])
        made = re.findall(r'cluster_partition_new\(&\w+, "([^"]+)"\)',
                          (ROOT / "source/objects/objects.c").read_text(encoding="utf-8") +
                          (ROOT / "source/objects/object_lights.c").read_text(encoding="utf-8"))
        self.assertEqual(sorted(made), sorted(CLUSTER_PARTITIONS))
        for partition in CLUSTER_PARTITIONS:
            for name, kind, schema in ((partition, "global", "cluster_first_references_schema"),
                                       ("cluster " + partition + " reference", "data_array", "data_reference_schema"),
                                       (partition + " cluster reference", "data_array", "cluster_reference_schema")):
                self.assertIn('{ "' + name + '", _game_state_schema_region_' + kind + ", &" + schema + " },", classes)
        for name, schema in (("object list header", "object_list_header_schema"),
                             ("list object reference", "data_reference_schema")):
            self.assertIn('{ "' + name + '", _game_state_schema_region_data_array, &' + schema + " },", classes)
        # (a cluster's reference holds a cluster index: cluster_partition_reconnect adds cluster_index,
        # cluster_partition_disconnect reads it back as a short)
        self.assertIn("partition->cluster_reference_data,\n\t\t\tfirst_cluster_reference,\n\t\t\tcluster_index,", partitions)
        self.assertIn("short const cluster_index = (short)cluster_reference->datum_index;", partitions)
        # the pad words: never named, nor a datum compared or set as a whole outside data.c's datum code
        users = {path: path.read_text(encoding="utf-8", errors="replace") for path in (ROOT / "source").rglob("*.[ch]")}
        users = {path: text for path, text in users.items()
                 if re.search(r"struct (?:data_reference|object_list_header_datum)\b", text)}
        self.assertEqual(sorted(path.name for path in users),
                         ["cluster_partitions.c", "object_lists.c", "object_lists.h", "reference_lists.h"])
        # (and the accessors of their datums are used nowhere else)
        for path in (ROOT / "source").rglob("*.[ch]"):
            if path not in users:
                self.assertIsNone(re.search(r"\bobject_list_(?:header_)?get\(", path.read_text(encoding="utf-8",
                                                                                              errors="replace")), path.name)
        for path, text in users.items():
            # (but its layout check's sizeof(((struct data_reference *)0)->pad))
            self.assertIsNone(re.search(r"(?<!\*\)0\))(?:->|\.)pad\b", text), path.name)
            self.assertIsNone(re.search(r"\b(?:cs)?mem(?:cpy|cmp|set)\s*\(\s*(?:\(\w+ \*\))?\s*&?\s*\*?\s*(?:reference|list)\b",
                                        text), path.name)
        whole = [line.strip() for text in users.values() for line in text.splitlines()
                 if re.search(r"\*\s*\w*(?:reference|list)\s*=\s*\*\s*\w+\s*;", line)]
        self.assertEqual(whole, ["*result_reference = *source_reference;"])
        # (and datum_initialize clears a whole datum before it is given out)
        data = (ROOT / "source/memory/data.c").read_text(encoding="utf-8")
        self.assertIn("csmemset(header, 0, data->size);", function_body(data, "datum_initialize"))

    def test_every_allocation_is_a_region(self):
        text = GAME_STATE.read_text(encoding="utf-8")
        for name in ("game_state_malloc", "game_state_gpu_malloc"):
            self.assertIn("game_state_region_record(name, pointer, size, ", function_body(text, name))
        for name, kind in (("game_state_data_new", "data_array"), ("game_state_memory_pool_new", "memory_pool"),
                           ("game_state_lruv_cache_new", "lruv_cache")):
            self.assertIn("_game_state_schema_region_" + kind, function_body(text, name))

    def test_no_untyped_bytes_are_written(self):
        """the codec has no opaque or host-memory field type: unresolved bytes are refused"""
        text = SCHEMA.read_text(encoding="utf-8")
        encode = function_body(text, "schema_encode_instance")
        self.assertRegex(encode, r"case _game_state_schema_hs_cell:\n\t\tcase _game_state_schema_unresolved:\n"
                                 r"\t\t\treturn _game_state_schema_stream_unresolved;")
        self.assertNotIn("memcpy", text)


if __name__ == "__main__":
    unittest.main()
