/*
ENGINE_SCENARIO_HOST.C

Host reference for the Wii engine build's controlled fixed-step scenario
(HWI-015, port/wii/engine/fixed_step_scenario.h). Built for the native
ports' i686 ABI with the Linux build's game flags
(tools/wii/run_engine_scenario_host.py), with the same engine units the
scenario uses on the Wii: the scheduler (source/game/game_time.c), data
arrays (data.c), CRC (crc.c), random numbers (random_math.c), vector math
(real_math.c) and the shared musl-derived transcendental functions.

What the Wii build takes from the rest of the linked engine is supplied
here, as the scenario sees it there: a local game with no player, whose
input is the scenario's script (engine_hooks.c wraps the same three entry
points), the game state as one allocation, and no network.

It runs the same render cadences as the Wii driver (with 60 Hz in place of
the Wii's measured vertical retrace) and prints one CADENCE line each, in
the Wii log's format.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "game/game.h"
#include "saved games/game_state.h"

#include "../../port/wii/engine/fixed_step_scenario.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the game state: one block, as the Wii's arena region */

#define HOST_GAME_STATE_BYTES 0x40000

static unsigned char *game_state;
static long game_state_used;

void *game_state_malloc(const char *name, const char *type, long size)
{
	void *result;

	(void)name;
	(void)type;
	if (!game_state)
		game_state = calloc(1, HOST_GAME_STATE_BYTES);
	if (!game_state || (size & 3) || game_state_used + size > HOST_GAME_STATE_BYTES)
	{
		fprintf(stderr, "game_state_malloc: %ld bytes do not fit\n", size);
		exit(3);
	}
	result = game_state + game_state_used;
	game_state_used += size;
	return result;
}

struct data_array *game_state_data_new(const char *name, short maximum_count, short size)
{
	struct data_array *data = game_state_malloc(name, "data array", data_allocation_size(maximum_count, size));

	data_initialize(data, name, maximum_count, size);
	return data;
}

/* ---------- what cseries and the error report provide

(cseries.h renames the C library's names to its own; the definitions here
call the C library's) */

#undef memset
#undef memcpy
#undef strncpy
#undef malloc
#undef free

void display_assert(char *information, char *file, long line, boolean fatal)
{
	fprintf(stderr, "EXCEPTION %s in %s,#%ld: %s\n", fatal ? "halt" : "warn", file, line,
		information ? information : "");
}

void release_assert_failed(char const *information, char const *file, long line, boolean fatal)
{
	display_assert((char *)information, (char *)file, line, fatal);
}

void system_exit(long code)
{
	exit((int)(code ? code : 1));
}

void *csmemset(void *buffer, long c, unsigned long size)
{
	return memset(buffer, (int)c, size);
}

void *csmemcpy(void *destination, const void *source, unsigned long size)
{
	return memcpy(destination, source, size);
}

char *csstrncpy(char *s1, const char *s2, unsigned long size)
{
	return strncpy(s1, s2, size);
}

char *csprintf(char *buffer, char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsprintf(buffer, format, arguments);
	va_end(arguments);
	return buffer;
}

void *debug_malloc(unsigned int size, boolean clear, const char *file, long line)
{
	(void)file;
	(void)line;
	return clear ? calloc(1, size) : malloc(size);
}

void debug_free(void *pointer, const char *file, long line)
{
	(void)file;
	(void)line;
	free(pointer);
}

char temporary[256];

/* no multiplayer game engine runs (random_math.c asks) */
boolean game_engine_running(void)
{
	return FALSE;
}

/* the Linux layer's printf family (port/linux/include/stdio.h renames the
game's calls to them) */
#undef printf
#undef fprintf
#undef vsprintf
#undef vfprintf

int halo_linux_vsprintf(char *buffer, const char *format, va_list arguments)
{
	return vsprintf(buffer, format, arguments);
}

int halo_linux_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vprintf(format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_fprintf(FILE *file, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vfprintf(file, format, arguments);
	va_end(arguments);
	return result;
}

void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

/* x87 precision control: the host's maths is SSE, as the Linux build's */
unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	(void)new_value;
	(void)mask;
	return 0;
}

void error(short priority, const char *format, ...)
{
	(void)priority;
	fprintf(stderr, "error: %s\n", format);
}

/* ---------- a local game with no player and no network */

static short connection;

void game_connection_set(short value)
{
	connection = value;
}

short game_connection(void)
{
	return connection;
}

void players_initialize_for_new_map(void) {}
boolean update_server_new(void) { return TRUE; }
void update_server_delete(void) {}
void update_server_start(void) {}
void update_client_start(void) {}
/* engine_hooks.c on the Wii: the scenario's script is the input */
void update_client_local_ticks(short ticks) { (void)ticks; }
long update_client_get_maximum_possible_server_time(void) { return LONG_MAX; }
boolean network_game_distributed_client(void) { return FALSE; }
void *global_network_game_client_get(void) { return NULL; }
boolean network_game_client_server_has_started_game(void *client) { (void)client; return FALSE; }
void *global_network_game_server_get(void) { return NULL; }
boolean network_game_server_update_ticks(void *server, short ticks) { (void)server; (void)ticks; return FALSE; }
void network_distributed_tick(void) {}

/* presentation: the Wii build reports interpolation unsupported, so its
hooks do nothing there either */
void render_interpolation_tick(void) {}
void render_interpolation_frame_begin(void) {}
void render_interpolation_frame_end(void) {}

/* engine_hooks.c on the Wii: the scenario's tick and frame while armed */
void game_tick(void)
{
	fixed_step_scenario_tick();
}

void game_frame(real dt)
{
	fixed_step_scenario_game_frame(dt);
}

/* ---------- the cadences of the Wii driver (port/wii/engine/wii_engine_main.c) */

static float cadence_dt(int cadence, long frame, unsigned long *state)
{
	switch (cadence)
	{
	case 0: return 1.0f / 60.0f;
	case 1: return 1.0f / 30.0f;
	case 2: return 1.0f / 20.0f;
	case 3: return 1.0f / 144.0f;
	case 4:
		if (frame == 40)
			return 0.5f;
		*state = *state * 1664525UL + 1013904223UL;
		return (4.0f + (float)(*state >> 16 & 0xffff) / 65535.0f * 79.0f) / 1000.0f;
	default: return 1.0f / 60.0f;
	}
}

int main(void)
{
	static const char *const names[] = { "60hz", "30hz", "20hz", "144hz", "irregular", "60hz_for_vsync" };
	int cadence, failures = 0;
	unsigned long reference = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	game_time_initialize();
	if (!fixed_step_scenario_allocate())
		return 3;
	printf("BEGIN host pointer_bits=%u game_state_used=%ld\n", (unsigned)(sizeof(void *) * 8), game_state_used);
	for (cadence = 0; cadence < 6; cadence++)
	{
		struct fixed_step_scenario_result result;
		unsigned long state = 12345;
		long frame = 0;

		if (!fixed_step_scenario_begin(0x48574931UL))
			return 4;
		while (frame < 100000 && fixed_step_scenario_frame(cadence_dt(cadence, frame, &state)))
			frame++;
		fixed_step_scenario_end(&result);
		if (!cadence)
			reference = result.digest;
		failures += result.ticks != FIXED_STEP_SCENARIO_TICKS || result.digest != reference;
		printf("CADENCE cycle=0 cadence=%s ticks=%ld frames=%ld frames_without_ticks=%ld most_ticks_in_a_frame=%ld "
			"digest=%08lx chain=%08lx at1=%08lx at30=%08lx at150=%08lx at300=%08lx seed=%08lx bodies=%ld spawned=%ld "
			"deleted=%ld same=%d\n", names[cadence], result.ticks, result.frames, result.frames_without_ticks,
			result.most_ticks_in_a_frame, result.digest, result.chain, result.digest_at[0], result.digest_at[1],
			result.digest_at[2], result.digest_at[3], result.random_seed, result.live_bodies, result.spawned,
			result.deleted, result.digest == reference);
	}
	printf("END host failures=%d result=%s\n", failures, failures ? "fail" : "pass");
	return failures ? 1 : 0;
}
