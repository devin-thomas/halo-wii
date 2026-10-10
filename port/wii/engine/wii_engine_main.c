/*
WII_ENGINE_MAIN.C

The Wii engine build's driver (HWI-015). libogc's start-up calls main; the
link wraps it (tools/wii/engine_build.py), so this __wrap_main runs, and
__real_main is the engine's own (source/shell/shell_xbox.c), which stays
linked: the image is the whole game's.

One launch:
1. Runtime start (FPSCR[NI] cleared, ADR-018), the memory map at entry, video
   and GX as the diagnostics configure them, the SD card, the launch counter
   and the log (WII_ENGINE_ROOT/engine.log).
2. The platform services through the SDK interfaces the engine calls
   (wii_platform_checks.c).
3. The engine's MEM2 arena (ADR-015), then the engine's own start-up:
   shell_initialize (which reports the unsupported rasterizer and fails, as
   it must), game_initialize, and an engine warning through its error
   report into d:\debug.txt.
4. Three load/run/unload cycles of the controlled fixed-step scenario
   (fixed_step_scenario.h), each at six render cadences: 60, 30, 20 and
   144 Hz, an irregular cadence with a half-second hitch, and frames paced
   by the video's vertical retrace with measured frame times. Every run must
   give the same state digests; the heap must not grow across cycles.
5. The engine's shut-down; the arena must be restored. The main stack's
   peak, the unsupported entry points called, and the result.
6. A deliberate fatal engine assertion: its halt is reported and the
   program exits (wii_driver_halt).
*/

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "build_id.h"
#include "wii_platform.h"
#include "engine_hooks.h"
#include "fixed_step_scenario.h"
#include "../runtime/runtime_start.h"

#define LOG_PATH WII_ENGINE_ROOT "/engine.log"
#define RUNS_PATH WII_ENGINE_ROOT "/runs.txt"
#define RESULT_PATH WII_ENGINE_ROOT "/result.txt"
#define RUNS_PREFIX "halo-wii-engine-v1 "
#define FIFO_BYTES (256u * 1024u)
#define GENERAL_BYTES (1024u * 1024u)
#define CYCLES 3
#define SCENARIO_SEED 0x48574931UL /* "HWI1" */
#define PAINT_BYTES (112u * 1024u)
#define PAINT ((uint32_t)0x5a17c0deu)
#define MAIN_STACK_BYTES 0x20000u

int wii_platform_checks(void);
int wii_platform_configure(void);
int __real_main(void);

extern u8 __Arena1Lo[], __Arena1Hi[], __Arena2Lo[], __Arena2Hi[];
extern u8 __bss_start[], __bss_end[], __sbss_start[];

static int failures;
static const char *stage = "start";
static int result_written;
static u64 boot_time;

static void check(int ok, const char *what)
{
	if (!ok)
	{
		failures++;
		wii_log("FAIL %s\n", what);
	}
}

/* ---------- main stack peak, from paint below the entry frame */

static uintptr_t paint_top, paint_bottom;

__attribute__((noinline)) static void paint_stack(uintptr_t entry)
{
	paint_top = (entry - 512) & ~(uintptr_t)3;
	paint_bottom = paint_top - PAINT_BYTES;
	for (volatile uint32_t *word = (uint32_t *)paint_bottom; (uintptr_t)word < paint_top; ++word)
		*word = PAINT;
}

static uintptr_t stack_low_water(void)
{
	const uint32_t *word = (const uint32_t *)paint_bottom;

	while ((uintptr_t)word < paint_top && *word == PAINT)
		++word;
	return (uintptr_t)word;
}

/* ---------- storage */

static int read_runs(unsigned *previous)
{
	char text[64] = "";
	FILE *file = fopen(RUNS_PATH, "rb");
	size_t used;
	char *stop;
	unsigned long count;

	*previous = 0;
	if (!file)
		return errno == ENOENT;
	used = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[used] = 0;
	if (strncmp(text, RUNS_PREFIX, sizeof(RUNS_PREFIX) - 1))
		return 0;
	errno = 0;
	count = strtoul(text + sizeof(RUNS_PREFIX) - 1, &stop, 10);
	if (errno || strcmp(stop, "\n"))
		return 0;
	*previous = (unsigned)count;
	return 1;
}

static int init_storage(unsigned *previous)
{
	unsigned readback;
	FILE *file;
	int written;

	if (!fatInitDefault())
		return 0;
	mkdir(WII_ENGINE_ROOT, 0777);
	mkdir(WII_ENGINE_DATA_ROOT, 0777);
	mkdir(WII_ENGINE_SAVE_ROOT, 0777);
	/* d:\maps\, where the engine looks for maps (cache_files_map_directory
	asserts it exists); the slice stages none, and the log says how many
	there are */
	mkdir(WII_ENGINE_DATA_ROOT "/maps", 0777);
	if (!read_runs(previous))
		return -1;
	file = fopen(RUNS_PATH, "w");
	if (!file)
		return -1;
	written = fprintf(file, RUNS_PREFIX "%u\n", *previous + 1);
	if (fclose(file) != 0 || written < 0 || !read_runs(&readback) || readback != *previous + 1)
		return -1;
	return wii_log_open(LOG_PATH) ? 1 : -1;
}

/* the previous launch's digest (result.txt), if any */
static int read_previous_result(unsigned long *digest, unsigned long *chain)
{
	FILE *file = fopen(RESULT_PATH, "rb");
	int fields;

	if (!file)
		return 0;
	fields = fscanf(file, "digest=%lx chain=%lx", digest, chain);
	fclose(file);
	return fields == 2;
}

static int write_result(unsigned long digest, unsigned long chain)
{
	FILE *file = fopen(RESULT_PATH, "w");
	int written;

	if (!file)
		return 0;
	written = fprintf(file, "digest=%08lx chain=%08lx build=%s\n", digest, chain, WII_BUILD_ID);
	return fclose(file) == 0 && written > 0;
}

/* ---------- the engine's own main */

static int full_game_requested(void)
{
	struct stat information;

	return fatInitDefault() && stat(WII_ENGINE_ROOT "/full_game.txt", &information) == 0;
}

static int run_full_game(void)
{
	struct wii_arena_report arena;
	int code;

	mkdir(WII_ENGINE_DATA_ROOT, 0777);
	mkdir(WII_ENGINE_DATA_ROOT "/maps", 0777);
	mkdir(WII_ENGINE_SAVE_ROOT, 0777);
	wii_log_open(LOG_PATH);
	wii_log("BEGIN target=engine mode=full_game build=%s\n", WII_BUILD_ID);
	stage = "full_game";
	wii_platform_configure();
	if (!wii_arena_open(0, GENERAL_BYTES, &arena))
		wii_log("FAIL arena_open\n");
	code = __real_main();
	wii_log("END target=engine mode=full_game main_returned=%d unsupported=%lu result=returned\n", code,
		wii_unsupported_names());
	wii_log_close();
	return code;
}

/* ---------- render cadences */

enum cadence
{
	_cadence_60,
	_cadence_30,
	_cadence_20,
	_cadence_144,
	_cadence_irregular,
	_cadence_vsync,
	NUMBER_OF_CADENCES
};

static const char *const cadence_names[NUMBER_OF_CADENCES] =
{
	"60hz", "30hz", "20hz", "144hz", "irregular", "vsync_measured"
};

static float cadence_dt(enum cadence cadence, long frame, unsigned long *state, u64 *last)
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
		u64 now;

		VIDEO_WaitVSync();
		now = gettime();
		{
			float dt = (float)ticks_to_microsecs(now - *last) / 1000000.0f;

			*last = now;
			return dt;
		}
	}
	}
}

static int run_cadence(enum cadence cadence, struct fixed_step_scenario_result *result)
{
	unsigned long state = 12345;
	u64 last;
	long frame = 0;

	stage = cadence_names[cadence];
	if (!fixed_step_scenario_begin(SCENARIO_SEED))
		return 0;
	if (cadence == _cadence_vsync)
	{
		VIDEO_WaitVSync();
	}
	last = gettime();
	while (frame < 100000 && fixed_step_scenario_frame(cadence_dt(cadence, frame, &state, &last)))
		frame++;
	fixed_step_scenario_end(result);
	return result->ticks == FIXED_STEP_SCENARIO_TICKS;
}

/* ---------- the halt path */

void wii_driver_halt(const char *error_text)
{
	int expected = !strcmp(stage, "halt_check");

	wii_log("HALT stage=%s expected=%d error=\"%s\"\n", stage, expected, error_text ? error_text : "");
	if (!result_written)
		wii_log("END target=engine build=%s failures=%d result=fail (halted in stage %s)\n", WII_BUILD_ID,
			failures + 1, stage);
	wii_log("EXIT via halt elapsed_ms=%" PRIu64 "\n", ticks_to_millisecs(gettime() - boot_time));
	wii_log_close();
	exit(expected ? 0 : 1);
}

/* ---------- main */

int __wrap_main(void)
{
	struct wii_runtime_start_record start;
	int ieee = wii_runtime_start(&start);
	uintptr_t entry_sp = (uintptr_t)__builtin_frame_address(0);
	uintptr_t a1lo = (uintptr_t)SYS_GetArena1Lo(), a1hi = (uintptr_t)SYS_GetArena1Hi();
	uintptr_t a2lo = (uintptr_t)SYS_GetArena2Lo(), a2hi = (uintptr_t)SYS_GetArena2Hi();
	struct mallinfo heap_entry = mallinfo();
	struct fixed_step_scenario_result reference, result;
	struct wii_arena_report arena;
	unsigned previous_runs = 0;
	int storage, cycle_heap[CYCLES], identical = 1, reference_set = 0;
	unsigned long previous_digest = 0, previous_chain = 0;
	int have_previous;
	GXRModeObj *mode;
	void *fifo, *xfb0, *xfb1;

	paint_stack(entry_sp);
	boot_time = gettime();
	/* With WII_ENGINE_ROOT/full_game.txt on the card, the engine's own main
	runs instead (source/shell/shell_xbox.c: start-up, then main_loop if the
	shell starts); on the Wii its shell reports the unsupported rasterizer
	and main returns. The call also keeps the whole game linked, so the
	image measured is the whole game's. */
	if (full_game_requested())
		return run_full_game();
	VIDEO_Init();
	mode = VIDEO_GetPreferredMode(NULL);
	fifo = memalign(32, FIFO_BYTES);
	xfb0 = SYS_AllocateFramebuffer(mode);
	xfb1 = SYS_AllocateFramebuffer(mode);
	if (!fifo || !xfb0 || !xfb1)
		return 1;
	memset(fifo, 0, FIFO_BYTES);
	DCFlushRange(fifo, FIFO_BYTES);
	VIDEO_Configure(mode);
	VIDEO_SetNextFramebuffer(MEM_K0_TO_K1(xfb0));
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	GX_Init(fifo, FIFO_BYTES);

	storage = init_storage(&previous_runs);
	if (storage != 1)
		return 1;
	wii_log("BEGIN target=engine build=%s previous_runs=%u ios=%d.%d\n", WII_BUILD_ID, previous_runs,
		IOS_GetVersion(), IOS_GetRevision());
	wii_log("FPSCR entry=%08" PRIx32 " engine=%08" PRIx32 " ieee=%d\n", start.fp_control_before,
		start.fp_control_after, ieee);
	check(ieee, "fpscr_ni");
	wii_log("MAP image bss=%p..%p sbss=%p arena1_link=%p..%p arena2_link=%p..%p\n", (void *)__bss_start,
		(void *)__bss_end, (void *)__sbss_start, (void *)__Arena1Lo, (void *)__Arena1Hi, (void *)__Arena2Lo,
		(void *)__Arena2Hi);
	wii_log("MAP entry arena1=%p..%p bytes=%lu arena2=%p..%p bytes=%lu heap_in_use=%d main_sp=%p main_stack=%u\n",
		(void *)a1lo, (void *)a1hi, (unsigned long)(a1hi - a1lo), (void *)a2lo, (void *)a2hi,
		(unsigned long)(a2hi - a2lo), heap_entry.uordblks, (void *)entry_sp, MAIN_STACK_BYTES);
	wii_log("MAP gx fifo=%p xfb0=%p xfb1=%p xfb_bytes_each=%u arena1_after_gx=%p..%p bytes=%lu heap_in_use=%d\n",
		fifo, xfb0, xfb1, VIDEO_GetFrameBufferSize(mode), SYS_GetArena1Lo(), SYS_GetArena1Hi(),
		(unsigned long)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo()), mallinfo().uordblks);
	have_previous = read_previous_result(&previous_digest, &previous_chain);
	wii_log("PERSIST previous_result=%d digest=%08lx chain=%08lx\n", have_previous, previous_digest,
		previous_chain);

	/* ---------- platform services */
	stage = "arena";
	if (!wii_arena_open((size_t)(previous_runs & 1) * 0x1000, GENERAL_BYTES, &arena))
	{
		wii_log("FAIL arena_open required=%lu\n", (unsigned long)arena.required);
		check(0, "arena_open");
	}
	wii_log("ARENA open offset=%u low=%p high_before=%p high_during=%p required=%lu spare=%lu state=%p+%lu "
		"tags=%p+%lu sound=%p+%lu general=%p+%lu\n", (previous_runs & 1) * 0x1000u, (void *)arena.arena_low_before,
		(void *)arena.arena_high_before, (void *)arena.arena_high_during, (unsigned long)arena.required,
		(unsigned long)arena.spare, (void *)arena.region_base[_wii_arena_game_state],
		(unsigned long)arena.region_size[_wii_arena_game_state], (void *)arena.region_base[_wii_arena_tag_cache],
		(unsigned long)arena.region_size[_wii_arena_tag_cache], (void *)arena.region_base[_wii_arena_sound_cache],
		(unsigned long)arena.region_size[_wii_arena_sound_cache], (void *)arena.region_base[_wii_arena_general],
		(unsigned long)arena.region_size[_wii_arena_general]);
	stage = "platform";
	check(wii_platform_configure(), "platform_configure");
	{
		int platform_failures = wii_platform_checks();

		wii_log("PLATFORM failures=%d\n", platform_failures);
		check(!platform_failures, "platform_checks");
	}

	/* ---------- the engine's start-up */
	{
		int shell, game;
		struct mallinfo before = mallinfo();

		stage = "shell_initialize";
		wii_log("STAGE shell_initialize begin\n");
		shell = wii_engine_shell_initialize();
		wii_log("STAGE shell_initialize result=%d (0 expected: rasterizer unsupported) error=\"%s\" unsupported=%lu\n",
			shell, wii_engine_error_text(), wii_unsupported_names());
		check(!shell, "shell_initialize_reports_rasterizer");
		stage = "game_initialize";
		wii_log("STAGE game_initialize begin\n");
		game = wii_engine_game_initialize();
		wii_log("STAGE game_initialize result=%d game_state_used=%lu heap_in_use=%d (before %d) unsupported=%lu\n",
			game, wii_engine_game_state_used(), mallinfo().uordblks, before.uordblks, wii_unsupported_names());
		check(game, "game_initialize");
		check(wii_engine_report_diagnostic_storage(), "diagnostic_storage");
		stage = "engine_report";
		wii_engine_report_warning("HWI-015 engine warning report check");
		/* the engine keeps d:\debug.txt open, and libfat writes a file's size
		when it is closed: the runner reads the file back after the launch */
		check(strstr(wii_engine_error_text(), "HWI-015 engine warning report check") != NULL,
			"engine_report_in_error_buffer");
		wii_log("STAGE engine_report error_buffer_has_warning=%d (d:\\debug.txt is read back by the host)\n",
			strstr(wii_engine_error_text(), "HWI-015 engine warning report check") != NULL);
		wii_log("MAP after_engine_start arena1_lo=%p arena2_lo=%p\n", SYS_GetArena1Lo(), SYS_GetArena2Lo());
	}

	/* ---------- load/run/unload cycles at varied render cadence */
	for (int cycle = 0; cycle < CYCLES; cycle++)
	{
		for (int cadence = 0; cadence < NUMBER_OF_CADENCES; cadence++)
		{
			int ran = run_cadence((enum cadence)cadence, &result);
			int same;

			if (!reference_set && ran)
			{
				reference = result;
				reference_set = 1;
			}
			same = ran && result.digest == reference.digest && result.chain == reference.chain &&
				!memcmp(result.digest_at, reference.digest_at, sizeof(result.digest_at)) &&
				result.random_seed == reference.random_seed && result.live_bodies == reference.live_bodies;
			identical &= same;
			wii_log("CADENCE cycle=%d cadence=%s ticks=%ld frames=%ld frames_without_ticks=%ld "
				"most_ticks_in_a_frame=%ld digest=%08lx chain=%08lx at1=%08lx at30=%08lx at150=%08lx at300=%08lx "
				"seed=%08lx bodies=%ld spawned=%ld deleted=%ld same=%d\n", cycle, cadence_names[cadence], result.ticks,
				result.frames, result.frames_without_ticks, result.most_ticks_in_a_frame, result.digest, result.chain,
				result.digest_at[0], result.digest_at[1], result.digest_at[2], result.digest_at[3], result.random_seed,
				result.live_bodies, result.spawned, result.deleted, same);
		}
		cycle_heap[cycle] = mallinfo().uordblks;
		wii_arena_report_get(&arena);
		/* (mallinfo counts the gap between the banks once the heap has moved
		into MEM2: compare it between cycles, and the arenas' low bounds, which
		the heap moves as it grows) */
		wii_log("CYCLE %d heap_in_use=%d arena1_lo=%p arena2_lo=%p general_in_use=%lu unsupported_names=%lu "
			"unsupported_calls=%lu\n", cycle, cycle_heap[cycle], SYS_GetArena1Lo(), SYS_GetArena2Lo(),
			(unsigned long)arena.general_in_use, wii_unsupported_names(), wii_unsupported_calls());
	}
	check(identical, "cadence_digests_identical");
	{
		int flat = 1;

		for (int cycle = 1; cycle < CYCLES; cycle++)
			flat &= cycle_heap[cycle] == cycle_heap[0];
		check(flat, "heap_flat_across_cycles");
		wii_log("CYCLES heap_flat=%d digest=%08lx chain=%08lx\n", flat, reference.digest, reference.chain);
	}
	if (have_previous)
		check(previous_digest == reference.digest && previous_chain == reference.chain, "digest_same_as_last_launch");
	check(write_result(reference.digest, reference.chain), "result_written");

	/* ---------- shut-down */
	{
		int restored;

		stage = "shutdown";
		wii_engine_shutdown();
		restored = wii_arena_close(&arena);
		wii_log("ARENA close restored=%d high_after=%p high_before=%p low_before=%p low_after=%p general_peak=%lu "
			"failed_allocations=%d heap_in_use=%d\n", restored, (void *)arena.arena_high_after,
			(void *)arena.arena_high_before, (void *)arena.arena_low_before, (void *)arena.arena_low_after,
			(unsigned long)arena.general_peak, arena.failed_allocations, mallinfo().uordblks);
		check(restored, "arena_restored");
	}

	{
		uintptr_t low_water = stack_low_water();

		wii_log("STACK entry_sp=%p painted=%u peak_below_entry=%lu window_exhausted=%d\n", (void *)entry_sp,
			PAINT_BYTES, (unsigned long)(paint_top - low_water + 512), low_water <= paint_bottom);
	}
	wii_log("UNSUPPORTED names=%lu calls=%lu\n", wii_unsupported_names(), wii_unsupported_calls());
	for (unsigned long index = 0; index < wii_unsupported_names(); index++)
		wii_log("UNSUPPORTED_NAME %s\n", wii_unsupported_name(index));
	wii_log("END target=engine build=%s cycles=%d cadences=%d failures=%d elapsed_ms=%" PRIu64 " result=%s\n",
		WII_BUILD_ID, CYCLES, NUMBER_OF_CADENCES, failures, ticks_to_millisecs(gettime() - boot_time),
		!failures && wii_log_healthy() ? "pass" : "fail");
	result_written = 1;

	/* ---------- the engine's halt path, last */
	stage = "halt_check";
	wii_log("HALT_CHECK begin\n");
	wii_engine_fatal_assert();
	/* not reached: the assertion halts */
	wii_log("FAIL halt_check returned\n");
	wii_log_close();
	return 1;
}

