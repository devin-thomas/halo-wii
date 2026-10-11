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
#include "ai/actors.h"
#include "hs/hs.h"
#include "cutscene/cinematics.h"
#include "scenario/scenario.h"

#include "real_map_scenario.h"
#include "cache_files_wii.h"
#include "tag_schema.h"

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
unsigned long *get_tick_local_random_seed_address(void);
/* (main.c's, HWI-016C: the main loop's handling of the game's requests) */
void main_port_game_requests(boolean before_update);
boolean main_port_game_requests_left(void);

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

/* (unit_dialogue.c's: the next dialogue variant a unit gets) */
extern long sequential_counter;

/* (player_queues_new.c's struct server_update, private there: its action
count, then an action per player; room for the ports' players) */
struct real_map_server_update
{
	word action_count;
	short pad;
	struct player_action actions[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
};

void update_server_handle_client_update(long machine_index, struct player_action *actions);
void update_server_next_update(void);
void update_server_build_server_update(long machine_index, void *update, long *update_number);

enum
{
	_allocation_bytes,
	_allocation_data,
	_allocation_pool,
	_allocation_lruv,
	_allocation_gpu,

	TAG_SLOT_BYTES = 0x01600000,
	REAL_MAP_LOCAL_RANDOM_SEED = 0x4C4F4341, /* "LOCA" */
	REAL_MAP_TICK_LOCAL_RANDOM_SEED = 0x5449434B, /* "TICK" (random_math.c, HWI-016F) */
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
	boolean has_frame_coupled_fields;    /* (HWI-016F) frame_coupled_fields */
	boolean frame_coupled_hwi015b;       /* as HWI-015B classified it */
	short initial_identifier;          /* a data array's next identifier as the first map loaded */
	unsigned long raw_at[REAL_MAP_CHECKPOINTS];
};

struct real_map_globals
{
	boolean recording;
	boolean identifiers_taken;
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
	long ticks;                                   /* the run's */
	unsigned long tick_digests[REAL_MAP_MAXIMUM_TICKS]; /* the simulation's */
	unsigned long tick_digests_raw[REAL_MAP_MAXIMUM_TICKS]; /* the raw simulation's */
	unsigned long simulation_raw_chain;
	unsigned long raw_chain;
	boolean canonical_every_tick;
	unsigned long last_raw_digest;
	unsigned long simulation_chain;
	unsigned long simulation_at[REAL_MAP_CHECKPOINTS];
	/* (HWI-016F) the simulation's digests as HWI-015B classified it */
	unsigned long legacy_digest;
	unsigned long legacy_raw_digest;
	unsigned long legacy_raw_chain;
	const unsigned long *reference;
	void (*observer)(long tick);
	unsigned long last_digest;
	long updates_built;
	/* the state's summary after the last tick (a frame may run a tick past it) */
	unsigned long final_random_seed;
	long final_objects;
	long final_player_unit;
	real_point3d final_player_position;
	unsigned long final_tags;
	short final_structure_bsp_index;
	/* the harness's script command */
	long command_tick;
	char command[128];
	long command_ran_tick;
	long command_result;
	short command_bsp_before, command_bsp_after;
	/* HWI-030: more commands, each after its own tick */
	long more_command_ticks[REAL_MAP_MAXIMUM_MORE_COMMANDS];
	char more_commands[REAL_MAP_MAXIMUM_MORE_COMMANDS][32];
	int more_command_count;
	long more_commands_ran;
	short more_commands_bsp_after;
	long bsp_reads_at_begin;
	long requests_left;                /* frames ending with a won map or a reset asked for */
	/* (HWI-016F) effects and looping sounds made outside the ticks: their
	arrays' identifier counters as the last tick (or the frame) began
	game_frame, and what the frames added */
	short frame_mark_effects, frame_mark_looping_sounds;
	long frame_made_effects, frame_made_looping_sounds;
};

extern struct data_array *hs_thread_data;
/* (effects.c's and game_sound.c's) */
extern struct data_array *effect_data;
extern struct data_array *game_looping_sound_data;

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

/* (HWI-016C) the raw digest: words as they are, no pointer made canonical.
The canonical digest reads a word that lies in a region as a pointer; four
bytes that are not one (a byte flag beside zeros, two shorts: measured in
a10's objects) can read as one in one byte order only, and the same state
then digests differently on the Wii and the host. The raw digest compares
the Wii and the host of one launch, whose regions lie at the same addresses
(the host maps the Wii's); the canonical one stays for comparing launches
placed apart (their persistence check) and keeps HWI-015B's values. */
static boolean digest_words_raw;

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
		unsigned long word = digest_words_raw ? *(unsigned long const *)(data + offset) :
			digest_canonical_word(*(unsigned long const *)(data + offset));
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

/* (HWI-016C) a script thread as one group of its live bytes. A thread's
stack (hs_runtime.c hs_stack_allocate) packs its frames and their values at
2-byte offsets, so its words straddle any fixed 8-byte grouping and land in
other groups in the other byte order: each thread is compared by the
multiset of its live bytes (weaker, as the tag slot's: it misses bytes only
moved inside a thread). Live: the thread's fields and each frame on its
stack, its header and its values, found from the top frame down. Between
frames lie two bytes of alignment, and above the top frame popped frames'
bytes, which nothing reads, partly written over at other offsets: what is
left of a scalar written over in part is a different part of it in each
byte order. The pointers the thread holds to itself (its top frame, each
frame's previous frame and result) are offsets in the thread. */
struct real_map_hs_stack_frame
{
	struct real_map_hs_stack_frame *previous;
	long expression_index;
	void *result;
	short size;
	byte data[2];
};

struct real_map_hs_thread
{
	short identifier;
	byte type;
	byte flags;
	long script_index;
	long sleep_until;
	long previous_sleep_until;
	struct real_map_hs_stack_frame *stack;
	long result;
	byte stack_data[0x200];
};

typedef char verify_real_map_hs_thread_size[sizeof(struct real_map_hs_thread) == 0x218 ? 1 : -1];

enum
{
	HS_FRAME_HEADER_BYTES = 14,
	HS_THREAD_MAXIMUM_FRAMES = 64,
};

static void digest_group_bytes(
	struct digest_state *state,
	void const *data,
	unsigned long size)
{
	byte const *bytes = data;
	unsigned long offset;

	for (offset = 0; offset < size; offset++)
	{
		state->group_low += digest_tables[0][bytes[offset]];
		state->group_high += digest_tables[1][bytes[offset]];
	}

	return;
}

/* a pointer a thread holds: inside the thread, its offset there; else the
digest's word */
static void digest_group_thread_pointer(
	struct digest_state *state,
	byte const *datum,
	void const *pointer)
{
	unsigned long word = (unsigned long)pointer;

	if (word - (unsigned long)datum < sizeof(struct real_map_hs_thread))
		word = 0x20000000UL | (word - (unsigned long)datum);
	else if (!digest_words_raw)
		word = digest_canonical_word(word);
	digest_group_bytes(state, &word, sizeof(word));

	return;
}

static void digest_add_hs_thread(
	struct digest_state *state,
	byte const *datum,
	unsigned long group)
{
	struct real_map_hs_thread const *thread = (struct real_map_hs_thread const *)datum;
	struct real_map_hs_stack_frame const *frame = thread->stack;
	short frames = 0;

	if (state->group_low || state->group_high)
		digest_group_end(state);
	state->group = group;
	digest_group_bytes(state, datum, 16); /* identifier, type, flags, script, the sleep times */
	digest_group_thread_pointer(state, datum, thread->stack);
	digest_group_bytes(state, &thread->result, sizeof(thread->result));
	if (!thread->identifier)
		return;
	while (frame && frames++ < HS_THREAD_MAXIMUM_FRAMES)
	{
		unsigned long at = (unsigned long)((byte const *)frame - thread->stack_data);

		if ((byte const *)frame < thread->stack_data || at > sizeof(thread->stack_data) - HS_FRAME_HEADER_BYTES ||
			frame->size < 0 || (unsigned long)frame->size > sizeof(thread->stack_data) - HS_FRAME_HEADER_BYTES - at)
		{
			break;
		}
		digest_group_thread_pointer(state, datum, frame->previous);
		digest_group_bytes(state, &frame->expression_index, sizeof(frame->expression_index));
		digest_group_thread_pointer(state, datum, frame->result);
		digest_group_bytes(state, &frame->size, sizeof(frame->size));
		digest_group_bytes(state, frame->data, (unsigned long)frame->size);
		frame = frame->previous;
	}

	return;
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

		if (!csstrcmp(allocation->name, "hs thread"))
		{
			long index;

			digest_add(&state, allocation->address, sizeof(struct data_array));
			for (index = 0; index < data->count && sizeof(struct data_array) + (unsigned long)(index + 1) * data->size <= allocation->size; index++)
			{
				byte const *datum = (byte const *)data->data + index * data->size;

				if (data->size != sizeof(struct real_map_hs_thread))
				{
					digest_add(&state, datum, data->size);
					continue;
				}
				digest_add_hs_thread(&state, datum, 0x80000000UL | (unsigned long)index);
			}
			break;
		}
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
ticks. The simulation's digest leaves them out; the whole state's has them.
- What game_frame (game.c) advances by frame time: particles
  (particles_update), particle systems (particle_systems_update), contrails
  (contrails_update), the objects' widgets: flags, antennas, glows, light
  volumes and lightning (widgets_update), and the game sound
  (game_sound_update).
- Measured besides (HWI-015B: the only other allocations that differed
  between cadences): the scheduler's remainder of frame time (game time
  globals' leftover_dt); the player's screen effects (player effects: its
  flashes and damage indicators count down by the ticks of the frame,
  game_time_get_elapsed, and draw on the engine's local random numbers); and
  the first-person view model (its idle pose timer draws on those local
  random numbers, which the per-frame presentation draws on too).
- (HWI-016F) The object looping sounds. Only the game's sound reads them:
  game_sound_update (game_frame) services each once a frame by its frame
  count (state, last_audible_frame_index), starts and stops the background
  sound of the camera's cluster there, and deletes there the unattached
  ones that have played out (game_sound.c). Objects and scripts make and
  delete sounds in the ticks, but keep of one only its index, which only the
  game's sound reads back (objects.c's attachments, a looping sound
  definition's runtime_scripting_sound_index). The indices themselves come
  from the one array, so a background sound started a frame later takes
  its index a tick later (measured: a10 at 20 Hz, tick 1513); an object made
  in such a tick would keep a different index (not measured in a10; the
  objects stay the simulation's). */
static char const *const frame_coupled_allocations[] =
{
	"game time globals",
	"game sound globals",
	"first person weapons",
	"player effects",
	"particle",
	"particle systems",
	"particle system particles",
	"contrail",
	"contrail point",
	"widget",
	"flag",
	"antenna",
	"glow",
	"glow particles",
	"light volumes",
	"lightnings",
};

/* (HWI-016F's, beyond HWI-015B's above) */
static char const *const frame_coupled_allocations_hwi016f[] =
{
	"object looping sounds",
};

/* whether the simulation's digest leaves the allocation out: as HWI-015B
classified them (only), or as HWI-016F does */
static boolean allocation_frame_coupled(
	struct real_map_allocation const *allocation,
	boolean hwi015b_only)
{
	short index;

	for (index = 0; index < NUMBEROF(frame_coupled_allocations); index++)
	{
		if (!csstrcmp(allocation->name, frame_coupled_allocations[index]))
			return TRUE;
	}
	for (index = 0; !hwi015b_only && index < NUMBEROF(frame_coupled_allocations_hwi016f); index++)
	{
		if (!csstrcmp(allocation->name, frame_coupled_allocations_hwi016f[index]))
			return TRUE;
	}

	return FALSE;
}

/* (HWI-016F) fields that follow the render frames inside allocations the
simulation otherwise owns. The simulation's digest reads them as zeros; the
whole state's has them. Each is read only by the game's sound or the
renderer (HWI-016F's evidence traces every reader):
- scenario globals: the four local players' atmospheric fog and the sound
  environment, from the first fog state to the end (scenario.h). The sound
  environment steps toward the camera's cluster's once a frame
  (scenario_get_sound_environment, from game_sound_update) and only
  sound_manager_set_sound_environment reads it; the fog steps once a drawn
  view (scenario_get_atmospheric_fog, from the renderer). The structure bsp
  index before them is the simulation's.
element_size: the data array's element size it applies to, 0 for an
allocation's bytes. */
struct frame_coupled_field
{
	char const *allocation;
	unsigned short element_size;
	unsigned short offset;
	unsigned short size;
};

enum
{
	FRAME_COUPLED_FIELD_COPY_BYTES = 0x400,
};

static struct frame_coupled_field const frame_coupled_fields[] =
{
	{ "scenario globals", 0, offsetof(struct scenario_globals, atmospheric_fog),
		sizeof(struct scenario_globals) - offsetof(struct scenario_globals, atmospheric_fog) },
};

typedef char verify_scenario_globals_bytes[sizeof(struct scenario_globals) <= FRAME_COUPLED_FIELD_COPY_BYTES ? 1 : -1];

static boolean allocation_has_frame_coupled_fields(
	struct real_map_allocation const *allocation)
{
	short index;

	for (index = 0; index < NUMBEROF(frame_coupled_fields); index++)
	{
		if (!csstrcmp(allocation->name, frame_coupled_fields[index].allocation))
			return TRUE;
	}

	return FALSE;
}

/* zero the frame-coupled fields of an element (or an allocation's bytes) */
static void frame_coupled_fields_clear(
	char const *name,
	unsigned long element_size,
	byte *copy,
	unsigned long size)
{
	short index;

	for (index = 0; index < NUMBEROF(frame_coupled_fields); index++)
	{
		struct frame_coupled_field const *field = &frame_coupled_fields[index];

		if (!csstrcmp(name, field->allocation) && field->element_size == element_size &&
			(unsigned long)field->offset + field->size <= size)
		{
			csmemset(copy + field->offset, 0, field->size);
		}
	}

	return;
}

/* the allocation's digest with its frame-coupled fields read as zeros: the
same groups of the same words as allocation_digest's, from a copy */
static unsigned long allocation_simulation_digest(
	struct real_map_allocation const *allocation)
{
	static byte copy[FRAME_COUPLED_FIELD_COPY_BYTES];
	struct digest_state state;

	if (!allocation->has_frame_coupled_fields)
		return allocation_digest(allocation);
	csmemset(&state, 0, sizeof(state));
	if (allocation->kind == _allocation_data)
	{
		struct data_array const *data = (struct data_array const *)allocation->address;
		unsigned long size = (unsigned long)MAX(data->size, 0);
		long index;

		digest_add(&state, allocation->address, sizeof(struct data_array));
		for (index = 0; index < data->count && sizeof(struct data_array) + (unsigned long)(index + 1) * size <= allocation->size; index++)
		{
			byte const *datum = (byte const *)data->data + index * size;

			if (size > sizeof(copy))
			{
				digest_add(&state, datum, size);
				continue;
			}
			csmemcpy(copy, datum, size);
			frame_coupled_fields_clear(allocation->name, size, copy, size);
			digest_add_at(&state, copy, datum, size);
		}
	}
	else if (allocation->kind == _allocation_bytes && allocation->size <= sizeof(copy))
	{
		csmemcpy(copy, allocation->address, allocation->size);
		frame_coupled_fields_clear(allocation->name, 0, copy, allocation->size);
		digest_add_at(&state, copy, allocation->address, allocation->size);
	}
	else
	{
		return allocation_digest(allocation);
	}

	return digest_finish(&state);
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
frame-coupled allocations and fields) in simulation; HWI-015B's (without
the frame-coupled allocations only, HWI-016F's fields kept) in legacy */
static unsigned long game_state_digest(
	int checkpoint,
	unsigned long *simulation,
	unsigned long *simulation_raw,
	unsigned long *whole_raw,
	unsigned long *legacy,
	unsigned long *legacy_raw)
{
	unsigned long crc;
	long index;

	/* (the canonical digest every tick, or at the checkpoints only:
	real_map_set_canonical_every_tick) */
	boolean canonical = real_map_globals.canonical_every_tick || checkpoint >= 0;

	crc_new(&crc);
	crc_new(simulation);
	crc_new(simulation_raw);
	crc_new(whole_raw);
	crc_new(legacy);
	crc_new(legacy_raw);
	for (index = 0; index < real_map_globals.allocation_count; index++)
	{
		struct real_map_allocation *allocation = &real_map_globals.allocations[index];
		unsigned long digest = canonical ? allocation_digest(allocation) : 0;
		unsigned long raw;

		digest_words_raw = TRUE;
		raw = allocation_digest(allocation);
		digest_words_raw = FALSE;
		crc_add_word(&crc, digest);
		crc_add_word(whole_raw, raw);
		if (!allocation->frame_coupled_hwi015b)
		{
			crc_add_word(legacy, digest);
			crc_add_word(legacy_raw, raw);
		}
		if (!allocation->frame_coupled)
		{
			unsigned long simulation_digest = digest;
			unsigned long simulation_digest_raw = raw;

			if (allocation->has_frame_coupled_fields)
			{
				simulation_digest = canonical ? allocation_simulation_digest(allocation) : 0;
				digest_words_raw = TRUE;
				simulation_digest_raw = allocation_simulation_digest(allocation);
				digest_words_raw = FALSE;
			}
			crc_add_word(simulation, simulation_digest);
			crc_add_word(simulation_raw, simulation_digest_raw);
		}
		if (checkpoint >= 0)
		{
			allocation->digest_at[checkpoint] = digest;
			allocation->raw_at[checkpoint] = raw;
		}
	}

	return crc;
}

/* the multiset of size bytes: tag data holds scalars at any byte offset
(an animation's frames, a script's nodes) and unconverted bytes (vertex
payloads, text), which no fixed grouping keeps together in both byte orders
and which read as words differently in each, so the tag slot is compared by
its bytes' multiset, raw (weaker: it misses bytes only moved, not any
changed). Pointers into the tag slot or the game state are not made
canonical here: two runs compare equal only with the tag slot at the same
address (the host maps the Wii's). The one pointer into the program's own
static data the engine writes into tags as they load (tag_validate.c points
a reference without a tag at its empty name: measured, HWI-015B) is one
value: the program's addresses are each build's own */
static unsigned long digest_multiset(
	byte const *address,
	unsigned long size)
{
	unsigned long low = 0, high = 0;
	unsigned long offset;
	unsigned long empty_name = (unsigned long)tag_validate_empty_name_address();

	for (offset = 0; offset + 4 <= size; offset += 4)
	{
		unsigned long word = *(unsigned long const *)(address + offset);
		byte const *bytes = address + offset;
		static byte const image_pointer[4] = { 0x1e, 0x1e, 0x1e, 0x1e };
		short index;

		if (word == empty_name)
			bytes = image_pointer;
		for (index = 0; index < 4; index++)
		{
			low += digest_tables[0][bytes[index]];
			high += digest_tables[1][bytes[index]];
		}
	}
	for (; offset < size; offset++)
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
	allocation->frame_coupled = allocation_frame_coupled(allocation, FALSE);
	allocation->frame_coupled_hwi015b = allocation_frame_coupled(allocation, TRUE);
	allocation->has_frame_coupled_fields = allocation_has_frame_coupled_fields(allocation);
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
	if (on)
		real_map_globals.identifiers_taken = FALSE;
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

unsigned long real_map_allocation_raw_digest(
	long index,
	int checkpoint)
{
	return real_map_globals.allocations[index].raw_at[checkpoint];
}

unsigned long real_map_allocation_digest_now(
	long index)
{
	unsigned long digest;

	digest_words_raw = TRUE;
	digest = allocation_digest(&real_map_globals.allocations[index]);
	digest_words_raw = FALSE;

	return digest;
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

/* update_client_local_ticks (player_queues_new.c) as a local game runs it,
but with this run's script as the local player's input of each tick: the
local game takes its players' input once a frame and builds every tick's
update from it (update_server_next_update copies each queue's action), so
the action is handed to the server before each update is built rather than
once before all of them. The update's number is the run's count of updates
built, one a tick. */
void real_map_local_ticks(
	short ticks)
{
	while (ticks-- > 0)
	{
		struct player_action actions[MAXIMUM_LOCAL_PLAYERS];
		struct real_map_server_update update;
		long update_number;
		short index;

		for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
		{
			csmemset(&actions[index], 0, sizeof(actions[index]));
			actions[index].desired_weapon_index = NONE;
			actions[index].desired_grenade_index = NONE;
			actions[index].desired_zoom_level = NONE;
		}
		scripted_action(real_map_globals.updates_built++, &actions[0]);
		update_server_handle_client_update(0, actions);
		update_server_next_update();
		update_server_build_server_update(0, &update, &update_number);
	}

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

/* (HWI-016F) datums the frames make in arrays the ticks make datums in too:
an array's identifier counter counts every datum made in it, so what it
counts between a frame's last tick (or the frame's start) and the frame's
end was made by the frame (game_frame: a particle's effect, a background
sound) */
static short identifier_counter(
	struct data_array const *data)
{
	return data ? data->next_identifier : 0;
}

static void frame_made_mark(
	void)
{
	real_map_globals.frame_mark_effects = identifier_counter(effect_data);
	real_map_globals.frame_mark_looping_sounds = identifier_counter(game_looping_sound_data);

	return;
}

static void frame_made_count(
	void)
{
	real_map_globals.frame_made_effects +=
		(unsigned short)(identifier_counter(effect_data) - real_map_globals.frame_mark_effects);
	real_map_globals.frame_made_looping_sounds +=
		(unsigned short)(identifier_counter(game_looping_sound_data) - real_map_globals.frame_mark_looping_sounds);

	return;
}

void real_map_after_tick(
	void)
{
	long tick;
	unsigned long digest;
	unsigned long simulation;
	unsigned long simulation_raw;
	unsigned long whole_raw;
	unsigned long legacy;
	unsigned long legacy_raw;
	int checkpoint = -1;

	if (!real_map_globals.armed || real_map_globals.tick >= real_map_globals.ticks)
		return;
	frame_made_mark();
	tick = ++real_map_globals.tick;
	if (tick == real_map_globals.ticks)
		checkpoint = 3;
	else if (tick == 1)
		checkpoint = 0;
	else if (tick == 30)
		checkpoint = 1;
	else if (tick == 150)
		checkpoint = 2;
	digest = game_state_digest(checkpoint, &simulation, &simulation_raw, &whole_raw, &legacy, &legacy_raw);
	real_map_globals.legacy_digest = legacy;
	real_map_globals.legacy_raw_digest = legacy_raw;
	crc_add_word(&real_map_globals.legacy_raw_chain, legacy_raw);
	real_map_globals.last_raw_digest = whole_raw;
	crc_add_word(&real_map_globals.raw_chain, whole_raw);
	real_map_globals.tick_digests[tick - 1] = simulation;
	real_map_globals.tick_digests_raw[tick - 1] = simulation_raw;
	crc_add_word(&real_map_globals.simulation_raw_chain, simulation_raw);
	real_map_globals.last_digest = digest;
	crc_add_word(&real_map_globals.chain, digest);
	crc_add_word(&real_map_globals.simulation_chain, simulation);
	if (checkpoint >= 0)
	{
		real_map_globals.digest_at[checkpoint] = digest;
		real_map_globals.simulation_at[checkpoint] = simulation;
	}
	/* (against the reference run's raw digests) */
	if (real_map_globals.reference && real_map_globals.first_divergent_tick == NONE &&
		real_map_globals.reference[tick - 1] != simulation_raw)
	{
		real_map_globals.first_divergent_tick = tick;
	}
	if (real_map_globals.player_spawn_tick == NONE && local_player_unit() != NONE)
		real_map_globals.player_spawn_tick = tick;
	if (tick == real_map_globals.ticks)
	{
		long unit = local_player_unit();

		real_map_globals.final_structure_bsp_index = global_structure_bsp_index;
		real_map_globals.final_random_seed = *get_global_random_seed_address();
		real_map_globals.final_objects = object_header_data ? object_header_data->actual_count : 0;
		real_map_globals.final_player_unit = unit;
		if (unit != NONE)
			object_get_origin(unit, &real_map_globals.final_player_position);
		real_map_globals.final_tags = tag_slot_digest();
	}
	if (real_map_globals.observer)
		real_map_globals.observer(tick);
	/* the harness's script command, through the engine's own compiler and
	runtime (as a console command), between this tick and the next */
	if (real_map_globals.command_tick == tick && real_map_globals.command[0])
	{
		real_map_globals.command_ran_tick = tick;
		real_map_globals.command_bsp_before = global_structure_bsp_index;
		real_map_globals.command_result = hs_compile_and_evaluate(real_map_globals.command);
		real_map_globals.command_bsp_after = global_structure_bsp_index;
	}
	{
		int index;

		for (index = 0; index < real_map_globals.more_command_count; index++)
		{
			if (real_map_globals.more_command_ticks[index] == tick)
			{
				hs_compile_and_evaluate(real_map_globals.more_commands[index]);
				real_map_globals.more_commands_ran++;
				real_map_globals.more_commands_bsp_after = global_structure_bsp_index;
			}
		}
	}

	return;
}

void real_map_set_command(
	long tick,
	const char *command)
{
	real_map_globals.command_tick = tick;
	real_map_globals.command[0] = 0;
	real_map_globals.more_command_count = 0;
	if (command)
	{
		csstrncpy(real_map_globals.command, command, sizeof(real_map_globals.command) - 1);
		real_map_globals.command[sizeof(real_map_globals.command) - 1] = 0;
	}

	return;
}

int real_map_add_command(
	long tick,
	const char *command)
{
	int index = real_map_globals.more_command_count;

	if (index >= REAL_MAP_MAXIMUM_MORE_COMMANDS || !command ||
		csstrlen(command) >= sizeof(real_map_globals.more_commands[0]))
	{
		return FALSE;
	}
	real_map_globals.more_command_ticks[index] = tick;
	csstrcpy(real_map_globals.more_commands[index], command);
	real_map_globals.more_command_count++;

	return TRUE;
}

void real_map_progress_get(
	struct real_map_progress *progress)
{
	long unit = local_player_unit();

	csmemset(progress, 0, sizeof(*progress));
	progress->tick = real_map_globals.tick;
	progress->structure_bsp_index = global_structure_bsp_index;
	progress->cinematic = cinematic_in_progress() ? 1 : 0;
	progress->objects = object_header_data ? object_header_data->actual_count : 0;
	progress->actors = actor_data ? actor_data->actual_count : 0;
	progress->script_threads = hs_thread_data ? hs_thread_data->actual_count : 0;
	progress->player_unit = unit;
	if (unit != NONE)
	{
		real_point3d position;

		object_get_origin(unit, &position);
		progress->player_position[0] = position.x;
		progress->player_position[1] = position.y;
		progress->player_position[2] = position.z;
	}

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
	/* (HWI-016C) likewise the units' dialogue variants (unit_dialogue.c
	unit_dialogue_determine_variant: each new unit takes the next of its
	variants, by a counter that also counts on across maps): each load
	starts it at 0, as a fresh start has it */
	sequential_counter = 0;
	/* (HWI-016C) and every data array's identifier counter, which gives
	each new datum its identifier and counts on across maps for as long as
	the program runs (data_make_valid keeps it): each load starts it where
	it was as the first map loaded, so a load's identifiers (a campaign
	map's script globals, objects, effects) are the same whatever ran
	before */
	{
		long index;

		for (index = 0; index < real_map_globals.allocation_count; index++)
		{
			struct real_map_allocation *allocation = &real_map_globals.allocations[index];
			struct data_array *data = (struct data_array *)allocation->address;

			if (allocation->kind != _allocation_data)
				continue;
			if (!real_map_globals.identifiers_taken)
				allocation->initial_identifier = data->next_identifier;
			else
				data->next_identifier = allocation->initial_identifier;
		}
		real_map_globals.identifiers_taken = TRUE;
	}
	/* (HWI-016C) and the engine's local random numbers, which a campaign
	map's effects made as it loads draw on (as real_map_begin does for a
	run) */
	*get_global_local_random_seed_address() = REAL_MAP_LOCAL_RANDOM_SEED;
	*get_tick_local_random_seed_address() = REAL_MAP_TICK_LOCAL_RANDOM_SEED;
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
	const unsigned long *reference_digests,
	long ticks)
{
	struct wii_cache_load_report load;

	if (!real_map_globals.loaded || real_map_globals.armed || ticks < 1 || ticks > REAL_MAP_MAXIMUM_TICKS)
		return FALSE;
	wii_cache_load_report_get(&load);
	real_map_globals.bsp_reads_at_begin = load.bsp_reads;
	real_map_globals.requests_left = 0;
	real_map_globals.frame_made_effects = 0;
	real_map_globals.frame_made_looping_sounds = 0;
	real_map_globals.ticks = ticks;
	real_map_globals.command_ran_tick = NONE;
	real_map_globals.command_result = 0;
	real_map_globals.more_commands_ran = 0;
	real_map_globals.more_commands_bsp_after = NONE;
	real_map_globals.final_structure_bsp_index = NONE;
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
	crc_new(&real_map_globals.simulation_raw_chain);
	crc_new(&real_map_globals.legacy_raw_chain);
	real_map_globals.legacy_digest = 0;
	real_map_globals.legacy_raw_digest = 0;
	crc_new(&real_map_globals.raw_chain);
	real_map_globals.last_raw_digest = 0;
	real_map_globals.updates_built = 0;
	real_map_globals.final_player_unit = NONE;
	real_map_globals.final_tags = 0;
	csmemset(real_map_globals.simulation_at, 0, sizeof(real_map_globals.simulation_at));
	real_map_globals.last_digest = 0;
	/* the engine's local random numbers (presentation: a first-person
	weapon's idle poses, effects), which random_math_initialize seeds from
	the clock, as every machine draws its own: each run starts them from
	the same seed, as it gives the same input */
	*get_global_local_random_seed_address() = REAL_MAP_LOCAL_RANDOM_SEED;
	*get_tick_local_random_seed_address() = REAL_MAP_TICK_LOCAL_RANDOM_SEED;
	real_map_globals.armed = TRUE;
	game_time_start();
	game_initial_pulse();

	return TRUE;
}

int real_map_frame(
	float dt)
{
	short elapsed;

	/* the frame as main_loop runs it for the game: what the game asked of
	the main loop (a structure bsp switch a trigger volume asked for, a
	checkpoint save, a respawn ...), its time update with its ticks, then a
	checkpoint save in progress */
	main_port_game_requests(TRUE);
	frame_made_mark();
	game_time_update(dt);
	frame_made_count();
	main_port_game_requests(FALSE);
	if (main_port_game_requests_left())
		real_map_globals.requests_left++;
	elapsed = game_time_get_elapsed();
	real_map_globals.frames++;
	if (!elapsed)
		real_map_globals.frames_without_ticks++;
	if (elapsed > real_map_globals.most_ticks_in_a_frame)
		real_map_globals.most_ticks_in_a_frame = elapsed;

	return real_map_globals.tick < real_map_globals.ticks;
}

void real_map_end(
	struct real_map_result *result)
{
	long unit = real_map_globals.final_player_unit;
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
	result->simulation_raw_digest = real_map_globals.tick ? real_map_globals.tick_digests_raw[real_map_globals.tick - 1] : 0;
	result->simulation_raw_chain = real_map_globals.simulation_raw_chain;
	result->simulation_hwi015b_digest = real_map_globals.legacy_digest;
	result->simulation_hwi015b_raw_digest = real_map_globals.legacy_raw_digest;
	result->simulation_hwi015b_raw_chain = real_map_globals.legacy_raw_chain;
	result->raw_digest = real_map_globals.last_raw_digest;
	result->raw_chain = real_map_globals.raw_chain;
	for (index = 0; index < REAL_MAP_CHECKPOINTS; index++)
	{
		result->digest_at[index] = real_map_globals.digest_at[index];
		result->simulation_at[index] = real_map_globals.simulation_at[index];
	}
	result->tags_at_start = real_map_globals.tags_at_start;
	result->tags_at_end = real_map_globals.final_tags;
	result->random_seed = real_map_globals.final_random_seed;
	result->objects = real_map_globals.final_objects;
	result->player_unit = unit;
	if (unit != NONE)
	{
		result->player_position[0] = real_map_globals.final_player_position.x;
		result->player_position[1] = real_map_globals.final_player_position.y;
		result->player_position[2] = real_map_globals.final_player_position.z;
	}
	result->player_spawn_tick = real_map_globals.player_spawn_tick;
	result->first_divergent_tick = real_map_globals.first_divergent_tick;
	result->structure_bsp_index = real_map_globals.final_structure_bsp_index;
	{
		struct wii_cache_load_report load;

		wii_cache_load_report_get(&load);
		result->bsp_switches = load.bsp_reads - real_map_globals.bsp_reads_at_begin;
	}
	result->command_tick = real_map_globals.command_ran_tick;
	result->command_result = real_map_globals.command_result;
	result->command_bsp_before = real_map_globals.command_bsp_before;
	result->command_bsp_after = real_map_globals.command_bsp_after;
	result->more_commands_ran = real_map_globals.more_commands_ran;
	result->more_commands_bsp_after = real_map_globals.more_commands_bsp_after;
	result->requests_left = real_map_globals.requests_left;
	result->frame_made_effects = real_map_globals.frame_made_effects;
	result->frame_made_looping_sounds = real_map_globals.frame_made_looping_sounds;
	real_map_globals.armed = FALSE;
	game_time_end();

	return;
}

const unsigned long *real_map_tick_digests(
	void)
{
	return real_map_globals.tick_digests;
}

const unsigned long *real_map_tick_digests_raw(
	void)
{
	return real_map_globals.tick_digests_raw;
}

void real_map_set_canonical_every_tick(
	int every)
{
	real_map_globals.canonical_every_tick = every ? TRUE : FALSE;

	return;
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

void *real_map_tag_slot_address(
	void)
{
	return physical_memory_get_tag_cache_base_address();
}

unsigned long real_map_allocation_offset(
	long index)
{
	return (unsigned long)(real_map_globals.allocations[index].address -
		(byte *)physical_memory_get_game_state_base_address());
}
