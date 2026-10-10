/*
ENGINE_MAP_RUN.C

The real-map run shared by the Wii driver and the i686 host reference
(engine_map_run.h, HWI-015B, HWI-016C). Platform side: C types and the Wii
platform layer's log and clock only; the engine is reached through
fixed_step_scenario.h, real_map_scenario.h and cache_files_wii.h.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wii_platform.h"
#include "engine_map_run.h"
#include "fixed_step_scenario.h"
#include "real_map_scenario.h"
#include "cache_files_wii.h"

#define FIXED_STEP_SEED 0x48574931UL /* "HWI1", as HWI-015A */
#define FIXED_STEP_DIGEST 0xc1ad6ae6UL
#define FIXED_STEP_CHAIN 0x58ce9719UL
#define PROGRESS_INTERVAL 150

enum cadence
{
	_cadence_60,
	_cadence_30,
	_cadence_20,
	_cadence_144,
	_cadence_irregular,
	_cadence_vsync,
};

static const char *const cadence_names[ENGINE_MAP_RUN_CADENCES] =
{
	"60hz", "30hz", "20hz", "144hz", "irregular", "vsync_measured"
};

/* a line of the plan (engine_map_run.h) */
struct map_line
{
	int failure;                 /* a "fail" line */
	char scenario_path[128];
	char name[32];               /* the scenario path's last part */
	long ticks;
	int cycles;
	unsigned cadences;           /* bit per enum cadence */
	int warmup;
	int has_expect;
	unsigned long expect;
	long switch_tick;
	int switch_index;
	short expected_error;        /* a fail line's, or a map line's expect_error= */
	char directory[96];          /* a fail line's maps directory */
};

static struct map_line plan_lines[ENGINE_MAP_RUN_MAXIMUM_LINES];
static unsigned long reference_digests[REAL_MAP_MAXIMUM_TICKS];
static const char *current_map = "";
static int progress_logging;

/* the frame times of a run paced by the vertical retrace, logged so the host
can replay them (VSYNC_DT lines) */
#define VSYNC_MAXIMUM_FRAMES (REAL_MAP_MAXIMUM_TICKS * 2 + 400)
static float vsync_frames[VSYNC_MAXIMUM_FRAMES];
static long vsync_frame_count;

/* ---------- allocation dumps (engine_map_run_set_dump) and progress */

#define DUMP_MAXIMUM 8

static long dump_indices[DUMP_MAXIMUM];
static int dump_count;
static long dump_tick = -1;
static long dump_run;
static char dump_directory[128];

static void dump_allocations(long tick)
{
	if (tick == 1)
		dump_run++;
	if (tick != dump_tick)
		return;
	for (int item = 0; item < dump_count; item++)
	{
		long index = dump_indices[item];
		char path[192];
		FILE *file;
		const void *address;
		unsigned long bytes = 0;

		if (index >= real_map_allocation_count())
			continue;
		if (index == -2)
		{
			/* the tag slot's loaded ranges, one after another */
			struct wii_cache_load_report load;

			wii_cache_load_report_get(&load);
			snprintf(path, sizeof(path), "%s/dump-%ld-tags.bin", dump_directory, dump_run);
			file = fopen(path, "wb");
			for (long range = 0; file && range < load.range_count; range++)
				fwrite((const char *)real_map_tag_slot_address() + load.range_offset[range], 1, load.range_size[range], file);
			if (file)
				fclose(file);
			continue;
		}
		if (index < 0)
		{
			for (long other = 0; other < real_map_allocation_count(); other++)
			{
				unsigned long end = real_map_allocation_offset(other) + real_map_allocation_bytes(other);

				if (end > bytes && end <= 0x1400000UL)
					bytes = end;
			}
			address = (const char *)real_map_allocation_address(0) - real_map_allocation_offset(0);
		}
		else
		{
			address = real_map_allocation_address(index);
			bytes = real_map_allocation_bytes(index);
		}
		snprintf(path, sizeof(path), "%s/dump-%ld-%ld.bin", dump_directory, dump_run, index);
		file = fopen(path, "wb");
		if (file)
		{
			fwrite(address, 1, bytes, file);
			fclose(file);
		}
	}
}

static void tick_observer(long tick)
{
	if (dump_count)
		dump_allocations(tick);
	if (progress_logging && (tick == 1 || tick % PROGRESS_INTERVAL == 0))
	{
		struct real_map_progress progress;

		real_map_progress_get(&progress);
		wii_log("PROGRESS map=%s tick=%ld bsp=%d cinematic=%d objects=%ld actors=%ld script_threads=%ld "
			"player_unit=%08lx position=%.3f,%.3f,%.3f\n", current_map, progress.tick, progress.structure_bsp_index,
			progress.cinematic, progress.objects, progress.actors, progress.script_threads,
			(unsigned long)progress.player_unit, (double)progress.player_position[0],
			(double)progress.player_position[1], (double)progress.player_position[2]);
	}
}

int engine_map_run_set_dump(const char *spec, const char *directory)
{
	const char *colon = spec ? strchr(spec, ':') : NULL;
	const char *cursor = spec;

	if (!colon || !directory)
		return 0;
	dump_count = 0;
	while (cursor < colon && dump_count < DUMP_MAXIMUM)
	{
		char *end;
		long index = strtol(cursor, &end, 10);

		if (end == cursor)
			return 0;
		dump_indices[dump_count++] = index;
		cursor = *end == ',' ? end + 1 : end;
	}
	dump_tick = strtol(colon + 1, NULL, 10);
	snprintf(dump_directory, sizeof(dump_directory), "%s", directory);
	dump_run = 0;
	return dump_count > 0 && dump_tick > 0;
}

/* ---------- the plan */

static short error_kind(const char *name)
{
	for (short kind = 0; kind < NUMBER_OF_WII_MAP_ERRORS; kind++)
	{
		if (!strcmp(name, wii_cache_error_name(kind)))
			return kind;
	}
	return -1;
}

static int parse_cadences(const char *text, unsigned *mask)
{
	char copy[128];
	char *token;

	snprintf(copy, sizeof(copy), "%s", text);
	*mask = 0;
	for (token = strtok(copy, ","); token; token = strtok(NULL, ","))
	{
		int found = 0;

		for (int cadence = 0; cadence < ENGINE_MAP_RUN_CADENCES; cadence++)
		{
			if (!strcmp(token, cadence_names[cadence]))
			{
				*mask |= 1u << cadence;
				found = 1;
			}
		}
		if (!found)
			return 0;
	}
	return *mask != 0;
}

/* one line of the plan; 0 if it is not understood */
static int parse_line(char *text, int default_cycles, struct map_line *line)
{
	char *words[12];
	int count = 0;
	int first = 0;
	char *cursor;

	memset(line, 0, sizeof(*line));
	for (cursor = strtok(text, " \t"); cursor && count < 12; cursor = strtok(NULL, " \t"))
		words[count++] = cursor;
	if (!count)
		return 0;
	line->ticks = REAL_MAP_TICKS;
	line->cycles = default_cycles;
	line->cadences = (1u << ENGINE_MAP_RUN_CADENCES) - 1;
	line->warmup = 1;
	line->expected_error = -1;
	if (!strcmp(words[0], "fail"))
	{
		if (count < 3)
			return 0;
		line->failure = 1;
		line->expected_error = error_kind(words[2]);
		if (line->expected_error <= 0)
			return 0;
		first = 1;
	}
	snprintf(line->scenario_path, sizeof(line->scenario_path), "%s", words[first]);
	{
		const char *name = strrchr(line->scenario_path, '\\');

		snprintf(line->name, sizeof(line->name), "%s", name ? name + 1 : line->scenario_path);
	}
	for (int index = first + (line->failure ? 2 : 1); index < count; index++)
	{
		char *word = words[index];
		char *value = strchr(word, '=');

		if (!value)
			return 0;
		*value++ = 0;
		if (!strcmp(word, "ticks"))
			line->ticks = strtol(value, NULL, 10);
		else if (!strcmp(word, "cycles"))
			line->cycles = (int)strtol(value, NULL, 10);
		else if (!strcmp(word, "cadences"))
		{
			if (!parse_cadences(value, &line->cadences))
				return 0;
		}
		else if (!strcmp(word, "warmup"))
			line->warmup = (int)strtol(value, NULL, 10);
		else if (!strcmp(word, "expect"))
		{
			line->has_expect = 1;
			line->expect = strtoul(value, NULL, 16);
		}
		else if (!strcmp(word, "switch_bsp"))
		{
			char *colon = strchr(value, ':');

			if (!colon)
				return 0;
			line->switch_tick = strtol(value, NULL, 10);
			line->switch_index = (int)strtol(colon + 1, NULL, 10);
		}
		else if (!strcmp(word, "expect_error"))
		{
			line->expected_error = error_kind(value);
			if (line->expected_error <= 0)
				return 0;
		}
		else if (!strcmp(word, "directory"))
			snprintf(line->directory, sizeof(line->directory), "%s", value);
		else
			return 0;
	}
	return line->ticks >= 1 && line->ticks <= REAL_MAP_MAXIMUM_TICKS && line->cycles >= 1 && line->cycles <= 8 &&
		(line->switch_tick >= 0 && line->switch_tick < line->ticks);
}

/* the plan's lines; -1 if a line is not understood */
static int parse_plan(const char *plan, int default_cycles)
{
	char text[4096];
	char *line;
	char *next;
	int count = 0;

	snprintf(text, sizeof(text), "%s", plan ? plan : "");
	for (line = text; line && *line; line = next)
	{
		size_t length;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		length = strlen(line);
		while (length && (line[length - 1] == '\r' || line[length - 1] == ' '))
			line[--length] = 0;
		if (!length || line[0] == '#')
			continue;
		if (count >= ENGINE_MAP_RUN_MAXIMUM_LINES || !parse_line(line, default_cycles, &plan_lines[count]))
		{
			wii_log("PLAN error line=%d\n", count + 1);
			return -1;
		}
		count++;
	}
	return count;
}

/* ---------- runs */

static float cadence_dt(const struct engine_map_run_config *config, enum cadence cadence, long frame,
	unsigned long *state)
{
	switch (cadence)
	{
	case _cadence_60: return 1.0f / 60.0f;
	case _cadence_30: return 1.0f / 30.0f;
	case _cadence_20: return 1.0f / 20.0f;
	case _cadence_144: return 1.0f / 144.0f;
	case _cadence_irregular:
		/* 4 ms to 83 ms, and one 0.5 s hitch (beyond a local game's 7 ticks
		a frame, whose time the engine discards) */
		if (frame == 40)
			return 0.5f;
		*state = *state * 1664525UL + 1013904223UL;
		return (4.0f + (float)(*state >> 16 & 0xffff) / 65535.0f * 79.0f) / 1000.0f;
	case _cadence_vsync:
	default:
	{
		float dt = config->vsync_dt ? config->vsync_dt(config->context) : 1.0f / 60.0f;

		if (vsync_frame_count < VSYNC_MAXIMUM_FRAMES)
			vsync_frames[vsync_frame_count++] = dt;
		return dt;
	}
	}
}

static int run_fixed_step(void)
{
	struct fixed_step_scenario_result result;
	long frame = 0;
	int pass;

	if (!fixed_step_scenario_begin(FIXED_STEP_SEED))
		return 0;
	while (frame < 100000 && fixed_step_scenario_frame(1.0f / 30.0f))
		frame++;
	fixed_step_scenario_end(&result);
	pass = result.ticks == FIXED_STEP_SCENARIO_TICKS && result.digest == FIXED_STEP_DIGEST &&
		result.chain == FIXED_STEP_CHAIN;
	wii_log("FIXED_STEP cadence=30hz ticks=%ld digest=%08lx chain=%08lx expected=%08lx/%08lx pass=%d\n",
		result.ticks, result.digest, result.chain, FIXED_STEP_DIGEST, FIXED_STEP_CHAIN, pass);
	return pass;
}

static int run_cadence(const struct engine_map_run_config *config, const struct map_line *line, int cycle,
	enum cadence cadence, int reference, struct real_map_result *result, unsigned long *milliseconds)
{
	unsigned long state = 12345;
	long frame = 0;
	unsigned long long started;

	real_map_set_command(line->switch_tick, NULL);
	if (line->switch_tick)
	{
		char command[48];

		snprintf(command, sizeof(command), "(switch_bsp %d)", line->switch_index);
		real_map_set_command(line->switch_tick, command);
	}
	progress_logging = reference;
	if (!real_map_begin(reference ? NULL : reference_digests, line->ticks))
		return 0;
	vsync_frame_count = 0;
	if (cadence == _cadence_vsync && config->vsync_begin)
		config->vsync_begin(config->context);
	started = wii_time_microseconds();
	while (frame < 40L * REAL_MAP_MAXIMUM_TICKS && real_map_frame(cadence_dt(config, cadence, frame, &state)))
		frame++;
	*milliseconds = (unsigned long)((wii_time_microseconds() - started) / 1000ULL);
	real_map_end(result);
	progress_logging = 0;
	if (reference)
		memcpy(reference_digests, real_map_tick_digests(), sizeof(unsigned long) * (size_t)line->ticks);
	wii_log("RUN map=%s cycle=%d cadence=%s ticks=%ld frames=%ld frames_without_ticks=%ld most_ticks_in_a_frame=%ld "
		"digest=%08lx chain=%08lx at1=%08lx at30=%08lx at150=%08lx atend=%08lx "
		"sim=%08lx sim_chain=%08lx sim_at1=%08lx sim_at30=%08lx sim_at150=%08lx sim_atend=%08lx first_divergent_tick=%ld "
		"seed=%08lx objects=%ld player_unit=%08lx spawn_tick=%ld position=%.6f,%.6f,%.6f tags_end=%08lx "
		"bsp=%d bsp_switches=%ld command_tick=%ld command_result=%ld requests_left=%ld ms=%lu\n",
		line->name, cycle, cadence_names[cadence], result->ticks, result->frames, result->frames_without_ticks,
		result->most_ticks_in_a_frame, result->digest, result->chain, result->digest_at[0], result->digest_at[1],
		result->digest_at[2], result->digest_at[3], result->simulation_digest, result->simulation_chain,
		result->simulation_at[0], result->simulation_at[1], result->simulation_at[2], result->simulation_at[3],
		result->first_divergent_tick, result->random_seed,
		result->objects, (unsigned long)result->player_unit, result->player_spawn_tick,
		(double)result->player_position[0], (double)result->player_position[1], (double)result->player_position[2],
		result->tags_at_end, result->structure_bsp_index, result->bsp_switches, result->command_tick,
		result->command_result, result->requests_left, *milliseconds);
	if (cadence == _cadence_vsync)
	{
		/* the frame times, bit for bit, ten a line */
		for (long frame = 0; frame < vsync_frame_count; frame += 10)
		{
			char text[16 + 10 * 9];
			int used = 0;

			for (long index = frame; index < frame + 10 && index < vsync_frame_count; index++)
			{
				unsigned long bits;

				memcpy(&bits, &vsync_frames[index], sizeof(bits));
				used += snprintf(text + used, sizeof(text) - (size_t)used, " %08lx", bits);
			}
			wii_log("VSYNC_DT map=%s cycle=%d from=%ld%s\n", line->name, cycle, frame, text);
		}
	}
	return result->ticks == line->ticks;
}

/* a "fail" line: the load must be refused with its kind, and nothing left loaded */
static int run_failure(const struct map_line *line, struct engine_map_run_report *report)
{
	char text[384];
	unsigned long long started = wii_time_microseconds();
	int loaded;
	short kind;
	int pass;

	wii_cache_files_set_map_directory(line->directory[0] ? line->directory : NULL);
	loaded = real_map_load(line->scenario_path);
	kind = wii_cache_last_error(text, sizeof(text));
	if (loaded)
		real_map_unload();
	wii_cache_files_set_map_directory(NULL);
	pass = !loaded && kind == line->expected_error;
	report->expectations++;
	report->expectations_met += pass;
	if (!pass)
		report->failures++;
	wii_log("LOAD_FAILURE map=%s expected=%s kind=%s loaded=%d ms=%lu pass=%d text=\"%s\"\n", line->name,
		wii_cache_error_name(line->expected_error), wii_cache_error_name(kind), loaded,
		(unsigned long)((wii_time_microseconds() - started) / 1000ULL), pass, text);
	return pass;
}

static int run_map(const struct engine_map_run_config *config, const struct map_line *line, int first_map,
	struct engine_map_run_report *report)
{
	struct real_map_result first;
	struct real_map_result cadence_first[ENGINE_MAP_RUN_CADENCES];
	long first_heap = 0;
	int have_first = 0;
	int identical = 1, whole_identical = 1, heap_flat = 1, runs = 0;

	memset(&first, 0, sizeof(first));
	memset(cadence_first, 0, sizeof(cadence_first));
	current_map = line->name;
	wii_log("MAP_LINE map=%s scenario=%s ticks=%ld cycles=%d cadences=%x warmup=%d expect=%08lx switch_bsp=%ld:%d "
		"expect_error=%s\n", line->name, line->scenario_path, line->ticks, line->cycles, line->cadences, line->warmup,
		line->has_expect ? line->expect : 0UL, line->switch_tick, line->switch_index,
		line->expected_error > 0 ? wii_cache_error_name(line->expected_error) : "none");

	/* a warm-up run first, not measured: a map leaves state behind for the
	next (a light's stale field, the game sound's first-map flag), so every
	measured run follows the same 30 Hz run */
	if (line->warmup)
	{
		struct real_map_result warm;
		unsigned long milliseconds = 0;
		int ran = real_map_load(line->scenario_path) &&
			run_cadence(config, line, -1, _cadence_30, 0, &warm, &milliseconds);

		real_map_unload();
		wii_log("WARMUP map=%s ran=%d\n", line->name, ran);
		if (!ran)
		{
			report->failures++;
			return 0;
		}
	}
	for (int cycle = 0; cycle < line->cycles; cycle++)
	{
		long heap;

		/* every run loads the map, as a new game does, and unloads it */
		for (int cadence = 0; cadence < ENGINE_MAP_RUN_CADENCES; cadence++)
		{
			struct wii_cache_load_report load;
			struct real_map_result result;
			unsigned long milliseconds = 0;
			int reference = !have_first;
			unsigned long long started;
			unsigned long load_milliseconds;
			int loaded;
			int ran;
			int same;
			int whole_same;

			if (!(line->cadences & (1u << cadence)))
				continue;
			started = wii_time_microseconds();
			loaded = real_map_load(line->scenario_path);
			load_milliseconds = (unsigned long)((wii_time_microseconds() - started) / 1000ULL);
			wii_cache_load_report_get(&load);
			wii_log("LOAD map=%s cycle=%d cadence=%s scenario=%s loaded=%d ms=%lu segments=%ld reads=%ld bytes=%lu "
				"relocations=%lu unstaged_reads=%lu cache_ms=%lu staging=%08lx unsupported=%lu\n", line->name, cycle,
				cadence_names[cadence], line->scenario_path, loaded, load_milliseconds, load.segments, load.reads,
				load.bytes, load.relocations, load.unstaged_reads, load.milliseconds, load.staging_digest,
				wii_unsupported_names());
			if (!loaded)
			{
				report->failures++;
				wii_log("FAIL map_load map=%s cycle=%d\n", line->name, cycle);
				return 0;
			}
			if (reference && first_map)
			{
				report->map_loaded = 1;
				report->load_milliseconds = load_milliseconds;
			}
			ran = run_cadence(config, line, cycle, (enum cadence)cadence, reference, &result, &milliseconds);
			runs++;
			report->runs++;
			if (ran)
			{
				for (long index = 0; index < real_map_allocation_count(); index++)
				{
					wii_log("ALLOC map=%s cycle=%d cadence=%s index=%ld name=\"%s\" offset=%lu bytes=%lu at1=%08lx at30=%08lx at150=%08lx atend=%08lx\n",
						line->name, cycle, cadence_names[cadence], index, real_map_allocation_name(index),
						real_map_allocation_offset(index), real_map_allocation_bytes(index),
						real_map_allocation_digest(index, 0), real_map_allocation_digest(index, 1),
						real_map_allocation_digest(index, 2), real_map_allocation_digest(index, 3));
				}
			}
			if (reference && ran)
			{
				first = result;
				have_first = 1;
				if (first_map)
				{
					report->digest = result.digest;
					report->chain = result.chain;
					report->simulation_digest = result.simulation_digest;
					report->simulation_chain = result.simulation_chain;
					report->tags_at_start = result.tags_at_start;
					report->player_spawn_tick = result.player_spawn_tick;
					report->run_milliseconds = milliseconds;
				}
				/* every tick's simulation digest, ten a line */
				for (long tick = 0; tick < line->ticks; tick += 10)
				{
					char text[16 + 10 * 9];
					int used = 0;

					for (long index = tick; index < tick + 10 && index < line->ticks; index++)
						used += snprintf(text + used, sizeof(text) - (size_t)used, " %08lx", reference_digests[index]);
					wii_log("TICKS map=%s from=%ld%s\n", line->name, tick + 1, text);
				}
			}
			/* the simulation the same at every cadence and in every cycle; the
			whole state the same as this cadence's in the first cycle */
			if (cycle == 0 && ran)
				cadence_first[cadence] = result;
			same = ran && have_first && result.simulation_digest == first.simulation_digest &&
				result.simulation_chain == first.simulation_chain && result.first_divergent_tick == -1 &&
				result.random_seed == first.random_seed && result.objects == first.objects &&
				result.tags_at_start == first.tags_at_start && result.tags_at_end == first.tags_at_end &&
				result.structure_bsp_index == first.structure_bsp_index;
			whole_same = ran && result.digest == cadence_first[cadence].digest &&
				result.chain == cadence_first[cadence].chain;
			if (!same)
			{
				identical = 0;
				report->identical = 0;
				report->failures++;
			}
			/* (an observation, not a failure: the first-person view model's idle
			pose timer draws on the local random numbers, which per-frame
			presentation also draws on, as the cycle before left it) */
			if (!whole_same)
			{
				whole_identical = 0;
				report->whole_identical = 0;
			}
			/* a damaged structure bsp refused as the script switches to it:
			the run keeps the bsp it had (expect_error=) */
			if (line->expected_error > 0)
			{
				char text[384];
				short kind = wii_cache_last_error(text, sizeof(text));
				int pass = kind == line->expected_error && result.structure_bsp_index == 0 && ran;

				report->expectations++;
				report->expectations_met += pass;
				if (!pass)
					report->failures++;
				wii_log("BSP_FAILURE map=%s cycle=%d cadence=%s expected=%s kind=%s bsp=%d pass=%d text=\"%s\"\n",
					line->name, cycle, cadence_names[cadence], wii_cache_error_name(line->expected_error),
					wii_cache_error_name(kind), result.structure_bsp_index, pass, text);
			}
			wii_log("RUN_SAME map=%s cycle=%d cadence=%s simulation_same=%d whole_same_as_cycle0=%d\n", line->name,
				cycle, cadence_names[cadence], same, whole_same);
			real_map_unload();
		}
		heap = config->heap_in_use ? config->heap_in_use(config->context) : 0;
		if (cycle == 0)
			first_heap = heap;
		else if (heap != first_heap)
			heap_flat = 0;
		wii_log("CYCLE map=%s cycle=%d heap_in_use=%ld unsupported_names=%lu unsupported_calls=%lu\n", line->name,
			cycle, heap, wii_unsupported_names(), wii_unsupported_calls());
	}
	if (!heap_flat)
	{
		report->heap_flat = 0;
		report->failures++;
	}
	if (line->has_expect)
	{
		int pass = have_first && first.simulation_digest == line->expect;

		report->expectations++;
		report->expectations_met += pass;
		if (!pass)
			report->failures++;
		wii_log("EXPECT map=%s simulation=%08lx expected=%08lx pass=%d\n", line->name, first.simulation_digest,
			line->expect, pass);
	}
	wii_log("MAP_RUNS map=%s runs=%d simulation_identical=%d whole_identical_per_cadence=%d heap_flat=%d digest=%08lx "
		"chain=%08lx sim=%08lx sim_chain=%08lx tags_start=%08lx spawn_tick=%ld\n",
		line->name, runs, identical, whole_identical, heap_flat, first.digest, first.chain, first.simulation_digest,
		first.simulation_chain, first.tags_at_start, first.player_spawn_tick);
	return have_first;
}

int engine_map_run(
	const struct engine_map_run_config *config,
	struct engine_map_run_report *report)
{
	int lines;
	int maps = 0;

	memset(report, 0, sizeof(*report));
	report->identical = 1;
	report->whole_identical = 1;
	report->heap_flat = 1;
	real_map_set_tick_observer(tick_observer);

	report->fixed_step_pass = run_fixed_step();
	if (!report->fixed_step_pass)
		report->failures++;
	lines = parse_plan(config->plan, config->cycles);
	if (lines <= 0)
	{
		report->failures++;
		wii_log("FAIL plan lines=%d\n", lines);
		return 0;
	}
	for (int index = 0; index < lines; index++)
	{
		const struct map_line *line = &plan_lines[index];

		if (line->failure)
			run_failure(line, report);
		else if (!run_map(config, line, maps++ == 0, report))
			return 0;
	}
	wii_log("MAP_PLAN lines=%d maps=%d runs=%d simulation_identical=%d whole_identical_per_cadence=%d heap_flat=%d "
		"expectations=%d met=%d map_errors=%lu failures=%d\n", lines, maps, report->runs, report->identical,
		report->whole_identical, report->heap_flat, report->expectations, report->expectations_met, wii_cache_errors(),
		report->failures);
	return report->failures == 0;
}
