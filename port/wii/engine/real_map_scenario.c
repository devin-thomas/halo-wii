/*
REAL_MAP_SCENARIO.C

A real map through the engine's own game tick (real_map_scenario.h,
HWI-015B). Compiled as an engine unit in the Wii engine build and in the
i686 host reference (tools/wii/run_engine_map_host.py).
*/

#include "cseries.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "memory/memory_pool.h"
#include "memory/crc.h"
#include "game/game.h"
#include "game/players.h"
#include "saved games/game_state.h"
#include "cache/physical_memory_map.h"
#include "objects/objects.h"
#include "units/units.h"

#include "real_map_scenario.h"
#include "cache_files_wii.h"

/* the engine's own (main.c, game.c, player_queues_new.c, scenario.c ...),
as main_new_map and main_reset_map_private call them */
void game_dispose_from_old_map(void);
void game_unload(void);
void game_initialize_for_new_map(void);
void game_initial_pulse(void);
void game_time_start(void);
void game_time_end(void);
short game_time_get_elapsed(void);
boolean scenario_switch_structure_bsp(short structure_bsp_index);
void input_flush(void);
void game_connection_set(short connection);
unsigned long *get_global_random_seed_address(void);
unsigned long *get_global_local_random_seed_address(void);

/* (game.c's, private there) */
struct real_map_game_options
{
	unsigned long flags;
	short code_version;
	short difficulty;
	unsigned long random_seed;
	char map_name[256];
};

typedef char verify_real_map_game_options_size[sizeof(struct real_map_game_options) == 0x10C ? 1 : -1];

/* (object_lights.c's lights_globals, private there: its first members) */
struct real_map_lights_globals_prefix
{
	boolean marker_initialized;
	byte pad1[3];
	long marker;
};

extern struct real_map_lights_globals_prefix lights_globals_prefix __asm__("lights_globals");

/* (player_queues_new.c's, private there) */
struct real_map_server_update
{
	word action_count;
	short pad;
	struct player_action actions[1];
};

enum
{
	_allocation_bytes,
	_allocation_data,
	_allocation_pool,
	_allocation_lruv,
	_allocation_gpu,

	TAG_SLOT_BYTES = 0x01600000,
	REAL_MAP_LOCAL_RANDOM_SEED = 0x4C4F4341, /* "LOCA" */
	GAME_STATE_REGION_BYTES = 0x01600000,
};

struct real_map_allocation
{
	char name[32];
	short kind;
	byte *address;
	unsigned long size;
	unsigned long digest_at[REAL_MAP_CHECKPOINTS];
	boolean frame_coupled;
};

struct real_map_globals
{
	boolean recording;
	boolean loaded;
	boolean armed;
	long allocation_count;
	struct real_map_allocation allocations[REAL_MAP_MAXIMUM_ALLOCATIONS];
	char scenario_path[256];
	long tick;
	long frames;
	long frames_without_ticks;
	long most_ticks_in_a_frame;
	long player_spawn_tick;
	long first_divergent_tick;
	unsigned long chain;
	unsigned long digest_at[REAL_MAP_CHECKPOINTS];
	unsigned long tags_at_start;
	unsigned long tick_digests[REAL_MAP_TICKS];   /* the simulation's */
	unsigned long simulation_chain;
	unsigned long simulation_at[REAL_MAP_CHECKPOINTS];
	const unsigned long *reference;
	void (*observer)(long tick);
	unsigned long last_digest;
};

static struct real_map_globals real_map_globals;
static unsigned long digest_tables[2][256];

/* ---------- the digest */

static void digest_tables_initialize(
	void)
{
	unsigned long state = 0x48574942UL; /* "HWIB" */
	short table;
	short index;

	if (digest_tables[0][1])
		return;
	for (table = 0; table < 2; table++)
	{
		for (index = 0; index < 256; index++)
		{
			state = state * 1664525UL + 1013904223UL;
			digest_tables[table][index] = state ^ (state >> 15);
		}
	}

	return;
}

/* a word that points into the game state or the tag slot, as its region and
offset (native order) */
static unsigned long digest_canonical_word(
	unsigned long word)
{
	unsigned long game_state = (unsigned long)physical_memory_get_game_state_base_address();
	unsigned long tags = (unsigned long)physical_memory_get_tag_cache_base_address();

	if (word - game_state < GAME_STATE_REGION_BYTES)
		return 0x40000000UL | (word - game_state);
	if (word - tags < TAG_SLOT_BYTES)
		return 0x80000000UL | (word - tags);

	return word;
}

struct digest_state
{
	unsigned long low, high;
	unsigned long group_low, group_high;
	unsigned long group;
};

static void digest_group_end(
	struct digest_state *state)
{
	state->low = (state->low ^ state->group_low ^ state->group) * 16777619UL;
	state->high = (state->high + (state->group_high ^ (state->group << 7))) * 2654435761UL;
	state->group_low = 0;
	state->group_high = 0;

	return;
}

/* size bytes at address (4-byte aligned): 8-byte groups aligned to the
game state's base (whose base is page aligned) */
/* size bytes at data, whose place in the game state is address (they are
the same but for a canonical copy, digest_add_canonical) */
static void digest_add_at(
	struct digest_state *state,
	byte const *data,
	byte const *address,
	unsigned long size)
{
	unsigned long base = (unsigned long)physical_memory_get_game_state_base_address();
	unsigned long offset;

	for (offset = 0; offset + 4 <= size; offset += 4)
	{
		unsigned long word = digest_canonical_word(*(unsigned long const *)(data + offset));
		byte bytes[4];
		unsigned long group = ((unsigned long)(address + offset) - base) >> 3;
		short index;

		csmemcpy(bytes, &word, 4);
		if (group != state->group && (state->group_low || state->group_high))
			digest_group_end(state);
		state->group = group;
		for (index = 0; index < 4; index++)
		{
			state->group_low += digest_tables[0][bytes[index]];
			state->group_high += digest_tables[1][bytes[index]];
		}
	}
	for (; offset < size; offset++)
	{
		state->group_low += digest_tables[0][data[offset]];
		state->group_high += digest_tables[1][data[offset]];
	}

	return;
}

static void digest_add(
	struct digest_state *state,
	byte const *address,
	unsigned long size)
{
	digest_add_at(state, address, address, size);

	return;
}

/* ---------- in-memory bit-fields in the game state

ADR-018: a compiler allocates bit-fields in its own order (x86 from the low
bit, PowerPC from the high bit), which is allowed for state that never
leaves memory. The digest compares the state between the two, so it reads
each such unit by its fields: these units, each two 4-bit fields in one byte
(the first declared, then the second), are hashed low-field-first whatever
the compiler's order. The ones the game state holds (measured: the only
allocation whose bytes differed for this reason, HWI-015B):
- players globals' bsp_switch_state (players.h: local_player_triggered_switch,
  bsp_check_recursive_switch_ticks), byte 0x2F. */
struct nibble_bit_field_unit
{
	char const *allocation;
	unsigned long offset;
};

static struct nibble_bit_field_unit const nibble_bit_field_units[] =
{
	{ "players globals", 0x2F },
};

struct nibble_bit_field_probe
{
	char first : 4;
	byte second : 4;
};

/* whether this compiler puts a byte's first 4-bit field in its high nibble */
static boolean nibble_first_field_high(
	void)
{
	union
	{
		struct nibble_bit_field_probe fields;
		byte value;
	} probe;

	probe.value = 0;
	probe.fields.first = 1;

	return probe.value == 0x10;
}

/* an allocation's bytes, its nibble bit-field units read low-field-first */
static void digest_add_canonical(
	struct digest_state *state,
	struct real_map_allocation const *allocation,
	unsigned long size)
{
	unsigned long done = 0;
	short index;

	for (index = 0; index < NUMBEROF(nibble_bit_field_units); index++)
	{
		struct nibble_bit_field_unit const *unit = &nibble_bit_field_units[index];
		unsigned long group;
		byte copy[8];

		if (csstrcmp(allocation->name, unit->allocation) || unit->offset >= size || !nibble_first_field_high())
			continue;
		/* the 8-byte group holding the unit, from a canonical copy */
		group = unit->offset & ~7UL;
		if (group < done || group + 8 > size)
			continue;
		digest_add(state, allocation->address + done, group - done);
		csmemcpy(copy, allocation->address + group, 8);
		copy[unit->offset - group] = (byte)((copy[unit->offset - group] >> 4) | (copy[unit->offset - group] << 4));
		digest_add_at(state, copy, allocation->address + group, 8);
		done = group + 8;
	}
	digest_add(state, allocation->address + done, size - done);

	return;
}

static unsigned long digest_finish(
	struct digest_state *state)
{
	digest_group_end(state);

	return state->low ^ (state->high * 0x9E3779B1UL) ^ (state->high >> 13);
}

unsigned long real_map_digest_bytes(
	const void *address,
	unsigned long size)
{
	struct digest_state state;

	digest_tables_initialize();
	csmemset(&state, 0, sizeof(state));
	digest_add(&state, address, size);

	return digest_finish(&state);
}

static unsigned long allocation_digest(
	struct real_map_allocation const *allocation)
{
	struct digest_state state;

	csmemset(&state, 0, sizeof(state));
	switch (allocation->kind)
	{
	case _allocation_data:
	{
		struct data_array const *data = (struct data_array const *)allocation->address;
		unsigned long live = sizeof(struct data_array) + (unsigned long)MAX(data->count, 0) * data->size;

		digest_add(&state, allocation->address, MIN(live, allocation->size));
		break;
	}
	case _allocation_pool:
	{
		struct memory_pool const *pool = (struct memory_pool const *)allocation->address;
		struct memory_pool_block const *block;
		long blocks = 0;

		digest_add(&state, allocation->address, sizeof(struct memory_pool));
		for (block = pool->first_block; block && blocks < 0x10000; block = block->next_block, blocks++)
		{
			unsigned long at = (unsigned long)block - (unsigned long)allocation->address;

			if (at >= allocation->size || block->size <= 0 || (unsigned long)block->size > allocation->size - at)
				break;
			digest_add(&state, (byte const *)block, (unsigned long)block->size);
		}
		break;
	}
	default:
		digest_add_canonical(&state, allocation, allocation->size);
		break;
	}

	return digest_finish(&state);
}

/* the allocations whose state follows the render frames, not only the
ticks (measured: the only ones that differ between cadences, HWI-015B):
- game time globals: the scheduler's remainder of frame time (leftover_dt);
- game sound globals: game_sound_update, once a frame (game_frame);
- first person weapons: the view model; its idle pose timer (ticks_until_pose)
  draws on the engine's local random numbers, which the per-frame
  presentation draws on too, so more frames a tick shift it.
The simulation's digest leaves them out; the whole state's has them. */
static char const *const frame_coupled_allocations[] =
{
	"game time globals",
	"game sound globals",
	"first person weapons",
};

static boolean allocation_frame_coupled(
	struct real_map_allocation const *allocation)
{
	short index;

	for (index = 0; index < NUMBEROF(frame_coupled_allocations); index++)
	{
		if (!csstrcmp(allocation->name, frame_coupled_allocations[index]))
			return TRUE;
	}

	return FALSE;
}

static void crc_add_word(
	unsigned long *crc,
	unsigned long word)
{
	byte bytes[4];

	bytes[0] = (byte)word;
	bytes[1] = (byte)(word >> 8);
	bytes[2] = (byte)(word >> 16);
	bytes[3] = (byte)(word >> 24);
	crc_checksum_buffer(crc, bytes, 4);

	return;
}

/* the whole game state's digest; the simulation's (without the
frame-coupled allocations) in simulation */
static unsigned long game_state_digest(
	int checkpoint,
	unsigned long *simulation)
{
	unsigned long crc;
	long index;

	crc_new(&crc);
	crc_new(simulation);
	for (index = 0; index < real_map_globals.allocation_count; index++)
	{
		struct real_map_allocation *allocation = &real_map_globals.allocations[index];
		unsigned long digest = allocation_digest(allocation);

		crc_add_word(&crc, digest);
		if (!allocation->frame_coupled)
			crc_add_word(simulation, digest);
		if (checkpoint >= 0)
			allocation->digest_at[checkpoint] = digest;
	}

	return crc;
}

/* the multiset of size bytes: tag data holds scalars at any byte offset
(an animation's frames, a script's nodes) and unconverted bytes (vertex
payloads, text), which no fixed grouping keeps together in both byte orders
and which read as words differently in each, so the tag slot is compared by
its bytes' multiset, raw (weaker: it misses bytes only moved, not any
changed). Pointers are not made canonical here: two runs compare equal only
with the tag slot at the same address (the host maps the Wii's) */
static unsigned long digest_multiset(
	byte const *address,
	unsigned long size)
{
	unsigned long low = 0, high = 0;
	unsigned long offset;

	for (offset = 0; offset < size; offset++)
	{
		low += digest_tables[0][address[offset]];
		high += digest_tables[1][address[offset]];
	}

	return low ^ (high * 0x9E3779B1UL) ^ (high >> 13) ^ size;
}

/* the tag slot's loaded bytes (the tag data, the structure BSP: the rest of
the slot is the loader's fill), runtime fields included */
static unsigned long tag_slot_digest(
	void)
{
	struct wii_cache_load_report load;
	byte const *slot = physical_memory_get_tag_cache_base_address();
	unsigned long crc;
	long index;

	wii_cache_load_report_get(&load);
	crc_new(&crc);
	for (index = 0; index < load.range_count; index++)
	{
		crc_add_word(&crc, load.range_offset[index]);
		crc_add_word(&crc, digest_multiset(slot + load.range_offset[index], load.range_size[index]));
	}

	return crc;
}

/* ---------- the allocations (the wrapped game_state_* allocators) */

static void allocation_record(
	const char *name,
	short kind,
	void *address,
	unsigned long size)
{
	struct real_map_allocation *allocation;

	if (!real_map_globals.recording || !address || !size && kind != _allocation_lruv ||
		real_map_globals.allocation_count >= REAL_MAP_MAXIMUM_ALLOCATIONS)
	{
		return;
	}
	allocation = &real_map_globals.allocations[real_map_globals.allocation_count++];
	csmemset(allocation, 0, sizeof(*allocation));
	csstrncpy(allocation->name, name ? name : "", sizeof(allocation->name) - 1);
	allocation->name[sizeof(allocation->name) - 1] = 0;
	allocation->frame_coupled = allocation_frame_coupled(allocation);
	allocation->kind = kind;
	allocation->address = address;
	allocation->size = size;

	return;
}

void *__real_game_state_malloc(const char *name, const char *type, long size);
void *__real_game_state_gpu_malloc(const char *name, const char *type, long size);
struct data_array *__real_game_state_data_new(const char *name, short maximum_count, short size);
struct memory_pool *__real_game_state_memory_pool_new(const char *name, long size);
void *__real_game_state_lruv_cache_new(const char *name, long maximum_block_count, void *delete_block_proc,
	void *locked_block_proc, long page_count, long page_size_bits);

void *__wrap_game_state_malloc(
	const char *name,
	const char *type,
	long size)
{
	void *result = __real_game_state_malloc(name, type, size);

	allocation_record(name, _allocation_bytes, result, (unsigned long)size);
	return result;
}

void *__wrap_game_state_gpu_malloc(
	const char *name,
	const char *type,
	long size)
{
	void *result = __real_game_state_gpu_malloc(name, type, size);

	allocation_record(name, _allocation_gpu, result, (unsigned long)size);
	return result;
}

struct data_array *__wrap_game_state_data_new(
	const char *name,
	short maximum_count,
	short size)
{
	struct data_array *result = __real_game_state_data_new(name, maximum_count, size);

	allocation_record(name, _allocation_data, result, (unsigned long)data_allocation_size(maximum_count, size));
	return result;
}

struct memory_pool *__wrap_game_state_memory_pool_new(
	const char *name,
	long size)
{
	struct memory_pool *result = __real_game_state_memory_pool_new(name, size);

	allocation_record(name, _allocation_pool, result, (unsigned long)memory_pool_allocation_size(size));
	return result;
}

void *__wrap_game_state_lruv_cache_new(
	const char *name,
	long maximum_block_count,
	void *delete_block_proc,
	void *locked_block_proc,
	long page_count,
	long page_size_bits)
{
	void *result = __real_game_state_lruv_cache_new(name, maximum_block_count, delete_block_proc, locked_block_proc,
		page_count, page_size_bits);

	/* (its size: the lruv cache's header and data array, which the census
	measures as lruv_allocation_size) */
	allocation_record(name, _allocation_lruv, result, 0);
	return result;
}

void real_map_record_allocations(
	int on)
{
	if (on && !real_map_globals.recording)
		real_map_globals.allocation_count = 0;
	real_map_globals.recording = on ? TRUE : FALSE;

	return;
}

long real_map_allocation_count(
	void)
{
	return real_map_globals.allocation_count;
}

const char *real_map_allocation_name(
	long index)
{
	return real_map_globals.allocations[index].name;
}

unsigned long real_map_allocation_bytes(
	long index)
{
	return real_map_globals.allocations[index].size;
}

unsigned long real_map_allocation_digest(
	long index,
	int checkpoint)
{
	return real_map_globals.allocations[index].digest_at[checkpoint];
}

/* ---------- the scripted input */

/* a triangle wave from -1 to 1 over period ticks */
static real triangle(
	long tick,
	long period)
{
	long phase = tick % period;
	real fraction = (real)phase / (real)period;

	return fraction < 0.5f ? 4.0f * fraction - 1.0f : 3.0f - 4.0f * fraction;
}

static void scripted_action(
	long tick,
	struct player_action *action)
{
	csmemset(action, 0, sizeof(*action));
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	action->desired_facing.yaw = 0.6f * triangle(tick, 160);
	action->desired_facing.pitch = 0.15f * triangle(tick + 40, 90);
	if (tick >= 30 && tick < 120)
	{
		action->throttle.i = 1.0f;
	}
	else if (tick >= 120 && tick < 180)
	{
		action->throttle.i = 0.5f;
		action->throttle.j = 0.7f;
	}
	else if (tick >= 180 && tick < 240)
	{
		action->throttle.i = -1.0f;
	}
	else if (tick >= 240)
	{
		action->throttle.i = 0.8f;
		action->throttle.j = -0.4f;
	}
	if (tick == 60 || tick == 140 || tick == 200)
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
	if ((tick >= 90 && tick < 110) || (tick >= 210 && tick < 230))
	{
		SET_FLAG(action->control_flags, _unit_control_weapon_primary_trigger_bit, TRUE);
		action->primary_trigger = 1.0f;
	}
	if (tick == 160)
		SET_FLAG(action->control_flags, _unit_control_throw_grenade_bit, TRUE);
	if (tick >= 250 && tick < 270)
		SET_FLAG(action->control_flags, _unit_control_crouch_modifier_bit, TRUE);
	if (tick == 280)
		SET_FLAG(action->control_flags, _unit_control_weapon_reload_bit, TRUE);

	return;
}

void real_map_scripted_actions(
	void *server_update,
	long update_number)
{
	struct real_map_server_update *update = server_update;

	if (!real_map_globals.armed || !update || update->action_count < 1 || update_number < 0)
		return;
	/* the local player's queue is the first (the only player) */
	scripted_action(update_number, &update->actions[0]);

	return;
}

/* ---------- the tick */

static long local_player_unit(
	void)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player ? player->unit_index : NONE;
}

void real_map_after_tick(
	void)
{
	long tick;
	unsigned long digest;
	unsigned long simulation;
	int checkpoint = -1;

	if (!real_map_globals.armed || real_map_globals.tick >= REAL_MAP_TICKS)
		return;
	tick = ++real_map_globals.tick;
	switch (tick)
	{
	case 1: checkpoint = 0; break;
	case 30: checkpoint = 1; break;
	case 150: checkpoint = 2; break;
	case REAL_MAP_TICKS: checkpoint = 3; break;
	}
	digest = game_state_digest(checkpoint, &simulation);
	real_map_globals.tick_digests[tick - 1] = simulation;
	real_map_globals.last_digest = digest;
	crc_add_word(&real_map_globals.chain, digest);
	crc_add_word(&real_map_globals.simulation_chain, simulation);
	if (checkpoint >= 0)
	{
		real_map_globals.digest_at[checkpoint] = digest;
		real_map_globals.simulation_at[checkpoint] = simulation;
	}
	if (real_map_globals.reference && real_map_globals.first_divergent_tick == NONE &&
		real_map_globals.reference[tick - 1] != simulation)
	{
		real_map_globals.first_divergent_tick = tick;
	}
	if (real_map_globals.player_spawn_tick == NONE && local_player_unit() != NONE)
		real_map_globals.player_spawn_tick = tick;
	if (real_map_globals.observer)
		real_map_globals.observer(tick);

	return;
}

/* ---------- public code */

int real_map_load(
	const char *scenario_path)
{
	struct real_map_game_options options;

	if (real_map_globals.loaded)
		return FALSE;
	digest_tables_initialize();
	csstrncpy(real_map_globals.scenario_path, scenario_path, sizeof(real_map_globals.scenario_path) - 1);
	real_map_globals.scenario_path[sizeof(real_map_globals.scenario_path) - 1] = 0;
	game_connection_set(_game_connection_local);
	/* main_new_map's steps (the map is staged: precaching has nothing to do) */
	game_options_new((struct game_options *)&options);
	csstrncpy(options.map_name, scenario_path, sizeof(options.map_name) - 1);
	options.map_name[sizeof(options.map_name) - 1] = 0;
	input_flush();
	/* the lights' generation marker (object_lights.c lights_globals.marker:
	static storage, not the game state, counting up every tick for as long
	as the program runs, never reset with a map), which every light the map
	makes copies: each load starts it at 0, so a load's lights are the same
	whatever ran before (only its equality with a light's matters) */
	lights_globals_prefix.marker = 0;
	if (!game_load((struct game_options *)&options))
		return FALSE;
	game_initialize_for_new_map();
	/* create_local_players' (main.c) for one player on controller 0 */
	local_player_set_player_index(0, player_new(0, NONE, 0, NULL));
	real_map_globals.loaded = TRUE;
	real_map_globals.tags_at_start = tag_slot_digest();

	return TRUE;
}

int real_map_reset(
	void)
{
	if (!real_map_globals.loaded || real_map_globals.armed)
		return FALSE;
	/* main_reset_map_private's steps */
	scenario_switch_structure_bsp(0);
	game_dispose_from_old_map();
	input_flush();
	game_initialize_for_new_map();
	local_player_set_player_index(0, player_new(0, NONE, 0, NULL));

	return TRUE;
}

int real_map_begin(
	const unsigned long *reference_digests)
{
	if (!real_map_globals.loaded || real_map_globals.armed)
		return FALSE;
	real_map_globals.tick = 0;
	real_map_globals.frames = 0;
	real_map_globals.frames_without_ticks = 0;
	real_map_globals.most_ticks_in_a_frame = 0;
	real_map_globals.player_spawn_tick = NONE;
	real_map_globals.first_divergent_tick = NONE;
	real_map_globals.reference = reference_digests;
	csmemset(real_map_globals.digest_at, 0, sizeof(real_map_globals.digest_at));
	csmemset(real_map_globals.tick_digests, 0, sizeof(real_map_globals.tick_digests));
	crc_new(&real_map_globals.chain);
	crc_new(&real_map_globals.simulation_chain);
	csmemset(real_map_globals.simulation_at, 0, sizeof(real_map_globals.simulation_at));
	real_map_globals.last_digest = 0;
	/* the engine's local random numbers (presentation: a first-person
	weapon's idle poses, effects), which random_math_initialize seeds from
	the clock, as every machine draws its own: each run starts them from
	the same seed, as it gives the same input */
	*get_global_local_random_seed_address() = REAL_MAP_LOCAL_RANDOM_SEED;
	real_map_globals.armed = TRUE;
	game_time_start();
	game_initial_pulse();

	return TRUE;
}

int real_map_frame(
	float dt)
{
	short elapsed;

	game_time_update(dt);
	elapsed = game_time_get_elapsed();
	real_map_globals.frames++;
	if (!elapsed)
		real_map_globals.frames_without_ticks++;
	if (elapsed > real_map_globals.most_ticks_in_a_frame)
		real_map_globals.most_ticks_in_a_frame = elapsed;

	return real_map_globals.tick < REAL_MAP_TICKS;
}

void real_map_end(
	struct real_map_result *result)
{
	long unit = local_player_unit();
	short index;

	csmemset(result, 0, sizeof(*result));
	result->ticks = real_map_globals.tick;
	result->frames = real_map_globals.frames;
	result->frames_without_ticks = real_map_globals.frames_without_ticks;
	result->most_ticks_in_a_frame = real_map_globals.most_ticks_in_a_frame;
	result->digest = real_map_globals.last_digest;
	result->chain = real_map_globals.chain;
	result->simulation_digest = real_map_globals.tick ? real_map_globals.tick_digests[real_map_globals.tick - 1] : 0;
	result->simulation_chain = real_map_globals.simulation_chain;
	for (index = 0; index < REAL_MAP_CHECKPOINTS; index++)
	{
		result->digest_at[index] = real_map_globals.digest_at[index];
		result->simulation_at[index] = real_map_globals.simulation_at[index];
	}
	result->tags_at_start = real_map_globals.tags_at_start;
	result->tags_at_end = tag_slot_digest();
	result->random_seed = *get_global_random_seed_address();
	result->objects = object_header_data ? object_header_data->actual_count : 0;
	result->player_unit = unit;
	if (unit != NONE)
	{
		real_point3d origin;

		object_get_origin(unit, &origin);
		result->player_position[0] = origin.x;
		result->player_position[1] = origin.y;
		result->player_position[2] = origin.z;
	}
	result->player_spawn_tick = real_map_globals.player_spawn_tick;
	result->first_divergent_tick = real_map_globals.first_divergent_tick;
	real_map_globals.armed = FALSE;
	game_time_end();

	return;
}

const unsigned long *real_map_tick_digests(
	void)
{
	return real_map_globals.tick_digests;
}

void real_map_unload(
	void)
{
	if (!real_map_globals.loaded)
		return;
	game_dispose_from_old_map();
	game_unload();
	real_map_globals.loaded = FALSE;

	return;
}

int real_map_armed(
	void)
{
	return real_map_globals.armed;
}

void real_map_set_tick_observer(
	void (*observer)(long tick))
{
	real_map_globals.observer = observer;

	return;
}

void *real_map_allocation_address(
	long index)
{
	return real_map_globals.allocations[index].address;
}

unsigned long real_map_allocation_offset(
	long index)
{
	return (unsigned long)(real_map_globals.allocations[index].address -
		(byte *)physical_memory_get_game_state_base_address());
}
