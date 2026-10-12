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
#include "save_image_scenario.h"

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

/* (HWI-016F) the order a line's cadences run in: 30 Hz first, the game's
own (a frame a tick, as on the Xbox), so a line's reference run is the
native cadence's */
static const enum cadence cadence_order[ENGINE_MAP_RUN_CADENCES] =
{
	_cadence_30, _cadence_60, _cadence_20, _cadence_144, _cadence_irregular, _cadence_vsync
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
	unsigned long expect;        /* the simulation as HWI-015B classified it (HWI-015B's Prisoner: 127656ef) */
	int has_expect_simulation;
	unsigned long expect_simulation; /* (HWI-016F) the simulation, HWI-016F's frame-coupled fields left out */
	long switch_tick;
	int switch_index;
	/* HWI-030: switch_bsp=<tick>:<index>,<tick>:<index>,...: the switches
	after the first */
	int more_switches;
	long more_switch_ticks[REAL_MAP_MAXIMUM_MORE_COMMANDS];
	int more_switch_indices[REAL_MAP_MAXIMUM_MORE_COMMANDS];
	short expected_error;        /* a fail line's, or a map line's expect_error= */
	char directory[96];          /* a fail line's maps directory */
	int canonical_every_tick;    /* canonical=every (the default) or canonical=checkpoints */
	int invariance;              /* invariance=1 (the default): a run whose simulation differs is a failure */
	/* (HWI-034) script=<tick>:<command>: the command's script source ('~'
	for a space), run after that tick as switch_bsp's is; core_save=<tick>:
	a core saved by the harness after that tick (real_map_set_core_save) */
	long script_tick;
	char script[96];
	long core_save_tick;
	/* (HWI-076) image_test=<tick>: damaged copies of a core saved after that
	tick loaded and refused (save_image_scenario.h) */
	long image_test_tick;
};

static struct map_line plan_lines[ENGINE_MAP_RUN_MAXIMUM_LINES];
static unsigned long reference_digests[REAL_MAP_MAXIMUM_TICKS];
static unsigned long reference_raw_digests[REAL_MAP_MAXIMUM_TICKS];
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
	/* every allocation's digest at that tick, to name what differs */
	for (long index = 0; index < real_map_allocation_count(); index++)
	{
		wii_log("ALLOC_AT run=%ld tick=%ld index=%ld name=\"%s\" digest=%08lx\n", dump_run, tick, index,
			real_map_allocation_name(index), real_map_allocation_digest_now(index));
	}
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

/* (HWI-016F) "trace:<index>[,<index>...]": those allocations' raw digests
after every tick of every run, logged after the run (ALLOC_TRACE lines, a
hundred ticks a line: the log syncs every line), to find the tick at which
two runs' allocation first differs. Diagnosis only: its buffer is taken from
the heap when the spec is given. */
#define TRACE_PER_LINE 100

static long trace_indices[DUMP_MAXIMUM];
static int trace_count;
static long trace_run;
static long trace_ticks;
static unsigned long *trace_digests;

static void trace_allocations(long tick)
{
	if (tick == 1)
	{
		trace_run++;
		trace_ticks = 0;
	}
	if (!trace_digests || tick < 1 || tick > REAL_MAP_MAXIMUM_TICKS)
		return;
	for (int item = 0; item < trace_count; item++)
	{
		unsigned long digest = 0;

		if (trace_indices[item] >= 0 && trace_indices[item] < real_map_allocation_count())
			digest = real_map_allocation_digest_now(trace_indices[item]);
		trace_digests[(size_t)item * REAL_MAP_MAXIMUM_TICKS + (size_t)(tick - 1)] = digest;
	}
	trace_ticks = tick;
}

static void trace_flush(void)
{
	for (int item = 0; trace_digests && item < trace_count; item++)
	{
		for (long tick = 0; tick < trace_ticks; tick += TRACE_PER_LINE)
		{
			char text[16 + TRACE_PER_LINE * 9];
			int used = 0;

			for (long index = tick; index < tick + TRACE_PER_LINE && index < trace_ticks; index++)
				used += snprintf(text + used, sizeof(text) - (size_t)used, " %08lx",
					trace_digests[(size_t)item * REAL_MAP_MAXIMUM_TICKS + (size_t)index]);
			wii_log("ALLOC_TRACE run=%ld index=%ld from=%ld%s\n", trace_run, trace_indices[item], tick + 1, text);
		}
	}
	trace_ticks = 0;
}

/* (HWI-076) each save, load and check of a saved game's image, and the image
tests' cases, as IMAGE lines; the time from each one's beginning */
static unsigned long long image_began;

static void image_observer(const struct save_image_event *event)
{
	static const char *const kinds[] = {"save", "load", "check", "begin", "test", "test_summary"};
	unsigned long long now = wii_time_microseconds();

	if (event->kind == SAVE_IMAGE_EVENT_BEGIN)
	{
		image_began = now;
		return;
	}
	if (event->kind == SAVE_IMAGE_EVENT_SUMMARY)
	{
		wii_log("IMAGE_TEST map=%s tick=%ld original_valid=%d cases=%d refused_as_expected=%d intact=%d pass=%d\n",
			current_map, event->tick, !event->result, event->cases, event->cases_expected, event->cases_intact,
			!event->result && event->cases == event->cases_expected && event->cases == event->cases_intact);
		return;
	}
	wii_log("IMAGE map=%s event=%s tick=%ld case=%s result=%d refusal=%s at=%ld state_bytes=%lu file_bytes=%lu "
		"payload_bytes=%lu chunks=%lu chunk_bytes=%lu peak_bytes=%lu pointers=%lu references=%lu threads=%lu "
		"foreign=%lu address_like=%lu first_address_like=%ld dead_address_like=%lu passes=%d sha=%08lx%08lx%08lx%08lx%08lx%08lx%08lx%08lx "
		"state=%08lx same_as_saved=%d reencoded_same=%d intact=%d expected=%d us=%lu\n",
		current_map, kinds[event->kind], event->tick, event->test_case ? event->test_case : "-", event->result,
		event->refusal ? event->refusal : "none", event->refusal_offset, event->state_bytes, event->file_bytes,
		event->payload_bytes, event->chunks, event->chunk_bytes, event->peak_bytes, event->pointers, event->references,
		event->script_threads, event->foreign_pointers, event->address_like_words, event->first_address_like_offset,
		event->address_like_dead_words,
		event->passes, event->sha[0], event->sha[1], event->sha[2], event->sha[3], event->sha[4], event->sha[5],
		event->sha[6], event->sha[7], event->state_digest, event->same_as_saved, event->reencoded_same, event->intact,
		event->expected, event->kind == SAVE_IMAGE_EVENT_TEST ? 0UL : (unsigned long)(now - image_began));
}

static void tick_observer(long tick)
{
	save_image_scenario_after_tick(tick);
	if (trace_count)
		trace_allocations(tick);
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
	if (!strncmp(spec, "trace:", 6))
	{
		trace_count = 0;
		trace_run = 0;
		for (cursor = spec + 6; *cursor && trace_count < DUMP_MAXIMUM;)
		{
			char *end;
			long index = strtol(cursor, &end, 10);

			if (end == cursor)
				return 0;
			trace_indices[trace_count++] = index;
			cursor = *end == ',' ? end + 1 : end;
		}
		if (trace_count && !trace_digests)
			trace_digests = malloc(sizeof(unsigned long) * (size_t)REAL_MAP_MAXIMUM_TICKS * DUMP_MAXIMUM);
		return trace_count > 0 && trace_digests != NULL;
	}
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
	char *words[16];
	int count = 0;
	int first = 0;
	char *cursor;

	memset(line, 0, sizeof(*line));
	for (cursor = strtok(text, " \t"); cursor && count < 16; cursor = strtok(NULL, " \t"))
		words[count++] = cursor;
	if (!count)
		return 0;
	line->ticks = REAL_MAP_TICKS;
	line->cycles = default_cycles;
	line->cadences = (1u << ENGINE_MAP_RUN_CADENCES) - 1;
	line->warmup = 1;
	line->expected_error = -1;
	line->canonical_every_tick = 1;
	line->invariance = 1;
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
		else if (!strcmp(word, "expect_sim"))
		{
			line->has_expect_simulation = 1;
			line->expect_simulation = strtoul(value, NULL, 16);
		}
		else if (!strcmp(word, "switch_bsp"))
		{
			char *colon = strchr(value, ':');
			char *next;

			if (!colon)
				return 0;
			line->switch_tick = strtol(value, NULL, 10);
			line->switch_index = (int)strtol(colon + 1, &next, 10);
			/* (HWI-030) more switches, each "<tick>:<index>" after a comma */
			while (*next == ',')
			{
				char *pair = next + 1;
				int more = line->more_switches;

				colon = strchr(pair, ':');
				if (!colon || more >= REAL_MAP_MAXIMUM_MORE_COMMANDS)
					return 0;
				line->more_switch_ticks[more] = strtol(pair, NULL, 10);
				line->more_switch_indices[more] = (int)strtol(colon + 1, &next, 10);
				if (line->more_switch_ticks[more] < 1)
					return 0;
				line->more_switches++;
			}
		}
		else if (!strcmp(word, "expect_error"))
		{
			line->expected_error = error_kind(value);
			if (line->expected_error <= 0)
				return 0;
		}
		else if (!strcmp(word, "canonical"))
		{
			if (strcmp(value, "every") && strcmp(value, "checkpoints"))
				return 0;
			line->canonical_every_tick = !strcmp(value, "every");
		}
		else if (!strcmp(word, "invariance"))
			line->invariance = (int)strtol(value, NULL, 10);
		else if (!strcmp(word, "directory"))
			snprintf(line->directory, sizeof(line->directory), "%s", value);
		else if (!strcmp(word, "script"))
		{
			char *colon = strchr(value, ':');

			if (!colon || !colon[1] || strlen(colon + 1) >= sizeof(line->script))
				return 0;
			line->script_tick = strtol(value, NULL, 10);
			snprintf(line->script, sizeof(line->script), "%s", colon + 1);
			for (char *space = line->script; *space; space++)
			{
				if (*space == '~')
					*space = ' ';
			}
		}
		else if (!strcmp(word, "core_save"))
			line->core_save_tick = strtol(value, NULL, 10);
		else if (!strcmp(word, "image_test"))
			line->image_test_tick = strtol(value, NULL, 10);
		else if (!strcmp(word, "name"))
			snprintf(line->name, sizeof(line->name), "%s", value);
		else
			return 0;
	}
	for (int more = 0; more < line->more_switches; more++)
	{
		if (line->more_switch_ticks[more] >= line->ticks)
			return 0;
	}
	/* (one command a run: a script= or a switch_bsp=) */
	return line->ticks >= 1 && line->ticks <= REAL_MAP_MAXIMUM_TICKS && line->cycles >= 1 && line->cycles <= 8 &&
		(line->switch_tick >= 0 && line->switch_tick < line->ticks) &&
		(line->script_tick >= 0 && line->script_tick < line->ticks) && !(line->script_tick && line->switch_tick) &&
		(line->core_save_tick >= 0 && line->core_save_tick < line->ticks) &&
		(line->image_test_tick >= 0 && line->image_test_tick < line->ticks);
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
		for (int more = 0; more < line->more_switches; more++)
		{
			snprintf(command, sizeof(command), "(switch_bsp %d)", line->more_switch_indices[more]);
			real_map_add_command(line->more_switch_ticks[more], command);
		}
	}
	if (line->script_tick)
		real_map_set_command(line->script_tick, line->script);
	real_map_set_core_save(line->core_save_tick);
	save_image_scenario_set_test(line->image_test_tick);
	progress_logging = reference;
	real_map_set_canonical_every_tick(line->canonical_every_tick);
	if (!real_map_begin(reference ? NULL : reference_raw_digests, line->ticks))
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
	if (trace_count)
		trace_flush();
	if (reference)
	{
		memcpy(reference_digests, real_map_tick_digests(), sizeof(unsigned long) * (size_t)line->ticks);
		memcpy(reference_raw_digests, real_map_tick_digests_raw(), sizeof(unsigned long) * (size_t)line->ticks);
	}
	wii_log("RUN map=%s cycle=%d cadence=%s ticks=%ld frames=%ld frames_without_ticks=%ld most_ticks_in_a_frame=%ld "
		"digest=%08lx chain=%08lx at1=%08lx at30=%08lx at150=%08lx atend=%08lx "
		"sim=%08lx sim_chain=%08lx sim_at1=%08lx sim_at30=%08lx sim_at150=%08lx sim_atend=%08lx first_divergent_tick=%ld "
		"seed=%08lx objects=%ld player_unit=%08lx spawn_tick=%ld position=%.6f,%.6f,%.6f tags_end=%08lx "
		"bsp=%d bsp_switches=%ld command_tick=%ld command_result=%ld requests_left=%ld sim_raw=%08lx "
		"sim_raw_chain=%08lx raw=%08lx raw_chain=%08lx sim_hwi015b=%08lx sim_hwi015b_raw=%08lx "
		"sim_hwi015b_raw_chain=%08lx frame_effects=%ld frame_sounds=%ld requests=%lx request_tick=%ld "
		"ticks_after_request=%ld clock_ticks=%ld ticks_run=%ld carried=%ld frames_stopped=%ld carried_total=%ld "
		"most_carried=%ld queue_resets=%ld core_saved_tick=%ld ms=%lu\n",
		line->name, cycle, cadence_names[cadence], result->ticks, result->frames, result->frames_without_ticks,
		result->most_ticks_in_a_frame, result->digest, result->chain, result->digest_at[0], result->digest_at[1],
		result->digest_at[2], result->digest_at[3], result->simulation_digest, result->simulation_chain,
		result->simulation_at[0], result->simulation_at[1], result->simulation_at[2], result->simulation_at[3],
		result->first_divergent_tick, result->random_seed,
		result->objects, (unsigned long)result->player_unit, result->player_spawn_tick,
		(double)result->player_position[0], (double)result->player_position[1], (double)result->player_position[2],
		result->tags_at_end, result->structure_bsp_index, result->bsp_switches, result->command_tick,
		result->command_result, result->requests_left, result->simulation_raw_digest,
		result->simulation_raw_chain, result->raw_digest, result->raw_chain, result->simulation_hwi015b_digest,
		result->simulation_hwi015b_raw_digest, result->simulation_hwi015b_raw_chain, result->frame_made_effects,
		result->frame_made_looping_sounds, result->requests_seen, result->request_tick, result->ticks_after_request,
		result->clock_ticks, result->ticks_run, result->ticks_carried, result->frames_stopped,
		result->ticks_carried_total, result->most_ticks_carried, result->queue_resets, result->core_saved_tick,
		*milliseconds);
	if (line->more_switches)
	{
		wii_log("RUN_SWITCHES map=%s cycle=%d cadence=%s asked=%d ran=%ld bsp_after_last=%d bsp_at_end=%d "
			"bsp_switches=%ld\n", line->name, cycle, cadence_names[cadence], line->more_switches,
			result->more_commands_ran, result->more_commands_bsp_after, result->structure_bsp_index,
			result->bsp_switches);
	}
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
		"expect_error=%s script_tick=%ld core_save=%ld\n", line->name, line->scenario_path, line->ticks, line->cycles,
		line->cadences, line->warmup, line->has_expect ? line->expect : 0UL, line->switch_tick, line->switch_index,
		line->expected_error > 0 ? wii_cache_error_name(line->expected_error) : "none", line->script_tick,
		line->core_save_tick);

	/* a warm-up run first, not measured: a map leaves state behind for the
	next (a light's stale field, the game sound's first-map flag), so every
	measured run follows the same 30 Hz run */
	if (line->warmup)
	{
		struct real_map_result warm;
		unsigned long milliseconds = 0;
		/* (warmup=1: the line's ticks; warmup=N, N>1: N ticks, where the
		map's own state, not the run's length, is what the next run follows) */
		struct map_line warm_line = *line;
		int ran;

		if (line->warmup > 1)
			warm_line.ticks = line->warmup < REAL_MAP_MAXIMUM_TICKS ? line->warmup : REAL_MAP_MAXIMUM_TICKS;
		warm_line.switch_tick = 0;
		warm_line.script_tick = 0;
		warm_line.core_save_tick = 0;
		warm_line.image_test_tick = 0;
		ran = real_map_load(line->scenario_path) &&
			run_cadence(config, &warm_line, -1, _cadence_30, 0, &warm, &milliseconds);

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
		for (int order = 0; order < ENGINE_MAP_RUN_CADENCES; order++)
		{
			int cadence = (int)cadence_order[order];
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
					wii_log("ALLOC map=%s cycle=%d cadence=%s index=%ld name=\"%s\" offset=%lu bytes=%lu at1=%08lx at30=%08lx at150=%08lx atend=%08lx "
						"raw1=%08lx raw30=%08lx raw150=%08lx rawend=%08lx\n",
						line->name, cycle, cadence_names[cadence], index, real_map_allocation_name(index),
						real_map_allocation_offset(index), real_map_allocation_bytes(index),
						real_map_allocation_digest(index, 0), real_map_allocation_digest(index, 1),
						real_map_allocation_digest(index, 2), real_map_allocation_digest(index, 3),
						real_map_allocation_raw_digest(index, 0), real_map_allocation_raw_digest(index, 1),
						real_map_allocation_raw_digest(index, 2), real_map_allocation_raw_digest(index, 3));
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
				/* every tick's simulation digest, ten a line (the canonical
				one when it was taken every tick) */
				for (long tick = 0; line->canonical_every_tick && tick < line->ticks; tick += 10)
				{
					char text[16 + 10 * 9];
					int used = 0;

					for (long index = tick; index < tick + 10 && index < line->ticks; index++)
						used += snprintf(text + used, sizeof(text) - (size_t)used, " %08lx", reference_digests[index]);
					wii_log("TICKS map=%s from=%ld%s\n", line->name, tick + 1, text);
				}
				for (long tick = 0; tick < line->ticks; tick += 10)
				{
					char text[16 + 10 * 9];
					int used = 0;

					for (long index = tick; index < tick + 10 && index < line->ticks; index++)
						used += snprintf(text + used, sizeof(text) - (size_t)used, " %08lx", reference_raw_digests[index]);
					wii_log("TICKS_RAW map=%s from=%ld%s\n", line->name, tick + 1, text);
				}
			}
			/* the simulation the same at every cadence and in every cycle; the
			whole state the same as this cadence's in the first cycle */
			if (cycle == 0 && ran)
				cadence_first[cadence] = result;
			same = ran && have_first && result.simulation_raw_digest == first.simulation_raw_digest &&
				result.simulation_raw_chain == first.simulation_raw_chain && result.first_divergent_tick == -1 &&
				result.random_seed == first.random_seed && result.objects == first.objects &&
				result.tags_at_start == first.tags_at_start && result.tags_at_end == first.tags_at_end &&
				result.structure_bsp_index == first.structure_bsp_index;
			whole_same = ran && result.raw_digest == cadence_first[cadence].raw_digest &&
				result.raw_chain == cadence_first[cadence].raw_chain;
			/* (invariance=0: an observation, reported, not a failure) */
			if (!same)
			{
				identical = 0;
				report->identical = 0;
				if (line->invariance)
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
				int pass = kind == line->expected_error && ran && result.command_tick == line->switch_tick &&
					result.command_bsp_after == result.command_bsp_before &&
					result.command_bsp_after != line->switch_index;

				report->expectations++;
				report->expectations_met += pass;
				if (!pass)
					report->failures++;
				wii_log("BSP_FAILURE map=%s cycle=%d cadence=%s expected=%s kind=%s asked=%d bsp_before=%d bsp_after=%d "
					"bsp_at_end=%d pass=%d text=\"%s\"\n", line->name, cycle, cadence_names[cadence],
					wii_cache_error_name(line->expected_error), wii_cache_error_name(kind), line->switch_index,
					result.command_bsp_before, result.command_bsp_after, result.structure_bsp_index, pass, text);
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
		int pass = have_first && first.simulation_hwi015b_digest == line->expect;

		report->expectations++;
		report->expectations_met += pass;
		if (!pass)
			report->failures++;
		wii_log("EXPECT map=%s simulation_hwi015b=%08lx expected=%08lx pass=%d\n", line->name,
			first.simulation_hwi015b_digest, line->expect, pass);
	}
	if (line->has_expect_simulation)
	{
		int pass = have_first && first.simulation_digest == line->expect_simulation;

		report->expectations++;
		report->expectations_met += pass;
		if (!pass)
			report->failures++;
		wii_log("EXPECT_SIM map=%s simulation=%08lx expected=%08lx pass=%d\n", line->name, first.simulation_digest,
			line->expect_simulation, pass);
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
	save_image_scenario_set_observer(image_observer);

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
