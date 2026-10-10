/*
ENGINE_HOOKS.C

The engine-side half of the Wii engine driver (engine_hooks.h), and the
engine entry points the Wii build wraps (tools/wii/engine_build.py WRAPPED):

- halt_and_catch_fire: upstream draws the error with the rasterizer forever;
  the Wii build has no rasterizer, so the halt is reported to the SD log and
  the program exits (wii_driver_halt).
- game_tick, game_frame and update_client_get_maximum_possible_server_time:
  the engine's own while no fixed-step scenario is armed; the scenario's
  tick, frame and scripted input source while one is
  (fixed_step_scenario.h). After the engine's own game_tick, a real map's
  run digests the game state (real_map_scenario.h, HWI-015B).
- update_client_handle_server_update: a real map's run puts its scripted
  input in each tick's update as the update is built.

Compiled as an engine unit.
*/

#include "cseries.h"
#include "cseries_windows.h"
#include "math/real_math.h"
#include "game/game.h"
#include "saved games/game_state.h"
#include "cache/physical_memory_map.h"
#include "ai/ai_debug.h"

#include "engine_hooks.h"
#include "fixed_step_scenario.h"
#include "real_map_scenario.h"
#include "wii_platform.h"

/* the engine's (source/shell, source/main, source/cseries ...) */
boolean shell_platform_initialize(void);
void shell_platform_verify(void);
void cseries_initialize(void);
void errors_initialize(void);
void real_math_initialize(void);
void game_state_initialize(void);
void console_initialize(void);
void game_initialize(void);
void game_dispose(void);
void real_math_dispose(void);
void tag_files_open(void);
void tag_files_close(void);
void errors_dispose(void);
void shell_platform_dispose(void);
void cseries_dispose(void);
char *error_get(void);

/* source/shell/shell.c shell_initialize, step by step, without
rasterizer_initialize, which is reported unsupported: without a Direct3D
device it halts the engine (below). tag_files_open is the engine's: its
cache layer on the Wii is cache_files_wii.c (HWI-015B), which reads maps
staged on the SD card in the PowerPC's byte order, in place of the Xbox's
hard-disk cache (six cache files on z:\, about 0.9 GB, which the
unmodified layer fails to create on the card: the full-game mode shows
it). */
static boolean shell_initialized_steps;
static boolean tag_files_opened;

int wii_engine_shell_initialize(
	void)
{
	boolean rasterizer;

	physical_memory_allocate();
	cseries_initialize();
	if (!shell_platform_initialize())
	{
		wii_log("STAGE shell shell_platform_initialize=0\n");
		return FALSE;
	}
	errors_initialize();
	tag_files_open();
	tag_files_opened = TRUE;
	real_math_initialize();
	game_state_initialize();
	/* rasterizer_initialize without a Direct3D device halts the engine
	(rasterizer_xbox.c: its default bitmaps assert global_d3d_device), which
	the full-game mode shows; the Wii renderer is HWI-016/HWI-032 work */
	wii_unsupported("render", "rasterizer_initialize (no Direct3D 8 device; halts the engine)");
	rasterizer = FALSE;
	shell_platform_verify();
	shell_initialized_steps = TRUE;
	return rasterizer;
}

int wii_engine_game_initialize(
	void)
{
	int allocated;

	console_initialize();
	/* the game state's allocations, for the real map's digests */
	real_map_record_allocations(TRUE);
	game_initialize();
	allocated = fixed_step_scenario_allocate();
	real_map_record_allocations(FALSE);
	return allocated;
}

void wii_engine_shutdown(
	void)
{
	game_dispose();
	/* shell_dispose without sound, input, the rasterizer and the tag files,
	which did not initialize */
	if (shell_initialized_steps)
	{
		real_math_dispose();
		if (tag_files_opened)
		{
			tag_files_close();
			tag_files_opened = FALSE;
		}
		errors_dispose();
		shell_platform_dispose();
		cseries_dispose();
		shell_initialized_steps = FALSE;
	}
	physical_memory_free();
}

void wii_engine_report_warning(
	const char *text)
{
	display_assert((char *)text, __FILE__, __LINE__, FALSE);
}

const char *wii_engine_error_text(
	void)
{
	return error_get();
}

void wii_engine_fatal_assert(
	void)
{
	match_vassert(__FILE__, __LINE__, FALSE, "HWI-015 deliberate fatal assertion (halt path check)");
}

/* HWI-015D: what the engine compiled of its diagnostic storage, and
whether ai_debug_initialize ran: it sets the debug selection to none, which
player spawning reads (players.c player_get_starting_location_count) */
#ifndef HALO_PROFILE_FRAME_HISTORY
#define HALO_PROFILE_FRAME_HISTORY 1
#endif

int wii_engine_report_diagnostic_storage(
	void)
{
	int initialized = ai_debug.selected_squad_index==NONE && ai_debug.selected_actor_index==NONE && ai_debug.render;
	int ok = HALO_AI_DEBUG_RECORDS ? TRUE :
		initialized && actor_debug_array!=NULL && actor_path_debug_array==NULL;

	wii_log("STAGE diagnostic_storage ai_debug_records=%d profile_frame_history=%d ai_debug_bytes=%lu "
		"actor_debug_records=%s path_debug_storage=%s ai_debug_initialized=%d selected_squad=%ld "
		"selected_actor=%ld result=%s\n",
		HALO_AI_DEBUG_RECORDS, HALO_PROFILE_FRAME_HISTORY, (unsigned long)sizeof(ai_debug),
		actor_debug_array==NULL ? "none" : HALO_AI_DEBUG_RECORDS ? "per_actor" : "one_shared",
		actor_path_debug_array==NULL ? "none" : "allocated", initialized,
		(long)ai_debug.selected_squad_index, (long)ai_debug.selected_actor_index, ok ? "pass" : "fail");

	return ok;
}

unsigned long wii_engine_game_state_used(
	void)
{
	/* a zero-byte allocation is placed where the next would be */
	byte *next = game_state_malloc("hwi015 game state probe", NULL, 0);

	return (unsigned long)(next - (byte *)physical_memory_get_game_state_base_address());
}

/* ---------- wrapped engine entry points */

void __real_halt_and_catch_fire(void);
void __real_game_tick(void);
void __real_game_frame(real dt);
long __real_update_client_get_maximum_possible_server_time(void);

void __wrap_halt_and_catch_fire(
	void)
{
	wii_driver_halt(error_get());
}

void __wrap_game_tick(
	void)
{
	if (fixed_step_scenario_armed())
	{
		fixed_step_scenario_tick();
	}
	else
	{
		__real_game_tick();
		real_map_after_tick();
	}
}

void __wrap_game_frame(
	real dt)
{
	if (fixed_step_scenario_armed())
		fixed_step_scenario_game_frame(dt);
	else
		__real_game_frame(dt);
}

/* A local game takes its players' input into their queues once a frame
(player_queues_new.c), one queue per player; the scenario has no player
(a player needs a map), so its input is its script, read by its tick. */
void __real_update_client_local_ticks(short ticks);

void __wrap_update_client_local_ticks(
	short ticks)
{
	if (!fixed_step_scenario_armed())
		__real_update_client_local_ticks(ticks);
}

/* each tick's update, as update_server_next_update builds it: a real map's
run puts its scripted input in it first (the update's number is its tick's) */
struct server_update;
void __real_update_client_handle_server_update(struct server_update *update, long update_number);

void __wrap_update_client_handle_server_update(
	struct server_update *update,
	long update_number)
{
	real_map_scripted_actions(update, update_number);
	__real_update_client_handle_server_update(update, update_number);
}

/* a local game ticks only as far as its players' input reaches; the
scenario's script has every tick's */
long __wrap_update_client_get_maximum_possible_server_time(
	void)
{
	if (fixed_step_scenario_armed())
		return LONG_MAX;
	return __real_update_client_get_maximum_possible_server_time();
}

/* ---------- the rasterizer's halves of game_initialize and game_dispose

decals_initialize/decals_dispose (source/effects/decals.c) end in the
rasterizer's, which create and release Direct3D vertex buffers and assert
the device. The Wii build has no Direct3D device: the rasterizer half is
reported unsupported and does nothing; the game-state half (the decal data
array and globals) is the engine's own. */

void __wrap_rasterizer_decals_initialize(
	void)
{
	wii_unsupported("render", "rasterizer_decals_initialize");
}

void __wrap_rasterizer_decals_dispose(
	void)
{
	wii_unsupported("render", "rasterizer_decals_dispose");
}

/* (per map, HWI-015B) its vertex cache's flush, which the decals' own
unlocking (game state: their locked and permanent flags) comes first in;
that half is kept */
void decals_unlock(boolean permanent);

void __wrap__rasterizer_decals_dispose_from_old_map(
	void)
{
	wii_unsupported("render", "rasterizer_decals_dispose_from_old_map (vertex cache flush)");
	decals_unlock(TRUE);
}

/* ---------- the texture cache (HWI-015B)

The Xbox texture cache (source/cache/xbox_texture_cache.c) is made by
rasterizer_initialize (texture_cache_new), which the Wii build does not run:
it has no Direct3D device, and neither memory bank holds the Xbox texture
cache beside the required reservations (ADR-015 record; HWI-032 owns texture
residency). Opening and closing it with a map (scenario_tags_load,
scenario_tags_unload) would use its data array, which does not exist: they
are reported unsupported and do nothing. Bitmap pixels are not staged on the
card either (cache_files_wii.c). */

void __wrap_texture_cache_open(
	void)
{
	wii_unsupported("render", "texture_cache_open (no texture cache: no rasterizer)");
}

void __wrap_texture_cache_close(
	void)
{
	wii_unsupported("render", "texture_cache_close (no texture cache: no rasterizer)");
}

/* The sound cache likewise (source/cache/xbox_sound_cache.c) is made by
sound_initialize, which shell_initialize starts only after the rasterizer:
the Wii build has no DirectSound either, and sound samples are not staged. */

void __wrap_sound_cache_open(
	void)
{
	wii_unsupported("dsound", "sound_cache_open (no sound cache: sound not started)");
}

void __wrap_sound_cache_close(
	void)
{
	wii_unsupported("dsound", "sound_cache_close (no sound cache: sound not started)");
}

/* What objects predict they will draw and play (source/cache/
predicted_resources.c, from the game's tick: a weapon readied, an object
made) is precached into those two caches: it loads resources and changes no
game state. With neither cache, it is reported unsupported and does nothing. */

struct tag_block;

void __wrap_predicted_resources_precache(
	struct tag_block *predicted_resources)
{
	wii_unsupported("render", "predicted_resources_precache (no texture or sound cache)");
}

/* ---------- the AI's debug records

ai_debug_initialize (source/ai/ai_debug.c) allocates the AI's debug records
on the heap: at the native builds' capacities (1024 actors, 32 paths) they
are 26,603,520 + 3,755,904 bytes on the Wii, more than both memory banks have
free. The Wii build leaves them out by default (HALO_AI_DEBUG_RECORDS 0,
HWI-015D: nothing in the game reads them), and ai_debug_initialize runs as
upstream's. This wrapper is linked only with configure.py
--wii-diagnostic-storage keep, which compiles the records: it then reports
them unsupported, and the AI cannot run. */

void __wrap_ai_debug_initialize(
	void)
{
	wii_unsupported("diagnostics", "ai_debug_initialize (AI debug records: 30,359,424 bytes)");
}
