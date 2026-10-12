/*
SAVE_IMAGE_SCENARIO.H

(HWI-076) The real-map run's view of saved games: every save, load and
check of the canonical game-state image (source/saved games/
game_state_image.c) is reported, and a plan line's image_test=<tick> damages
a core saved at that tick in each way a load must refuse and loads each one,
which must leave the game state as it was.

Only C types here: engine_map_run.c includes it beside the platform layer.
*/

#ifndef __HALO_WII_SAVE_IMAGE_SCENARIO_H
#define __HALO_WII_SAVE_IMAGE_SCENARIO_H

enum
{
	SAVE_IMAGE_EVENT_SAVE,
	SAVE_IMAGE_EVENT_LOAD,
	SAVE_IMAGE_EVENT_CHECK,
	SAVE_IMAGE_EVENT_BEGIN,   /* a save, load or check begins (for timing) */
	SAVE_IMAGE_EVENT_TEST,    /* an image test's case: a damaged image loaded */
	SAVE_IMAGE_EVENT_SUMMARY, /* an image test's cases, all of them */
};

struct save_image_event
{
	int kind;
	int result;          /* 0 written, taken or valid; 1 refused; 2 failed while taking */
	const char *refusal; /* why it was refused ("none") */
	long refusal_offset;
	long tick;           /* the run's tick after which it happened */
	const char *test_case;
	int intact;          /* a test case: the game state as before the load */
	int expected;        /* a test case: refused for the reason expected */
	unsigned long state_bytes;
	unsigned long file_bytes;
	unsigned long payload_bytes;
	unsigned long chunks;
	unsigned long chunk_bytes;
	unsigned long peak_bytes;
	unsigned long pointers;
	unsigned long references;
	unsigned long script_threads;
	unsigned long foreign_pointers;
	unsigned long address_like_words;
	unsigned long address_like_dead_words;
	long first_address_like_offset;
	unsigned long sha[8];
	unsigned long state_digest; /* CRC of the game state's room, after a save or a load */
	int same_as_saved;  /* a load: the game state is the one this image was saved from (-1: none known) */
	int reencoded_same; /* a load: the game state written again gives this image (-1: not checked) */
	int passes;
	int cases;          /* the summary's: cases, refused as expected, the state intact */
	int cases_expected;
	int cases_intact;
};

void save_image_scenario_set_observer(void (*observer)(const struct save_image_event *event));
/* image tests after the given tick of every run from now on (0: none) */
void save_image_scenario_set_test(long tick);
/* after each tick of a run (engine_map_run.c's tick observer) */
void save_image_scenario_after_tick(long tick);

#endif
