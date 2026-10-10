/*
FIXED_STEP_SCENARIO.C

The controlled fixed-step update scenario (fixed_step_scenario.h). Compiled
as an engine unit (the engine's headers, ENGINE_CFLAGS) in the Wii engine
build, and with the same units for i686 by the host fixture
(tools/wii/engine_scenario_host.c), which supplies the few engine services
the Wii build takes from elsewhere in the engine.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "memory/crc.h"
#include "game/game.h"
#include "saved games/game_state.h"

#include "fixed_step_scenario.h"

/* the engine's own (player_queues_new.c, players.c), called as a loaded
map's start and end call them */
void players_initialize_for_new_map(void);
boolean update_server_new(void);
void update_server_delete(void);
void game_time_dispose_from_old_map(void);
void game_time_end(void);
short game_time_get_elapsed(void);

struct scenario_body
{
	short identifier; /* the data array's datum header */
	short flags;
	long spawned_tick;
	real_point3d position;
	real_vector3d velocity;
	real_vector3d forward;
	real energy;
};

struct scenario_globals
{
	boolean armed;
	long tick;
	long frames;
	long frames_without_ticks;
	long most_ticks_in_a_frame;
	long spawned;
	long deleted;
	unsigned long digest;
	unsigned long chain;
	unsigned long digest_at[4];
};

static struct data_array *scenario_bodies;
static struct scenario_globals *scenario;

/* ---------- the digest: little-endian bytes, whatever the CPU */

struct canonical
{
	unsigned char bytes[64];
	long used;
};

static void put32(struct canonical *out, unsigned long value)
{
	out->bytes[out->used++] = (unsigned char)value;
	out->bytes[out->used++] = (unsigned char)(value >> 8);
	out->bytes[out->used++] = (unsigned char)(value >> 16);
	out->bytes[out->used++] = (unsigned char)(value >> 24);
}

static void put_real(struct canonical *out, real value)
{
	union { real value; unsigned long bits; } pun;

	pun.value = value;
	put32(out, pun.bits);
}

static unsigned long scenario_state_digest(void)
{
	unsigned long crc;
	struct canonical out;
	short absolute_index;

	crc_new(&crc);
	out.used = 0;
	put32(&out, (unsigned long)scenario->tick);
	put32(&out, *get_global_random_seed_address());
	put32(&out, (unsigned long)scenario_bodies->count);
	put32(&out, (unsigned long)(unsigned short)scenario_bodies->next_identifier);
	crc_checksum_buffer(&crc, out.bytes, out.used);
	for (absolute_index = 0; absolute_index < scenario_bodies->maximum_count; absolute_index++)
	{
		struct scenario_body *body = (struct scenario_body *)((char *)scenario_bodies->data +
			absolute_index * scenario_bodies->size);
		short component;

		if (!body->identifier)
			continue;
		out.used = 0;
		put32(&out, (unsigned long)absolute_index);
		put32(&out, (unsigned long)(unsigned short)body->identifier);
		put32(&out, (unsigned long)(unsigned short)body->flags);
		put32(&out, (unsigned long)body->spawned_tick);
		for (component = 0; component < 3; component++)
		{
			put_real(&out, body->position.n[component]);
			put_real(&out, body->velocity.n[component]);
			put_real(&out, body->forward.n[component]);
		}
		put_real(&out, body->energy);
		crc_checksum_buffer(&crc, out.bytes, out.used);
	}
	return crc;
}

/* ---------- the scripted input of each tick */

struct scenario_input
{
	real_vector2d move;
	boolean fire;
};

static void scenario_input_for_tick(long tick, struct scenario_input *input)
{
	unsigned long seed = 0x9e3779b9UL * (unsigned long)(tick + 1) ^ 0x5eed0015UL;

	input->move.i = real_seed_random_range(&seed, -1.0f, 1.0f);
	input->move.j = real_seed_random_range(&seed, -1.0f, 1.0f);
	input->fire = seed_random_range(&seed, 0, 4) == 0;
}

/* ---------- public code */

int fixed_step_scenario_allocate(
	void)
{
	if (scenario_bodies)
		return TRUE;
	scenario = game_state_malloc("hwi015 fixed-step scenario", NULL, sizeof(*scenario));
	scenario_bodies = game_state_data_new("hwi015 scenario bodies", FIXED_STEP_SCENARIO_BODIES,
		sizeof(struct scenario_body));
	if (scenario)
		csmemset(scenario, 0, sizeof(*scenario));
	return scenario && scenario_bodies;
}

int fixed_step_scenario_armed(
	void)
{
	return scenario && scenario->armed;
}

int fixed_step_scenario_begin(
	unsigned long seed)
{
	if (!scenario || scenario->armed)
		return FALSE;
	csmemset(scenario, 0, sizeof(*scenario));
	crc_new(&scenario->chain);
	data_make_valid(scenario_bodies);
	data_delete_all(scenario_bodies);
	set_random_seed(seed);
	/* a local game's start, as game_initialize_for_new_map makes it */
	game_connection_set(_game_connection_local);
	players_initialize_for_new_map();
	if (!update_server_new())
		return FALSE;
	game_time_initialize_for_new_map();
	scenario->armed = TRUE;
	game_time_start();
	return TRUE;
}

int fixed_step_scenario_frame(
	float dt)
{
	short elapsed;

	game_time_update(dt);
	elapsed = game_time_get_elapsed();
	scenario->frames++;
	if (!elapsed)
		scenario->frames_without_ticks++;
	if (elapsed > scenario->most_ticks_in_a_frame)
		scenario->most_ticks_in_a_frame = elapsed;
	return scenario->tick < FIXED_STEP_SCENARIO_TICKS;
}

void fixed_step_scenario_end(
	struct fixed_step_scenario_result *result)
{
	short index;

	result->ticks = scenario->tick;
	result->frames = scenario->frames;
	result->frames_without_ticks = scenario->frames_without_ticks;
	result->most_ticks_in_a_frame = scenario->most_ticks_in_a_frame;
	result->digest = scenario->digest;
	result->chain = scenario->chain;
	for (index = 0; index < 4; index++)
		result->digest_at[index] = scenario->digest_at[index];
	result->live_bodies = scenario_bodies->count;
	result->spawned = scenario->spawned;
	result->deleted = scenario->deleted;
	result->random_seed = *get_global_random_seed_address();
	/* a map's end, as game_dispose_from_old_map makes it */
	game_time_end();
	game_time_dispose_from_old_map();
	update_server_delete();
	data_delete_all(scenario_bodies);
	data_make_invalid(scenario_bodies);
	scenario->armed = FALSE;
}

void fixed_step_scenario_tick(
	void)
{
	const real seconds_per_tick = 1.0f / TICKS_PER_SECOND;
	struct scenario_input input;
	struct data_iterator iterator;
	struct scenario_body *body;
	real_vector3d up = {{ 0.0f, 0.0f, 1.0f }};

	/* ticks the scheduler runs after the last one change nothing */
	if (scenario->tick >= FIXED_STEP_SCENARIO_TICKS)
		return;
	real_math_reset_precision();
	scenario_input_for_tick(scenario->tick, &input);

	if (input.fire && scenario_bodies->count < FIXED_STEP_SCENARIO_BODIES)
	{
		long index = datum_new(scenario_bodies);

		if (index != NONE)
		{
			body = datum_get(scenario_bodies, index);
			body->flags = (short)(scenario->spawned & 7);
			body->spawned_tick = scenario->tick;
			body->position.x = real_random_range(-8.0f, 8.0f);
			body->position.y = real_random_range(-8.0f, 8.0f);
			body->position.z = real_random_range(1.0f, 6.0f);
			body->velocity.i = input.move.i * 3.0f;
			body->velocity.j = input.move.j * 3.0f;
			body->velocity.k = real_random_range(0.0f, 4.0f);
			body->forward.i = real_random_range(-1.0f, 1.0f);
			body->forward.j = real_random_range(-1.0f, 1.0f);
			body->forward.k = 0.25f;
			normalize3d(&body->forward);
			body->energy = real_random_range(0.5f, 1.0f);
			scenario->spawned++;
		}
	}

	data_iterator_new(&iterator, scenario_bodies);
	while ((body = data_iterator_next(&iterator)) != NULL)
	{
		real speed, angle;
		short component;

		body->velocity.i += (input.move.i * 1.5f - body->velocity.i * 0.1f) * seconds_per_tick;
		body->velocity.j += (input.move.j * 1.5f - body->velocity.j * 0.1f) * seconds_per_tick;
		body->velocity.k += -9.8f * seconds_per_tick;
		for (component = 0; component < 3; component++)
			body->position.n[component] += body->velocity.n[component] * seconds_per_tick;
		if (body->position.z < 0.0f)
		{
			body->position.z = -body->position.z * 0.5f;
			body->velocity.k = -body->velocity.k * 0.6f;
			body->energy -= 0.05f;
		}
		speed = magnitude3d(&body->velocity);
		angle = speed * seconds_per_tick;
		rotate_vector_about_axis(&body->forward, &up, (real)sin(angle), (real)cos(angle));
		normalize3d(&body->forward);
		body->energy -= 0.002f;
		if (body->energy <= 0.0f)
		{
			datum_delete(scenario_bodies, iterator.datum_index);
			scenario->deleted++;
		}
	}

	scenario->tick++;
	scenario->digest = scenario_state_digest();
	{
		struct canonical out;

		out.used = 0;
		put32(&out, scenario->digest);
		crc_checksum_buffer(&scenario->chain, out.bytes, out.used);
	}
	switch (scenario->tick)
	{
	case 1: scenario->digest_at[0] = scenario->digest; break;
	case 30: scenario->digest_at[1] = scenario->digest; break;
	case 150: scenario->digest_at[2] = scenario->digest; break;
	case FIXED_STEP_SCENARIO_TICKS: scenario->digest_at[3] = scenario->digest; break;
	}
}

void fixed_step_scenario_game_frame(
	float dt)
{
	/* a frame changes no simulation state; the map's per-frame effects
	(particles, contrails, widgets, sound) need a map */
	(void)dt;
}
