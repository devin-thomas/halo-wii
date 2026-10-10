/*
ENGINE_MAP_RUN.H

The real-map run of the Wii engine build (HWI-015B), shared by the Wii
driver (wii_engine_main.c) and the i686 host reference
(tools/wii/engine_map_host.c), so both run exactly the same sequence and
print the same report lines (through wii_log):

1. The HWI-015A fixed-step scenario once at 30 Hz, before any map: its
   digest must still be c1ad6ae6 (chain 58ce9719).
2. cycles x (load the staged map; run it at six render cadences, the
   engine's map reset between runs; unload). Every run's every tick's
   digest must equal the first run's. The heap must stay flat across
   cycles (the driver measures it).

Only C types: the drivers include it beside libogc or the host's C library.
*/

#ifndef __HALO_WII_ENGINE_MAP_RUN_H
#define __HALO_WII_ENGINE_MAP_RUN_H

#define ENGINE_MAP_RUN_CADENCES 6

struct engine_map_run_config
{
	const char *scenario_path;   /* levels\...\name */
	int cycles;
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
	int identical;               /* every run's simulation digests equal the first run's */
	int whole_identical;         /* every run's whole-state digests equal its cadence's first cycle's */
	int heap_flat;
	unsigned long digest;        /* the first run's, after its last tick */
	unsigned long chain;
	unsigned long simulation_digest;
	unsigned long simulation_chain;
	unsigned long tags_at_start;
	long player_spawn_tick;
	unsigned long load_milliseconds; /* the first cycle's load */
	unsigned long run_milliseconds;  /* the first run's frames */
};

int engine_map_run(const struct engine_map_run_config *config, struct engine_map_run_report *report);

/* diagnosis of a divergence: spec "<allocation index>[,<index>...]:<tick>"
writes those allocations' bytes after that tick of every run (the warm-up is
run 1) to <directory>/dump-<run>-<index>.bin; a negative index dumps the
whole game state the allocations span, -2 the tag slot's loaded ranges
(dump-<run>-tags.bin). 1 if the spec was understood. */
int engine_map_run_set_dump(const char *spec, const char *directory);

#endif
