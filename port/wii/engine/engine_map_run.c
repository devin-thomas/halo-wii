/*
ENGINE_MAP_RUN.C

The real-map run shared by the Wii driver and the i686 host reference
(engine_map_run.h, HWI-015B). Platform side: C types and the Wii platform
layer's log and clock only; the engine is reached through
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

static unsigned long reference_digests[REAL_MAP_TICKS];

/* the frame times of a run paced by the vertical retrace, logged so the host
can replay them (VSYNC_DT lines) */
#define VSYNC_MAXIMUM_FRAMES 2400
static float vsync_frames[VSYNC_MAXIMUM_FRAMES];
static long vsync_frame_count;

/* ---------- allocation dumps (engine_map_run_set_dump) */

#define DUMP_MAXIMUM 8

static long dump_indices[DUMP_MAXIMUM];
static int dump_count;
static long dump_tick = -1;
static long dump_run;
static char dump_directory[128];

static void dump_observer(long tick)
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
	real_map_set_tick_observer(dump_observer);
	return dump_count > 0 && dump_tick > 0;
}

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

static int run_cadence(const struct engine_map_run_config *config, int cycle, enum cadence cadence, int reference,
	struct real_map_result *result, unsigned long *milliseconds)
{
	unsigned long state = 12345;
	long frame = 0;
	unsigned long long started;

	if (!real_map_begin(reference ? NULL : reference_digests))
		return 0;
	vsync_frame_count = 0;
	if (cadence == _cadence_vsync && config->vsync_begin)
		config->vsync_begin(config->context);
	started = wii_time_microseconds();
	while (frame < 200000 && real_map_frame(cadence_dt(config, cadence, frame, &state)))
		frame++;
	*milliseconds = (unsigned long)((wii_time_microseconds() - started) / 1000ULL);
	real_map_end(result);
	if (reference)
		memcpy(reference_digests, real_map_tick_digests(), sizeof(reference_digests));
	wii_log("RUN cycle=%d cadence=%s ticks=%ld frames=%ld frames_without_ticks=%ld most_ticks_in_a_frame=%ld "
		"digest=%08lx chain=%08lx at1=%08lx at30=%08lx at150=%08lx at300=%08lx "
		"sim=%08lx sim_chain=%08lx sim_at1=%08lx sim_at30=%08lx sim_at150=%08lx sim_at300=%08lx first_divergent_tick=%ld "
		"seed=%08lx objects=%ld player_unit=%08lx spawn_tick=%ld position=%.6f,%.6f,%.6f tags_end=%08lx ms=%lu\n",
		cycle, cadence_names[cadence], result->ticks, result->frames, result->frames_without_ticks,
		result->most_ticks_in_a_frame, result->digest, result->chain, result->digest_at[0], result->digest_at[1],
		result->digest_at[2], result->digest_at[3], result->simulation_digest, result->simulation_chain,
		result->simulation_at[0], result->simulation_at[1], result->simulation_at[2], result->simulation_at[3],
		result->first_divergent_tick, result->random_seed,
		result->objects, (unsigned long)result->player_unit, result->player_spawn_tick,
		(double)result->player_position[0], (double)result->player_position[1], (double)result->player_position[2],
		result->tags_at_end, *milliseconds);
	if (cadence == _cadence_vsync)
	{
		/* the frame times, bit for bit, ten a line */
		for (long frame = 0; frame < vsync_frame_count; frame += 10)
		{
			char line[16 + 10 * 9];
			int used = 0;

			for (long index = frame; index < frame + 10 && index < vsync_frame_count; index++)
			{
				unsigned long bits;

				memcpy(&bits, &vsync_frames[index], sizeof(bits));
				used += snprintf(line + used, sizeof(line) - (size_t)used, " %08lx", bits);
			}
			wii_log("VSYNC_DT cycle=%d from=%ld%s\n", cycle, frame, line);
		}
	}
	return result->ticks == REAL_MAP_TICKS;
}

int engine_map_run(
	const struct engine_map_run_config *config,
	struct engine_map_run_report *report)
{
	struct real_map_result first;
	struct real_map_result cadence_first[ENGINE_MAP_RUN_CADENCES];
	long first_heap = 0;
	int have_first = 0;

	memset(report, 0, sizeof(*report));
	memset(&first, 0, sizeof(first));
	report->identical = 1;
	report->whole_identical = 1;
	memset(cadence_first, 0, sizeof(cadence_first));
	report->heap_flat = 1;

	report->fixed_step_pass = run_fixed_step();
	if (!report->fixed_step_pass)
		report->failures++;

	/* a warm-up run first, not measured: a map leaves state behind for the
	next (a light's stale field, the game sound's first-map flag), so every
	measured run follows the same 300-tick 30 Hz run */
	{
		struct real_map_result warm;
		unsigned long milliseconds = 0;
		int ran = real_map_load(config->scenario_path) &&
			run_cadence(config, -1, _cadence_30, 0, &warm, &milliseconds);

		real_map_unload();
		wii_log("WARMUP ran=%d\n", ran);
		if (!ran)
		{
			report->failures++;
			return 0;
		}
	}
	for (int cycle = 0; cycle < config->cycles; cycle++)
	{
		long heap;

		/* every run loads the map, as a new game does, and unloads it */
		for (int cadence = 0; cadence < ENGINE_MAP_RUN_CADENCES; cadence++)
		{
			struct wii_cache_load_report load;
			struct real_map_result result;
			unsigned long milliseconds = 0;
			int reference = !have_first;
			unsigned long long started = wii_time_microseconds();
			int loaded = real_map_load(config->scenario_path);
			unsigned long load_milliseconds = (unsigned long)((wii_time_microseconds() - started) / 1000ULL);
			int ran;
			int same;
			int whole_same;

			wii_cache_load_report_get(&load);
			wii_log("LOAD cycle=%d cadence=%s scenario=%s loaded=%d ms=%lu segments=%ld reads=%ld bytes=%lu "
				"relocations=%lu unstaged_reads=%lu cache_ms=%lu unsupported=%lu\n", cycle, cadence_names[cadence],
				config->scenario_path, loaded, load_milliseconds, load.segments, load.reads, load.bytes,
				load.relocations, load.unstaged_reads, load.milliseconds, wii_unsupported_names());
			if (!loaded)
			{
				report->failures++;
				wii_log("FAIL map_load cycle=%d\n", cycle);
				return 0;
			}
			if (reference)
			{
				report->map_loaded = 1;
				report->load_milliseconds = load_milliseconds;
			}
			ran = run_cadence(config, cycle, (enum cadence)cadence, reference, &result, &milliseconds);
			report->runs++;
			if (ran)
			{
				for (long index = 0; index < real_map_allocation_count(); index++)
				{
					wii_log("ALLOC cycle=%d cadence=%s index=%ld name=\"%s\" offset=%lu bytes=%lu at1=%08lx at30=%08lx at150=%08lx at300=%08lx\n",
						cycle, cadence_names[cadence], index, real_map_allocation_name(index), real_map_allocation_offset(index),
						real_map_allocation_bytes(index), real_map_allocation_digest(index, 0),
						real_map_allocation_digest(index, 1), real_map_allocation_digest(index, 2),
						real_map_allocation_digest(index, 3));
				}
			}
			if (reference && ran)
			{
				first = result;
				have_first = 1;
				report->digest = result.digest;
				report->chain = result.chain;
				report->simulation_digest = result.simulation_digest;
				report->simulation_chain = result.simulation_chain;
				report->tags_at_start = result.tags_at_start;
				report->player_spawn_tick = result.player_spawn_tick;
				report->run_milliseconds = milliseconds;
				/* every tick's simulation digest, ten a line */
				for (long tick = 0; tick < REAL_MAP_TICKS; tick += 10)
				{
					char line[16 + 10 * 9];
					int used = 0;

					for (long index = tick; index < tick + 10 && index < REAL_MAP_TICKS; index++)
						used += snprintf(line + used, sizeof(line) - (size_t)used, " %08lx", reference_digests[index]);
					wii_log("TICKS from=%ld%s\n", tick + 1, line);
				}
			}
			/* the simulation the same at every cadence and in every cycle; the
			whole state the same as this cadence's in the first cycle */
			if (cycle == 0 && ran)
				cadence_first[cadence] = result;
			same = ran && have_first && result.simulation_digest == first.simulation_digest &&
				result.simulation_chain == first.simulation_chain && result.first_divergent_tick == -1 &&
				result.random_seed == first.random_seed && result.objects == first.objects &&
				result.tags_at_start == first.tags_at_start && result.tags_at_end == first.tags_at_end;
			whole_same = ran && result.digest == cadence_first[cadence].digest &&
				result.chain == cadence_first[cadence].chain;
			if (!same)
			{
				report->identical = 0;
				report->failures++;
			}
			/* (an observation, not a failure: the first-person view model's idle
			pose timer draws on the local random numbers, which per-frame
			presentation also draws on, as the cycle before left it) */
			if (!whole_same)
				report->whole_identical = 0;
			wii_log("RUN_SAME cycle=%d cadence=%s simulation_same=%d whole_same_as_cycle0=%d\n", cycle,
				cadence_names[cadence], same, whole_same);
			real_map_unload();
		}
		heap = config->heap_in_use ? config->heap_in_use(config->context) : 0;
		if (cycle == 0)
			first_heap = heap;
		else if (heap != first_heap)
			report->heap_flat = 0;
		wii_log("CYCLE %d heap_in_use=%ld unsupported_names=%lu unsupported_calls=%lu\n", cycle, heap,
			wii_unsupported_names(), wii_unsupported_calls());
	}
	if (!report->heap_flat)
		report->failures++;
	wii_log("MAP_RUNS runs=%d simulation_identical=%d whole_identical_per_cadence=%d heap_flat=%d digest=%08lx "
		"chain=%08lx sim=%08lx sim_chain=%08lx tags_start=%08lx spawn_tick=%ld\n",
		report->runs, report->identical, report->whole_identical, report->heap_flat, report->digest, report->chain,
		report->simulation_digest, report->simulation_chain, report->tags_at_start,
		report->player_spawn_tick);
	return report->failures == 0;
}
