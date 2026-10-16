/*
GAME_TIME.C

symbols in this file:
000A4D00 0020:
	_game_time_initialized (0000)
000A4D20 0030:
	_game_time_initialize (0000)
000A4D50 0050:
	_game_time_initialize_for_new_map (0000)
000A4DA0 0020:
	_game_time_dispose_from_old_map (0000)
000A4DC0 0010:
	_game_time_dispose (0000)
000A4DD0 0040:
	_game_time_end (0000)
000A4E10 0040:
	_game_time_get (0000)
000A4E50 0040:
	_game_time_get_elapsed (0000)
000A4E90 0040:
	_local_time_get (0000)
000A4ED0 0040:
	_local_time_get_elapsed (0000)
000A4F10 0040:
	_game_predicting (0000)
000A4F50 0050:
	_game_in_progress (0000)
000A4FA0 0040:
	_game_time_get_paused (0000)
000A4FE0 0050:
	_game_time_set_paused (0000)
000A5030 0040:
	_game_time_get_speed (0000)
000A5070 0050:
	_game_time_set_speed (0000)
000A50C0 0010:
	_game_time_statistics_new (0000)
000A50D0 01e0:
	_game_time_statistics_frame (0000)
000A52B0 00e0:
	_game_time_start (0000)
000A5390 0360:
	_game_time_update (0000)
0025C480 0012:
	??_C@_0BC@ENHAGKAA@game?5time?5globals?$AA@ (0000)
0025C494 0035:
	??_C@_0DF@KKMNPDPK@game_time_globals?5?$CG?$CG?5?$CBgame_time_@ (0000)
0025C4CC 0020:
	??_C@_0CA@INLHHDGN@c?3?2halo?2SOURCE?2game?2game_time?4c?$AA@ (0000)
0025C4EC 0012:
	??_C@_0BC@NOJBGBLH@game_time_globals?$AA@ (0000)
0025C500 0034:
	??_C@_0DE@PPGKJIJK@game_time_globals?5?$CG?$CG?5game_time_g@ (0000)
0025C534 001b:
	??_C@_0BL@DECMFGMD@?$CBgame_time_globals?9?$DOactive?$AA@ (0000)
0025C550 0021:
	??_C@_0CB@OIFBECDL@ticks_elapsed?5?$DM?$DN?5maximum_actions@ (0000)
0025C578 004c:
	??_C@_0EM@FOLKNDLG@game_time_globals?9?$DOleftover_dt?$DO?$DN@ (0000)
0025C5C8 004c:
	??_C@_0EM@FBBKPKLD@update?5server?5is?5too?5far?5ahead?5o@ (0000)
0043EE04 002c:
	_bss_0043ee04 (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries_windows.h"
#include "real_math.h"
#include "game.h"
#include "main/main.h"
#include "player_queues_new.h"
#include "networking/network_client_manager.h"
#include "networking/network_game_globals.h"
#include "networking/network_server_manager.h"
#include "saved games/game_state.h"
#include "saved games/game_state_schema.h"
/* port/linux/game/network_distributed.c's */
void network_distributed_tick(void);

/* ---------- constants */

enum
{
	SOME_LARGE_NUMBER_OF_TICKS = 1000, // 0x03E8
};

/* ---------- macros */

/* ---------- structures */

struct game_time_globals_struct
{
	boolean initialized;
	boolean active;
	boolean paused;
	/* (these three unused: kept for the game state's layout) */
	short monitor_state;
	short monitor_counter;
	short monitor_latency;
	long local_time;
	short last_local_time_elapsed;
	long server_time;
	real speed;
	real leftover_dt;
};

/* port (HWI-085): its schema for the typed game-state image
(game_state_schema.h) */
static struct game_state_schema_field const game_time_globals_schema_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("initialized", _game_state_schema_boolean, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("active", _game_state_schema_boolean, 0x1, 1),
	GAME_STATE_SCHEMA_FIELD("paused", _game_state_schema_boolean, 0x2, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x3, 1),
	GAME_STATE_SCHEMA_FIELD("monitor_state", _game_state_schema_s16, 0x4, 1),
	GAME_STATE_SCHEMA_FIELD("monitor_counter", _game_state_schema_s16, 0x6, 1),
	GAME_STATE_SCHEMA_FIELD("monitor_latency", _game_state_schema_s16, 0x8, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0xA, 2),
	GAME_STATE_SCHEMA_FIELD("local_time", _game_state_schema_s32, 0xC, 1),
	GAME_STATE_SCHEMA_FIELD("last_local_time_elapsed", _game_state_schema_s16, 0x10, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x12, 2),
	GAME_STATE_SCHEMA_FIELD("server_time", _game_state_schema_s32, 0x14, 1),
	GAME_STATE_SCHEMA_FIELD("speed", _game_state_schema_real, 0x18, 1),
	GAME_STATE_SCHEMA_FIELD("leftover_dt", _game_state_schema_real, 0x1C, 1),
};
static struct game_state_schema const game_time_globals_schema =
	GAME_STATE_SCHEMA_DEFINE("game_time_globals", 0x20, game_time_globals_schema_fields);

typedef char game_time_globals_schema_check[
	sizeof(struct game_time_globals_struct) == 0x20 &&
	offsetof(struct game_time_globals_struct, paused) == 0x2 &&
	offsetof(struct game_time_globals_struct, monitor_state) == 0x4 &&
	offsetof(struct game_time_globals_struct, monitor_latency) == 0x8 &&
	offsetof(struct game_time_globals_struct, local_time) == 0xC &&
	offsetof(struct game_time_globals_struct, last_local_time_elapsed) == 0x10 &&
	offsetof(struct game_time_globals_struct, server_time) == 0x14 &&
	offsetof(struct game_time_globals_struct, speed) == 0x18 &&
	offsetof(struct game_time_globals_struct, leftover_dt) == 0x1C ? 1 : -1];

/* ---------- prototypes */

/* ---------- globals */

static struct game_time_globals_struct *game_time_globals;
/* port (HWI-034): the ticks a frame's clock gave that a revert-class request
kept from running in that frame (game_time_update), which the next frame
runs before its own. Not the game state's: a revert does not take them back,
a new clock (game_time_start, a new map) drops them. And, for the run
harnesses (game_time_port_tick_counts), the clock's ticks and the ticks run
since the clock started. */
static long game_time_ticks_carried;
static long game_time_clock_ticks;
static long game_time_ticks_run;

/* ---------- public code */

boolean game_time_initialized(
	void)
{
	return (game_time_globals && game_time_globals->initialized);
}

void game_time_initialize(
	void)
{
	game_time_globals = (struct game_time_globals_struct *)game_state_malloc("game time globals", NULL, sizeof(*game_time_globals));
	memset(game_time_globals, 0, sizeof(*game_time_globals));
	game_state_port_schema(game_time_globals, &game_time_globals_schema);

	return;
}

void game_time_initialize_for_new_map(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 131, game_time_globals && !game_time_globals->initialized);
	memset(game_time_globals, 0, sizeof(*game_time_globals));
	game_time_globals->initialized = TRUE;
	game_time_ticks_carried = 0; /* port (HWI-034) */

	return;
}

void game_time_dispose_from_old_map(
	void)
{
	if (game_time_globals)
	{
		game_time_globals->initialized = FALSE;
		game_time_globals->active = FALSE;
	}

	return;
}

void game_time_dispose(
	void)
{
	return;
}

void game_time_end(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 196, game_time_globals);
	game_time_globals->active = FALSE;

	return;
}

void game_time_set_distributed(
	long time)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 0, game_time_globals && game_time_globals->initialized);

	game_time_globals->local_time = time;
	game_time_globals->server_time = time;
	game_time_globals->leftover_dt = 0.f;

	return;
}

long game_time_get(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 463, game_time_globals && game_time_globals->initialized);

	return game_time_globals->local_time;
}

short game_time_get_elapsed(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 471, game_time_globals && game_time_globals->initialized);

	return game_time_globals->last_local_time_elapsed;
}

long local_time_get(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 479, game_time_globals && game_time_globals->initialized);

	return game_time_globals->local_time;
}

short local_time_get_elapsed(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 487, game_time_globals && game_time_globals->initialized);

	return game_time_globals->last_local_time_elapsed;
}

boolean game_predicting(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 495, game_time_globals && game_time_globals->initialized);

	return FALSE;
}

boolean game_in_progress(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 505, game_time_globals);

	if (game_time_globals->initialized)
	{
		if (game_time_globals->active)
		{
			return TRUE;
		}

		/* bug? */
		if (game_time_globals->paused)
		{
			return TRUE;
		}
		else
		{
			return FALSE;
		}
	}

	return FALSE;
}

boolean game_time_get_paused(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 533, game_time_globals);

	return game_time_globals->paused;
}

void game_time_set_paused(
	boolean paused)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 541, game_time_globals);

	if (game_time_globals->initialized)
	{
		game_time_globals->active = !paused;
	}

	game_time_globals->paused = paused;

	return;
}

/* how far the clock has run into the next tick, 0 to 1: the native ports
draw frames between ticks (port/linux/game/render_interpolation.c) */
real game_time_get_tick_fraction(
	void)
{
	real fraction;

	if (!game_time_globals || !game_time_globals->active || game_time_globals->paused)
		return 1.0f;
	fraction = game_time_globals->leftover_dt * game_time_globals->speed * TICKS_PER_SECOND;
	return PIN(fraction, 0.0f, 1.0f);
}

real game_time_get_speed(
	void)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 555, game_time_globals);

	/* port: a client of another's game runs at the host's speed (its own
	set before it joined too: cheats_network_client_enforce) */
	if (network_game_distributed_client())
		return 1.0f;
	return game_time_globals->speed;
}

void game_time_set_speed(
	real speed)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 562, game_time_globals);

	game_time_globals->speed = speed;

	return;
}

/* port: the game's own speed put back; whether it was another
(cheats_network_client_enforce) */
boolean game_time_reset_speed(
	void)
{
	boolean changed;

	if (!game_time_globals)
		return FALSE;
	changed = game_time_globals->speed != 1.0f;
	game_time_globals->speed = 1.0f;

	return changed;
}

/* whether a client's clock waits for the host's first game update, which
brings the host's time (the host ticks only once every machine has
loaded) */
boolean game_time_held(
	void)
{
	struct network_game_client *client;

	if (game_connection() != _game_connection_network_client)
		return FALSE;
	client = global_network_game_client_get();
	return client && !network_game_client_server_has_started_game(client);
}

void game_time_start(
	void)
{
	short connection;

	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 162, game_time_globals && game_time_globals->initialized);
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 163, !game_time_globals->active);

	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 562, game_time_globals);

	game_time_globals->speed = 1.f;
	game_time_globals->leftover_dt = 0;
	game_time_globals->active = TRUE;
	/* port (HWI-034) */
	game_time_ticks_carried = 0;
	game_time_clock_ticks = 0;
	game_time_ticks_run = 0;

	connection = game_connection();

	switch (connection)
	{
	case _game_connection_local:
	case _game_connection_network_server:
	{
		update_server_start();
		break;
	}
	case _game_connection_network_client:
	case _game_connection_film_playback:
	{
		update_client_start();
		break;
	}
	}

	return;
}

void game_time_update(
	real time_delta_sec)
{
	match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 205, game_time_globals);

	if (game_time_globals->active)
	{
		long connection;
		long ticks_elapsed;
		real ticks_per_second = game_time_globals->speed*TICKS_PER_SECOND;

		/* The native builds draw several frames per tick
		(port/linux/game/render_interpolation.c). A frame that runs no tick has
		elapsed no game time: without this, the ticks of the last frame that
		ran some would count again on every frame after it, and whatever
		advances by game_time_get_elapsed() once a frame (chapter titles, HUD
		messages and flashes, screen flashes, camera shake and impulses) would
		run as many times too fast as there are frames per tick. On the Xbox
		every frame ran at least one tick. */
		game_time_globals->last_local_time_elapsed = 0;

		if (ticks_per_second > 0.f)
		{
			boolean discard_leftover_time;
			real game_time;
			real ticks_elapsed_real;

			connection = game_connection();
			switch (connection)
			{
			case _game_connection_film_playback:
				connection = TICKS_PER_SECOND;
				discard_leftover_time = FALSE;
				goto calculate_elapsed_ticks;
			case _game_connection_network_client:
			case _game_connection_network_server:
				/* (the distributed netcode: every machine ticks on its own
				clock, and the host waits for nobody) */
				connection = TICKS_PER_SECOND;
				break;
			case _game_connection_local:
				connection = 7;
				break;
			default:
				connection = 7;
				break;
			}
			discard_leftover_time = TRUE;

		calculate_elapsed_ticks:
			game_time = time_delta_sec + game_time_globals->leftover_dt;
			ticks_elapsed_real = (real)floor(game_time*ticks_per_second);
			ticks_elapsed = (long)(ticks_elapsed_real <= (real)SOME_LARGE_NUMBER_OF_TICKS ?
				ticks_elapsed_real : (real)SOME_LARGE_NUMBER_OF_TICKS);
			if (ticks_elapsed > connection)
			{
				ticks_elapsed = connection;
				if (discard_leftover_time)
					game_time = ticks_elapsed_real/ticks_per_second;
			}

			game_time_globals->leftover_dt = game_time - ticks_elapsed_real/ticks_per_second;
			if (game_time_globals->leftover_dt < 0.f)
				game_time_globals->leftover_dt = 0.f;
			match_assert("c:\\halo\\SOURCE\\game\\game_time.c", 306,
				game_time_globals->leftover_dt>=0.f && game_time_globals->leftover_dt<100.f);

			/* port (HWI-034): or ticks held back by the last frame */
			if (ticks_elapsed > 0 || game_time_ticks_carried > 0)
			{
				long final_local_time;
				long maximum_possible_server_time;
				long carried = game_time_ticks_carried;
				long ticks_run = 0;

				game_time_ticks_carried = 0;
				game_time_clock_ticks += ticks_elapsed;
				final_local_time = game_time_globals->local_time + ticks_elapsed + carried;
				switch (game_connection())
				{
				case _game_connection_local:
				{
					/* port (HWI-034): the held-back ticks' updates are queued
					already, unless the request emptied the queue (a revert's
					or a core load's update_queues_reset_and_fill_with_lies,
					game_state.c): those are built again, from this frame's
					input, as the frame's own are */
					long rebuilt = 0;

					if (carried > 0)
					{
						long queued = update_client_get_maximum_possible_server_time() - game_time_globals->server_time;

						rebuilt = carried - PIN(queued, 0, carried);
					}
					if (ticks_elapsed + rebuilt > 0)
						update_client_local_ticks((short)(ticks_elapsed + rebuilt));
					break;
				}
				case _game_connection_network_server:
					if (ticks_elapsed > 0)
						network_game_server_update_ticks(global_network_game_server_get(), (short)ticks_elapsed);
					break;
				}

				/* (a client of the distributed netcode ticks on its own clock,
				with its own input and the latest the host relayed, from the
				host's first game update, which brings the host's time: the
				host ticks only once every machine has loaded) */
				if (game_connection() == _game_connection_network_client)
				{
					maximum_possible_server_time = game_time_held() ?
						game_time_globals->server_time : final_local_time;
				}
				else
					maximum_possible_server_time = update_client_get_maximum_possible_server_time();
				if (maximum_possible_server_time > game_time_globals->server_time)
				{
					long final_server_time = MIN(maximum_possible_server_time, final_local_time);
					long server_updates = final_server_time - game_time_globals->server_time;
					long update_index;
					/* port (HWI-034): the revert-class requests already waiting
					as a frame of several ticks begins (main.c) */
					unsigned long requests_before = server_updates > 1 ? main_port_revert_class_requests() : 0;

					for (update_index = 0; update_index < server_updates; update_index++)
					{
						/* port (HWI-016F): what main_loop does for the game once a
						frame, before each tick after a frame's first (main.c) */
						if (update_index > 0)
							main_port_game_requests_between_ticks();
						/* port (HWI-016F): the tick's own local random numbers (random_math.c) */
						local_random_tick_begin();
						game_tick();
						local_random_tick_end();
						render_interpolation_tick();
						game_time_globals->server_time++;
						game_time_globals->local_time++;
						/* the distributed netcode's per-tick state */
						network_distributed_tick();
						ticks_run++;
						/* port (HWI-034, option B, every port): a tick
						that asked for a revert, a loss's revert (once due), a
						core load, a won map or a map reset ends the frame's
						ticks there. At 30 Hz, a frame a tick as on the Xbox,
						main_loop handles it before the next tick; the ticks
						after it in a frame of several would otherwise run
						first, on the state the request replaces, and draw the
						tick random numbers (random_math.c) a revert does not
						restore. The ticks left are carried to the next frame,
						none lost and none run twice. A frame of one tick
						never stops here. */
						if (update_index + 1 < server_updates &&
							(main_port_revert_class_requests() & ~requests_before))
						{
							/* (as many as the clock gives at most, as above) */
							game_time_ticks_carried = MIN(server_updates - ticks_run, SOME_LARGE_NUMBER_OF_TICKS);
							break;
						}
					}
				}
				else
				{
					/* (no update to run: the ticks held back still wait) */
					game_time_ticks_carried = carried;
				}
				game_time_ticks_run += ticks_run;

				/* (none while a client's clock waits: a frame that runs no tick
				reports none); port (HWI-034): the ticks a frame ran, those it
				held back the next frame's */
				game_time_globals->last_local_time_elapsed = game_time_held() ? 0 :
					(short)(ticks_elapsed + carried - game_time_ticks_carried);
			}
		}

		/* The effects and the widgets that move every frame (game_frame) hang
		off objects: they read the pose between the last two ticks, as the
		renderer does, and the ticks above have taken their snapshots already
		(port/linux/game/render_interpolation.c). The frames of one tick share
		the fraction, so what they hang off is drawn where they put it. */
		render_interpolation_frame_begin();
		game_frame(game_time_get_speed()*time_delta_sec);
		render_interpolation_frame_end();
	}
	else
	{
		game_time_globals->last_local_time_elapsed = 0;
	}

	return;
}

/* port (HWI-034): game.h */
void game_time_port_tick_counts(
	long *clock_ticks,
	long *ticks_run,
	long *ticks_carried)
{
	*clock_ticks = game_time_clock_ticks;
	*ticks_run = game_time_ticks_run;
	*ticks_carried = game_time_ticks_carried;

	return;
}

/* ---------- private code */
