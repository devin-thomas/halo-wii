/*
REAL_MAP_SCENARIO.H

A real map through the engine's own game tick (HWI-015B). The Wii driver
(wii_engine_main.c) and the i686 host reference (tools/wii/engine_map_host.c)
both run it, so their results compare tick by tick.

- Load: a map staged on the card (cache_files_wii.c) through the engine's
  own game_load, game_initialize_for_new_map and a local player, as
  main_new_map makes them.
- Run: the engine's own 30 Hz scheduler (game_time_update), whose game_tick
  and game_frame are the engine's. The local player's input is a script
  indexed by update (one update a tick), handed to the update server before
  each tick's update is built (real_map_local_ticks, in place of the local
  game's update_client_local_ticks), so every render cadence gives every
  tick the same input.
- Digest after every tick: the whole game state's live bytes, allocation by
  allocation, in a form that does not depend on byte order: each word that
  points into the game state or the tag slot is first replaced by its
  region and offset, then each aligned 8-byte group contributes the multiset
  of its bytes. A scalar of 2, 4 or 8 bytes keeps its bytes inside its own
  group whichever its byte order, so PowerPC and x86 give the same digest
  for the same values. A data array's live bytes are its header and its
  elements below its high-water count; the object pool's, its header and
  its blocks; any other allocation's, all of it. The tag slot (its runtime
  fields included) is digested the same way at load and at the end.
- Reset: the engine's own map reset (main_reset_map_private's steps) gives
  the next run the same start without reading the map again.
- Unload: game_dispose_from_old_map and game_unload.

Only C types here: the drivers include it beside libogc or the host's C
library.
*/

#ifndef __HALO_WII_REAL_MAP_SCENARIO_H
#define __HALO_WII_REAL_MAP_SCENARIO_H

#define REAL_MAP_TICKS 300            /* a run's ticks unless its map says (HWI-015B's) */
#define REAL_MAP_MAXIMUM_TICKS 9000   /* five minutes of game time */
#define REAL_MAP_CHECKPOINTS 4
#define REAL_MAP_MAXIMUM_ALLOCATIONS 160

struct real_map_result
{
	long ticks;
	long frames;
	long frames_without_ticks;
	long most_ticks_in_a_frame;
	unsigned long digest;                        /* the whole game state after the last tick */
	unsigned long chain;                         /* CRC of every tick's digest, in order */
	unsigned long digest_at[REAL_MAP_CHECKPOINTS]; /* after ticks 1, 30, 150 and the run's last */
	/* the same without the allocations the render frames reach (the
	scheduler's frame-time remainder, the game sound's frame update, the
	first-person view model: real_map_scenario.c) */
	unsigned long simulation_digest;
	unsigned long simulation_chain;
	unsigned long simulation_at[REAL_MAP_CHECKPOINTS];
	/* the simulation's raw digest (no pointer made canonical: real_map_scenario.c),
	for the Wii and the host of one launch */
	unsigned long simulation_raw_digest;
	unsigned long simulation_raw_chain;
	/* (HWI-016F) the simulation's digests with HWI-016F's frame-coupled
	fields kept in (HWI-015B's classification, under which its Prisoner
	gives 127656ef): the last tick's canonical and raw ones, and the raw
	chain */
	unsigned long simulation_hwi015b_digest;
	unsigned long simulation_hwi015b_raw_digest;
	unsigned long simulation_hwi015b_raw_chain;
	unsigned long raw_digest;                    /* the whole state's, raw */
	unsigned long raw_chain;
	unsigned long tags_at_start, tags_at_end;    /* the tag slot */
	unsigned long random_seed;
	long objects;                                /* live objects after the last tick */
	long player_unit;                            /* the local player's unit datum, or -1 */
	float player_position[3];
	long player_spawn_tick;                      /* the first tick after which it had a unit, or -1 */
	long first_divergent_tick;                   /* the simulation's, against the reference run, or -1 */
	short structure_bsp_index;                   /* loaded after the last tick */
	long bsp_switches;                           /* structure bsps loaded during the run */
	long command_tick;                           /* when the run's script command ran, or -1 */
	long command_result;                         /* hs_compile_and_evaluate's */
	short command_bsp_before, command_bsp_after; /* the structure bsp loaded as the command ran, and after it */
	long requests_left;                          /* frames ending with a won map or a reset asked for */
};

/* the game's state at a tick, for the log (engine_map_run.c's PROGRESS lines) */
struct real_map_progress
{
	long tick;
	short structure_bsp_index;
	int cinematic;                               /* cinematic_in_progress */
	long objects;
	long actors;                                 /* live AI actors */
	long script_threads;                         /* live script threads */
	long player_unit;
	float player_position[3];
};

/* the engine's allocations, recorded as game_initialize makes them (wrapped
game_state_* allocators): call before game_initialize */
void real_map_record_allocations(int on);
long real_map_allocation_count(void);
const char *real_map_allocation_name(long index);
unsigned long real_map_allocation_bytes(long index);
/* the allocation's digest at a checkpoint of the last run */
unsigned long real_map_allocation_digest(long index, int checkpoint);
unsigned long real_map_allocation_raw_digest(long index, int checkpoint);
/* (diagnosis) its raw digest as the game state is now */
unsigned long real_map_allocation_digest_now(long index);

/* load the staged map named by its scenario's path (levels\...\name), with
a local player; 0 on failure (the engine's error text says why) */
int real_map_load(const char *scenario_path);
/* the engine's map reset (a new run from the same start) */
int real_map_reset(void);
/* arm a run of ticks ticks (at most REAL_MAP_MAXIMUM_TICKS): the clock from
game time 0; the reference run's raw simulation digests (NULL for none) are
compared tick by tick */
int real_map_begin(const unsigned long *reference_digests, long ticks);
/* one rendered frame of dt seconds through the engine's scheduler; 0 once
the run's ticks have run */
int real_map_frame(float dt);
void real_map_end(struct real_map_result *result);
/* every tick's simulation digest of the last run (its ticks) */
const unsigned long *real_map_tick_digests(void);
const unsigned long *real_map_tick_digests_raw(void);
/* the canonical digests every tick (HWI-015B's), or only at the checkpoints
(1, 30, 150 and the last tick: a long run's raw digest every tick is the
comparison); each run's first divergent tick is the raw digests' */
void real_map_set_canonical_every_tick(int every);
/* a script command (script source, such as "(switch_bsp 1)") the harness
runs through the engine's own script compiler and runtime after the given
tick of every run armed from now on (0: none) */
void real_map_set_command(long tick, const char *command);
void real_map_progress_get(struct real_map_progress *progress);
void real_map_unload(void);
/* nonzero while a run is armed */
int real_map_armed(void);

/* the engine-side hooks (engine_hooks.c's game_tick and
update_client_local_ticks wrappers call them while a run is armed) */
void real_map_after_tick(void);
void real_map_local_ticks(short ticks);

/* (diagnostics) called after each tick's digest; and an allocation's address */
void real_map_set_tick_observer(void (*observer)(long tick));
void *real_map_allocation_address(long index);
unsigned long real_map_allocation_offset(long index); /* from the game state's base */
void *real_map_tag_slot_address(void);

/* the digest of size bytes at address, as a tick's (for checks) */
unsigned long real_map_digest_bytes(const void *address, unsigned long size);

#endif
