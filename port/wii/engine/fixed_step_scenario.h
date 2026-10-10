/*
FIXED_STEP_SCENARIO.H

The controlled fixed-step update scenario of the Wii engine build (HWI-015,
ADR-007): a deterministic simulation that runs inside the engine's own
30 Hz scheduler, game_time_update (source/game/game_time.c), while the
"render" cadence that feeds it varies.

While the scenario is armed, the engine's per-tick work (game_tick) is the
scenario's tick, its per-frame work (game_frame) only counts frames, and its
input source is a script indexed by tick: the scenario stands in for a
loaded map, which the Wii build cannot load yet (its tags are little-endian;
see port/wii/engine/README.md). Everything else on the path is the engine's:
game_time_update's accumulation of frame time, its tick budget per frame
(7 ticks, leftover discarded, for a local game), the game state its data
live in (game_state_data_new), its data arrays (data.c), its random number
generator (random_math.c) and its vector math (real_math.c, and the
musl-derived transcendental functions every port shares).

The state digest is a CRC-32 (the engine's crc.c) of a canonical
little-endian serialisation of the scenario's state, so the same simulation
gives the same digest on PowerPC and on x86.

This header uses only C types: the Wii driver includes it beside libogc.
*/

#ifndef __HALO_WII_FIXED_STEP_SCENARIO_H
#define __HALO_WII_FIXED_STEP_SCENARIO_H

#define FIXED_STEP_SCENARIO_TICKS 300 /* 10 s of simulation at 30 Hz */
#define FIXED_STEP_SCENARIO_BODIES 64

struct fixed_step_scenario_result
{
	long ticks;                 /* ticks simulated (the engine's game time) */
	long frames;                /* frames the scheduler was given */
	long frames_without_ticks;  /* frames that ran no tick */
	long most_ticks_in_a_frame;
	unsigned long digest;       /* the state after the last tick */
	unsigned long chain;        /* CRC of every tick's digest, in order */
	unsigned long digest_at[4]; /* after ticks 1, 30, 150 and 300 */
	long live_bodies;
	long spawned, deleted;
	unsigned long random_seed;  /* the engine's global random seed after the last tick */
};

/* the engine-side allocations: the scenario's data array and globals in the
game state; once per engine start, before the game state is locked */
int fixed_step_scenario_allocate(void);
/* arm the scenario and start the engine's clock from game time 0 */
int fixed_step_scenario_begin(unsigned long seed);
/* one rendered frame of dt seconds through the engine's scheduler; 0 once
FIXED_STEP_SCENARIO_TICKS ticks have run */
int fixed_step_scenario_frame(float dt);
/* stop the clock, disarm, and report */
void fixed_step_scenario_end(struct fixed_step_scenario_result *result);
/* nonzero while armed */
int fixed_step_scenario_armed(void);

/* the scenario's tick and frame (the Wii build's game_tick/game_frame
wrappers and the host fixture call them) */
void fixed_step_scenario_tick(void);
void fixed_step_scenario_game_frame(float dt);

#endif
