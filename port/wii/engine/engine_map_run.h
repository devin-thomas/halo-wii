/*
ENGINE_MAP_RUN.H

The real-map run of the Wii engine build (HWI-015B, HWI-016C), shared by the
Wii driver (wii_engine_main.c) and the i686 host reference
(tools/wii/engine_map_host.c), so both run exactly the same sequence and
print the same report lines (through wii_log):

1. The HWI-015A fixed-step scenario once at 30 Hz, before any map: its
   digest must still be c1ad6ae6 (chain 58ce9719).
2. The plan (map.txt on the card), a line at a time, in order:

   <scenario path> [ticks=N] [cycles=N] [cadences=<name>,...] [warmup=0|1|N]
       [expect=<simulation digest, 8 hex digits>] [expect_sim=<8 hex digits>]
       [switch_bsp=<tick>:<index>]
       [canonical=every|checkpoints] [invariance=1|0]
       [script=<tick>:<script source, ~ for a space>] [core_save=<tick>]
       [name=<the runs' name in the log>]
     A map's runs: an unmeasured warm-up run (unless warmup=0; of N ticks
     with warmup=N, N>1), then cycles
     x (load the staged map; run it at each cadence for ticks of the
     engine's own ticks; unload). Every run's every tick's simulation digest
     must equal the line's first run's (the 30 Hz run's: a line's cadences
     run 30 Hz first, HWI-016F); with expect=, its last must be that as
     HWI-015B classified the simulation (a regression check: HWI-015B's
     Prisoner is 127656ef), with expect_sim= as HWI-016F does (its
     frame-coupled fields left out too: real_map_scenario.c). switch_bsp= has
     the engine's script runtime compile and run "(switch_bsp <index>)"
     after that tick of every run, as a console command would.
     canonical=checkpoints digests the canonical state only at the
     checkpoints (the raw one every tick: real_map_scenario.h);
     invariance=0 reports a run whose simulation differs from the first's
     rather than failing it. (HWI-034) script= has the script runtime run
     that source after that tick, as switch_bsp= does (one or the other);
     core_save= saves a core after that tick (real_map_set_core_save);
     name= names the line's runs in the log, so lines of one map compare
     apart. Defaults:
     300 ticks, the driver's cycles, all six cadences (60hz, 30hz, 20hz,
     144hz, irregular, vsync_measured), a warm-up.

   fail <scenario path> <error kind> [directory=<Xbox path of a maps
       directory>]
     A load that must be refused with that kind (cache_files_wii.h:
     data_root, not_staged, stale, damaged, manifest_mismatch), leaving the
     engine able to load the next map. directory= points the cache layer at
     another maps directory for this load (a wrong data root).

   A map line with expect_error=<kind> is a map that loads but whose
   switch_bsp is refused with that kind (a damaged structure bsp): the run
   must keep the bsp it had.

   The heap must stay flat across a map's cycles (the driver measures it).

Only C types: the drivers include it beside libogc or the host's C library.
*/

#ifndef __HALO_WII_ENGINE_MAP_RUN_H
#define __HALO_WII_ENGINE_MAP_RUN_H

#define ENGINE_MAP_RUN_CADENCES 6
#define ENGINE_MAP_RUN_MAXIMUM_LINES 16

struct engine_map_run_config
{
	const char *plan;            /* map.txt's text: lines as above */
	int cycles;                  /* a map line's default */
	/* the measured frame time of the last vertical retrace (the Wii's
	video); NULL: 1/60 s (the host) */
	float (*vsync_dt)(void *context);
	void (*vsync_begin)(void *context);
	void *context;
	/* the driver's heap in use, compared between cycles */
	long (*heap_in_use)(void *context);
};

struct engine_map_run_report
{
	int failures;
	int fixed_step_pass;
	int map_loaded;
	int runs;
	int identical;               /* every run's simulation digests equal its line's first run's */
	int whole_identical;         /* every run's whole-state digests equal its cadence's first cycle's */
	int heap_flat;
	int expectations;            /* expect= and fail lines checked */
	int expectations_met;
	/* the plan's first map line's first run (the persistence check's) */
	unsigned long digest;        /* after its last tick */
	unsigned long chain;
	unsigned long simulation_digest;
	unsigned long simulation_chain;
	unsigned long tags_at_start;
	long player_spawn_tick;
	unsigned long load_milliseconds; /* its first load */
	unsigned long run_milliseconds;  /* its first run's frames */
};

int engine_map_run(const struct engine_map_run_config *config, struct engine_map_run_report *report);

/* diagnosis of a divergence: spec "trace:<allocation index>[,<index>...]"
logs those allocations' raw digests after every tick of every run
(ALLOC_TRACE lines, after each run), or spec "<allocation index>[,<index>...]:<tick>"
writes those allocations' bytes after that tick of every run (the warm-up is
run 1) to <directory>/dump-<run>-<index>.bin; a negative index dumps the
whole game state the allocations span, -2 the tag slot's loaded ranges
(dump-<run>-tags.bin). 1 if the spec was understood. */
int engine_map_run_set_dump(const char *spec, const char *directory);

#endif
