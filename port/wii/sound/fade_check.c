/*
FADE_CHECK.C

HWI-078: a host check of the scripted sound class fades on the game's ticks
(tools/wii/test_sound.py). The engine's own sound_classes.c is compiled in
below, with the little of cseries it uses; the test gives it empty headers
for the rest. A script sets fades at chosen ticks (sound_class_set_gain, as
hs's sound_class_set_gain does), and the fades are moved three ways:

- xbox: the Xbox's frame at 30 Hz, a tick a frame: the tick, then
  sound_render's sound_classes_update of the frame's one tick;
- tick: the ports' frames at a cadence, game_time_update's ticks of each
  frame, and after each tick sound_tick_update's sound_classes_update(1)
  (sound_manager.c), nothing in the frame's render;
- frame: HWI-074's coupling, the render's sound_classes_update of the
  ticks the frame clock's milliseconds make (the fraction carried over).

Each tick's game state (the classes' 0x264 bytes, sound_class_data) is
hashed where the map run hashes it (after the tick, before what follows it),
and chained.

  fade_check     FADE_XBOX, then FADE_TICK and FADE_FRAME for each cadence,
                 FADE_REVERT, FADE_RESULT; exit 0 if every tick-driven run
                 equals the Xbox's on every tick
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------- what sound_classes.c uses of cseries (cseries.h, errors.h) */

typedef unsigned char byte;
typedef float real;
typedef byte boolean;

#define TRUE 1
#define FALSE 0
#define NONE -1
#define CEILING(n,ceiling) ((n)>(ceiling)?(ceiling):(n))
#define PIN(n,floor,ceiling) ((n)<(floor) ? (floor) : CEILING((n),(ceiling)))
#define VALID_INDEX(index, count) (index>=0 && index<count)

static unsigned long assertions_failed;

#define match_assert(file, line, expr) if (!(expr)) { assertions_failed++; }

enum { _error_silent };

static void error(int priority, char const *format, ...)
{
	(void)priority;
	(void)format;
}

static byte sound_class_state[0x264];

static void *game_state_malloc(char const *name, char const *type, long size)
{
	(void)name;
	(void)type;
	return size <= (long)sizeof(sound_class_state) ? sound_class_state : NULL;
}

/* (the engine's long is 32 bits, as on the Xbox and the Wii) */
#if defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ != 4
#define long int
#endif
#include "../../../source/sound/sound_classes.c"
#undef long

/* ---------- the check */

enum
{
	CHECK_TICKS = 240,
	/* game_time.c: a local game's most ticks a frame (the rest discarded) */
	CHECK_MAXIMUM_TICKS_PER_FRAME = 7,
	CHECK_REVERT_TICK = 120,
};

struct script_event
{
	long tick;
	char const *name;
	real gain;
	short ticks;
};

/* (sound_class_set_gain's names match by part, as hs's) */
static struct script_event const script[] =
{
	{ 10, "", 0.f, 30 },          /* every class out over a second */
	{ 25, "ambient", 1.f, 45 },   /* three of them turned back halfway */
	{ 80, "music", 0.5f, 0 },     /* at once */
	{ 100, "weapon", 0.25f, 7 },
	{ 101, "weapon_fire", 0.75f, 90 },
	{ 150, "", 1.f, 60 },         /* every class back */
	{ 190, "scripted_dialog", 0.f, 1 },
};

enum cadence
{
	_cadence_30, _cadence_60, _cadence_20, _cadence_144, _cadence_irregular, _cadence_vsync,
	NUMBER_OF_CADENCES
};

static char const *const cadence_names[NUMBER_OF_CADENCES] =
{
	"30hz", "60hz", "20hz", "144hz", "irregular", "vsync"
};

/* engine_map_run.c's cadence_dt (the host's vsync: 1/60 s) */
static float cadence_dt(enum cadence cadence, long frame, unsigned long *state)
{
	switch (cadence)
	{
	case _cadence_60: return 1.0f / 60.0f;
	case _cadence_30: return 1.0f / 30.0f;
	case _cadence_20: return 1.0f / 20.0f;
	case _cadence_144: return 1.0f / 144.0f;
	case _cadence_irregular:
		if (frame == 40)
			return 0.5f;
		*state = (*state * 1664525UL + 1013904223UL) & 0xffffffffUL;
		return (4.0f + (float)(*state >> 16 & 0xffff) / 65535.0f * 79.0f) / 1000.0f;
	case _cadence_vsync:
	default:
		return 1.0f / 60.0f;
	}
}

static unsigned long crc_table[256];

static unsigned long crc_add(unsigned long crc, byte const *bytes, unsigned long count)
{
	unsigned long index;

	crc ^= 0xffffffffUL;
	for (index = 0; index < count; index++)
		crc = crc_table[(crc ^ bytes[index]) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffffUL;
}

struct run
{
	unsigned long digests[CHECK_TICKS];
	unsigned long chain;
	long frames;
	long ticks;
	/* sound_tick_report's: ticks a class's gain moved on, the classes moved */
	unsigned long fade_ticks;
	unsigned long fade_class_steps;
};

/* the tick: its script, then its digest */
static void run_tick(struct run *run)
{
	unsigned long index;

	for (index = 0; index < sizeof(script) / sizeof(script[0]); index++)
	{
		if (script[index].tick == run->ticks)
			sound_class_set_gain(script[index].name, script[index].gain, script[index].ticks);
	}
	run->digests[run->ticks] = crc_add(0, sound_class_state, sizeof(sound_class_state));
	run->chain = crc_add(run->chain, (byte const *)&run->digests[run->ticks], sizeof(run->digests[run->ticks]));
	run->ticks++;
}

/* sound_manager.c's sound_tick_update */
static void tick_update(struct run *run)
{
	short moving = sound_classes_moving();

	if (moving > 0)
	{
		run->fade_ticks++;
		run->fade_class_steps += (unsigned long)moving;
	}
	sound_classes_update(1);
}

static void run_new(struct run *run)
{
	memset(run, 0, sizeof(*run));
	sound_classes_initialize();
	sound_classes_initialize_for_new_map();
}

static void run_xbox(struct run *run)
{
	run_new(run);
	while (run->ticks < CHECK_TICKS)
	{
		run_tick(run);
		/* the frame's render: its clock says a tick has passed */
		sound_classes_update(1);
		run->frames++;
	}
}

/* frame_coupled: HWI-074's render of the frame clock's ticks; otherwise the
ticks' own step. From the run's ticks so far (a loaded state's) */
static void run_cadence(struct run *run, enum cadence cadence, int frame_coupled)
{
	unsigned long state = 0x48574937UL;
	real leftover_dt = 0.f;
	double run_seconds = 0.0;
	unsigned long run_milliseconds = 0;
	unsigned long render_time = 0;
	real leftover_ticks = 0.f;

	while (run->ticks < CHECK_TICKS)
	{
		float dt = cadence_dt(cadence, run->frames, &state);
		real game_time = dt + leftover_dt;
		real ticks_real = (real)floor(game_time * 30.f);
		long ticks = (long)ticks_real;
		long tick;

		/* game_time_update: a local game's ticks of the frame */
		if (ticks > CHECK_MAXIMUM_TICKS_PER_FRAME)
		{
			ticks = CHECK_MAXIMUM_TICKS_PER_FRAME;
			game_time = ticks_real / 30.f;
		}
		leftover_dt = game_time - ticks_real / 30.f;
		if (leftover_dt < 0.f)
			leftover_dt = 0.f;
		for (tick = 0; tick < ticks && run->ticks < CHECK_TICKS; tick++)
		{
			run_tick(run);
			if (!frame_coupled)
				tick_update(run);
		}
		if (frame_coupled)
		{
			/* sound_hooks.c's frame clock, then sound_render's ticks */
			long now;
			long whole;

			run_seconds += (double)dt;
			if ((unsigned long)(run_seconds * 1000.0) > run_milliseconds)
				run_milliseconds = (unsigned long)(run_seconds * 1000.0);
			now = (long)run_milliseconds;
			leftover_ticks += ((real)now - (real)render_time) * 0.029999999f;
			render_time = (unsigned long)now;
			whole = (long)leftover_ticks;
			leftover_ticks -= (real)whole;
			sound_classes_update(whole);
		}
		run->frames++;
	}
}

static long first_divergent(struct run const *run, struct run const *reference, long from)
{
	long tick;

	for (tick = from; tick < CHECK_TICKS; tick++)
	{
		if (run->digests[tick] != reference->digests[tick])
			return tick;
	}
	return NONE;
}

int main(void)
{
	static struct run xbox, run, revert;
	static byte saved[sizeof(sound_class_state)];
	unsigned long index;
	int cadence;
	int failures = 0;
	unsigned long frame_coupled_differing = 0;

	for (index = 0; index < 256; index++)
	{
		unsigned long crc = index;
		int bit;

		for (bit = 0; bit < 8; bit++)
			crc = crc & 1 ? 0xedb88320UL ^ (crc >> 1) : crc >> 1;
		crc_table[index] = crc;
	}

	run_xbox(&xbox);
	printf("FADE_XBOX ticks=%ld frames=%ld chain=%08lx last=%08lx\n", (long)xbox.ticks, (long)xbox.frames,
		xbox.chain, xbox.digests[CHECK_TICKS - 1]);
	for (cadence = 0; cadence < NUMBER_OF_CADENCES; cadence++)
	{
		long divergent;

		run_new(&run);
		run_cadence(&run, (enum cadence)cadence, FALSE);
		divergent = first_divergent(&run, &xbox, 0);
		printf("FADE_TICK cadence=%s ticks=%ld frames=%ld chain=%08lx fade_ticks=%lu fade_class_steps=%lu "
			"first_divergent=%ld\n", cadence_names[cadence], (long)run.ticks, (long)run.frames, run.chain,
			run.fade_ticks, run.fade_class_steps, (long)divergent);
		if (divergent != NONE || run.chain != xbox.chain || !run.fade_ticks)
			failures++;

		run_new(&run);
		run_cadence(&run, (enum cadence)cadence, TRUE);
		divergent = first_divergent(&run, &xbox, 0);
		printf("FADE_FRAME cadence=%s ticks=%ld frames=%ld chain=%08lx first_divergent=%ld\n",
			cadence_names[cadence], (long)run.ticks, (long)run.frames, run.chain, (long)divergent);
		if (divergent != NONE)
			frame_coupled_differing++;
	}

	/* a revert: the state saved after a tick's step (where main_loop saves,
	after the frame), the game run on, the save loaded and its ticks run again
	at another cadence: the same fades follow, as they did running on */
	run_new(&run);
	while (run.ticks < CHECK_TICKS)
	{
		run_tick(&run);
		tick_update(&run);
		if (run.ticks == CHECK_REVERT_TICK)
		{
			memcpy(saved, sound_class_state, sizeof(saved));
			revert = run;
		}
	}
	memcpy(sound_class_state, saved, sizeof(saved));
	run_cadence(&revert, _cadence_144, FALSE);
	printf("FADE_REVERT tick=%d cadence=144hz first_divergent=%ld\n", CHECK_REVERT_TICK,
		(long)first_divergent(&revert, &xbox, CHECK_REVERT_TICK));
	if (first_divergent(&revert, &xbox, CHECK_REVERT_TICK) != NONE)
		failures++;

	printf("FADE_RESULT failures=%d frame_coupled_differing=%lu assertions_failed=%lu\n", failures,
		frame_coupled_differing, assertions_failed);

	return failures || assertions_failed ? 1 : 0;
}
