/*
GAME_STATE.C

symbols in this file:
001AF250 0010:
	_dummy (0000)
001AF260 0010:
	_code_001af260 (0000)
001AF270 0010:
	_code_001af270 (0000)
001AF280 0020:
	_code_001af280 (0000)
001AF2A0 0010:
	_game_state_dispose (0000)
001AF2B0 00b0:
	_game_state_initialize_for_new_map (0000)
001AF360 0010:
	_game_state_dispose_from_old_map (0000)
001AF370 0020:
	_game_state_save (0000)
001AF390 0040:
	_game_state_revert (0000)
001AF3D0 0040:
	_game_state_save_to_persistent_storage (0000)
001AF410 0070:
	_game_state_test_persistent_storage (0000)
001AF480 0050:
	_game_state_save_core (0000)
001AF4D0 0020:
	_game_state_reverted (0000)
001AF4F0 0160:
	_game_state_header_valid (0000)
001AF650 0070:
	_game_state_allocation_record (0000)
001AF6C0 0020:
	_code_001af6c0 (0000)
001AF6E0 0110:
	_game_state_malloc (0000)
001AF7F0 0110:
	_game_state_gpu_malloc (0000)
001AF900 0040:
	_game_state_data_new (0000)
001AF940 0040:
	_game_state_memory_pool_new (0000)
001AF980 0050:
	_game_state_lruv_cache_new (0000)
001AF9D0 00f0:
	_game_state_try_and_load_from_persistent_storage (0000)
001AFAC0 00a0:
	_game_state_load_core (0000)
001AFB60 0050:
	_game_state_initialize (0000)
002A7E7C 0013:
	??_C@_0BD@BHHOKLIG@error?5writing?5?8?$CFs?8?$AA@ (0000)
002A7E90 000b:
	??_C@_0L@MGAFKEJL@saved?5?8?$CFs?8?$AA@ (0000)
002A7E9C 0025:
	??_C@_0CF@NHHJOMMA@checksum?5from?5map?5file?5doesn?8t?5m@ (0000)
002A7EC4 0021:
	??_C@_0CB@ELFCALCI@expected?5?$CD?$CFd?5players?5but?5got?5?$CD?$CFd@ (0000)
002A7EE8 001d:
	??_C@_0BN@CGADBPLM@allocation?5checksum?5mismatch?$AA@ (0000)
002A7F08 001b:
	??_C@_0BL@PFDNBAEN@expected?5?$CC?$CFs?$CC?5but?5got?5?$CC?$CFs?$CC?$AA@ (0000)
002A7F24 001f:
	??_C@_0BP@FJPMMOPC@expected?5build?5?$CD?$CFd?5but?5got?5?$CD?$CFd?$AA@ (0000)
002A7F44 0028:
	??_C@_0CI@KECEBGOP@c?3?2halo?2SOURCE?2saved?5games?2game_@ (0000)
002A7F6C 0013:
	??_C@_0BD@JHLLDANP@?$CF?540s?$CF?520s?$CF?510d?$CFs?6?$AA@ (0000)
002A7F80 0011:
	??_C@_0BB@DCECPIDG@d?3?2gamestate?4txt?$AA@ (0000)
002A7F98 0041:
	??_C@_0EB@JIJEJKFB@game_state_globals?4cpu_allocatio@ (0000)
002A7FDC 001b:
	??_C@_0BL@IDCLCKDD@?$CBgame_state_globals?4locked?$AA@ (0000)
002A7FF8 000a:
	??_C@_09KFGLKCMD@?$CB?$CIsize?$CG3?$CJ?$AA@ (0000)
002A8008 0041:
	??_C@_0EB@OBCPKDFA@game_state_globals?4gpu_allocatio@ (0000)
002A804C 000b:
	??_C@_0L@CIHKLGI@data?5array?$AA@ (0000)
002A8058 000b:
	??_C@_0L@HOFDEGMG@lruv?5cache?$AA@ (0000)
002A8064 0013:
	??_C@_0BD@OOCHHAFI@couldn?8t?5open?5?8?$CFs?8?$AA@ (0000)
002A8078 000c:
	??_C@_0M@NNNFMOGK@loaded?5?8?$CFs?8?$AA@ (0000)
00316840 003c:
	_data_00316840 (0000)
004D27B0 0020:
	_bss_004d27b0 (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries_windows.h"
#include "cseries/errors.h"
#include "real_math.h"
#include "console.h"
#include "game_state.h"
#include "game.h"
#include "tag_files.h"
#include "cache_files.h"
#include "scenario.h"
#include "main.h"
#include "objects.h"
#include "game_sound.h"
#include "sound_manager.h"
#include "observer.h"
#include "players.h"
#include "player_queues_new.h"
#include "rasterizer.h"
#include "recorded_animations.h"
#include "ai_debug.h"
#include "structures.h"
#include "director.h"
#include "hud_messaging.h"
#include "crc.h"
#include "data.h"
#include "lruv_cache.h"
#include "memory_pool.h"
#include "cluster_partitions.h"
#include "game_state_image.h"
#include "physical_memory_map.h"

void platform_log(const char *format, ...);

/* ---------- constants */

enum
{
	/* the native builds' larger game state (halo_port_capacity.h) */
	GAME_STATE_CPU_SIZE = HALO_PORT_GAME_STATE_CPU_SIZE,
	GAME_STATE_GPU_SIZE = HALO_PORT_GAME_STATE_GPU_SIZE,
	GAME_STATE_SIZE = GAME_STATE_CPU_SIZE+GAME_STATE_GPU_SIZE
};

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes */

void dummy(
	void);
static boolean game_state_header_valid(
	struct game_state_header *header,
	boolean halt_on_error);
static void game_state_allocation_record(
	const char *name,
	const char *type,
	long size,
	boolean gpu);
static void game_state_set_revert_time(
	void);

/* ---------- globals */

boolean recover_saved_games_hack;

static FILE* bss_004d27b0;

static struct
{
	void *base_address; // 0x0
	long cpu_allocation_size; // 0x4
	long gpu_allocation_size; // 0x8
	long allocation_size_checksum; // 0xC
	boolean locked; // 0x10
	boolean saved_game_valid; // 0x11
	long revert_time; // 0x14
	struct game_state_header *header; // 0x18
} game_state_globals = { 0 };

typedef void (*game_state_before_load_proc)(void);
typedef void (*game_state_after_load_proc)(void);
typedef void (*game_state_before_save_proc)(void);

static game_state_before_save_proc before_save_procs[] =
{
	dummy,
};

static game_state_before_load_proc before_load_procs[] =
{
	game_sound_clear,
	/* port: where the cluster lists' references are, which the game state
	being loaded does not hold */
	cluster_partitions_port_forget,
};

static game_state_after_load_proc after_load_procs[] =
{
	scenario_reload_structure_bsp_if_necessary,
	sound_stop_all,
	game_sound_restore,
	observer_initialize_for_new_map,
	update_queues_reset_and_fill_with_lies,
	rasterizer_decals_update_function_pointers,
	recorded_animations_clear_debug_storage,
	ai_debug_initialize_for_new_map,
	structure_detail_objects_flush,
	game_state_set_revert_time,
	player_control_fix_for_loaded_game_state,
	director_initialize_for_saved_game,
	scripted_hud_messages_clear
};

/* ---------- public code */

void dummy(
	void)
{
	return;
}

static void game_state_call_before_save_procs(
	void)
{
	game_state_before_save_proc *proc = before_save_procs;
	long i;

	for (i =NUMBEROF(before_save_procs); i>0; i--, proc++)
	{
		(*proc)();
	}

	return;
}

static void game_state_call_before_load_procs(
	void)
{
	game_state_before_load_proc *proc = before_load_procs;
	long i;

	for (i =NUMBEROF(before_load_procs); i>0; i--, proc++)
	{
		(*proc)();
	}

	return;
}

static void game_state_call_after_load_procs(
	void)
{
	game_state_after_load_proc *proc = after_load_procs;
	long i;

	for (i =NUMBEROF(after_load_procs); i>0; i--, proc++)
	{
		(*proc)();
	}

	return;
}

void game_state_dispose(
	void)
{
	game_state_free_buffer();
	game_state_close_file();

	return;
}

/* port: what last rewrote the game state, and when (game time), for
game_state_check_data_arrays' report */
static char const *game_state_last_event = "startup";
static long game_state_last_event_time = NONE;

static void game_state_note_event(
	char const *event)
{
	game_state_last_event = event;
	game_state_last_event_time = game_time_initialized() ? game_time_get() : NONE;
}

static void game_state_data_arrays_new_map(void);

void game_state_initialize_for_new_map(
	void)
{
	const char *name;

	game_state_note_event("map start");
	game_state_data_arrays_new_map();

	game_state_globals.locked = TRUE;
	game_state_globals.saved_game_valid = FALSE;
	game_state_globals.revert_time = NONE;

	memset(game_state_globals.header, 0, sizeof(*game_state_globals.header));

	name = tag_get_name(global_scenario_index);
	strcpy(game_state_globals.header->map_name, name);
	strcpy(game_state_globals.header->build_number, "01.01.14.2342");

	game_state_globals.header->player_count = player_spawn_count;
	game_state_globals.header->difficulty = game_difficulty_level_get();
	game_state_globals.header->cache_file_checksum = cache_files_get_checksum();
	game_state_globals.header->allocation_size_checksum = game_state_globals.allocation_size_checksum;

	return;
}

void game_state_dispose_from_old_map(
	void)
{
	return;
}

void game_state_save(
	void)
{
	game_state_call_before_save_procs();

	main_stop_time();
	game_state_globals.saved_game_valid = (game_state_write_to_file()!=FALSE);
	main_start_time();
	game_state_note_event("checkpoint saved");

	return;
}

/* port: whether game_state_revert has a saved state to go back to */
boolean game_state_port_saved_game_valid(
	void)
{
	return game_state_globals.saved_game_valid;
}

/* port: stamps the revert at the current game time again, after a network
co-op host moved its clock on past the revert (game_state_reverted
compares the two) */
void game_state_port_restamp_revert_time(
	void)
{
	game_state_globals.revert_time = game_time_get();
}

void game_state_revert(
	void)
{
	if (!game_state_globals.saved_game_valid && !recover_saved_games_hack)
	{
		main_reset_map();

		return;
	}

	game_state_note_event("checkpoint revert");
	/* port (HWI-076): the checkpoint is checked whole before anything
	changes, the before-load procedures included (game_state_port_image_load).
	One that is not taken leaves the game state as it is, and the map starts
	over */
	if (game_state_read_from_file())
	{
		game_state_call_after_load_procs();
	}
	else
	{
		game_state_globals.saved_game_valid = FALSE;
		main_reset_map();
	}

	return;
}

void game_state_save_to_persistent_storage(
	void)
{
	if (player_spawn_count==1)
	{
		game_state_note_event("save and quit");
		game_state_revert();
		game_state_write_to_persistent_storage(
			game_state_globals.base_address,
			&game_state_globals.header->checksum,
			sizeof(*game_state_globals.header),
			/* the whole of the native builds' larger game state */
			GAME_STATE_SIZE);
	}

	return;
}

boolean game_state_test_persistent_storage(
	char *map_name,
	short *difficulty,
	boolean *corrupted)
{
	struct game_state_header header;
	boolean success;

	if (game_state_read_header_from_persistent_storage(
		&header,
		&header.checksum,
		sizeof(*game_state_globals.header),
		/* the whole of the native builds' larger game state */
		GAME_STATE_SIZE,
		corrupted))
	{
		*difficulty = header.difficulty;
		csstrncpy(map_name, header.map_name, sizeof(header.map_name));
		map_name[sizeof(header.map_name) - 1] = 0;

		success = TRUE;
	}
	else
	{
		*difficulty = _game_difficulty_level_normal;
		strcpy(map_name, "");

		success = FALSE;
	}

	return success;
}

void game_state_save_core(
	const char *name)
{
	/* the whole of the native builds' larger game state */
	if (game_state_write_core(name, game_state_globals.base_address, GAME_STATE_SIZE))
	{
		console_printf(FALSE, "saved '%s'", name);
	}
	else
	{
		console_printf(FALSE, "error writing '%s'", name);
	}

	return;
}

boolean game_state_reverted(
	void)
{
	return (game_state_globals.revert_time==game_time_get());
}

static boolean game_state_header_valid(
	struct game_state_header *header,
	boolean halt_on_error)
{
	boolean valid = FALSE;

	if (csstrcmp(header->map_name, tag_get_name(global_scenario_index)))
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				409,
				FALSE,
				csprintf(temporary, "expected \"%s\" but got \"%s\"", tag_get_name(global_scenario_index), header->map_name));
		}
	}
	else if (header->allocation_size_checksum != game_state_globals.allocation_size_checksum)
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				413,
				FALSE,
				csprintf(temporary, "allocation checksum mismatch"));
		}
	}
	else if (header->player_count != player_spawn_count)
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				417,
				FALSE,
				csprintf(temporary, "expected #%d players but got #%d", player_spawn_count, header->player_count));
		}
	}
	else if (header->cache_file_checksum != cache_files_get_checksum())
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				422,
				FALSE,
				csprintf(temporary, "checksum from map file doesn't match"));
		}
	}
	else
	{
		valid = TRUE;
	}
	/* port: why a saved game is not loaded, for the log */
	if (!valid)
	{
		error(_error_silent, "the saved game is not taken: its header is of map '%s' (this is '%s'), allocations %08lx (%08lx), %d players (%d), map checksum %08lx (%08lx)",
			header->map_name, tag_get_name(global_scenario_index),
			(unsigned long)header->allocation_size_checksum, (unsigned long)game_state_globals.allocation_size_checksum,
			header->player_count, player_spawn_count,
			(unsigned long)header->cache_file_checksum, (unsigned long)cache_files_get_checksum());
	}

	return valid;
}

static void game_state_allocation_record(
	const char *name,
	const char *type,
	long size,
	boolean gpu)
{
	// The January compiler inlines this logger into both arena allocators while
	// retaining one out-of-line copy under its private address-derived name.
	FILE *file = bss_004d27b0;

	if (!file)
	{
		file = fopen("d:\\gamestate.txt", "w");
		bss_004d27b0 = file;
	}

	if (file)
	{
		fprintf(file, "% 40s% 20s% 10d%s\n", name, type, size, gpu ? "*" : "");
		fflush(bss_004d27b0);
	}

	return;
}

static void game_state_set_revert_time(
	void)
{
	game_state_globals.revert_time = game_time_get();
	game_time_set_paused(FALSE);

	return;
}

void *game_state_malloc(
	const char *name,
	const char *type,
	long size)
{
	byte *pointer;

	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 153, !(size&3));
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 156, !game_state_globals.locked);
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 159, game_state_globals.cpu_allocation_size+size<=GAME_STATE_CPU_SIZE);

	game_state_allocation_record(name, type, size, FALSE);

	pointer = (byte *)game_state_globals.base_address+game_state_globals.cpu_allocation_size;
	game_state_globals.cpu_allocation_size+= size;

	crc_checksum_buffer((unsigned long *)&game_state_globals.allocation_size_checksum, &size, sizeof(size));

	return pointer;
}

void *game_state_gpu_malloc(
	const char *name,
	const char *type,
	long size)
{
	byte *pointer;

	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 182, !(size&3));
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 185, !game_state_globals.locked);
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 188, game_state_globals.gpu_allocation_size+size<=GAME_STATE_GPU_SIZE);

	game_state_allocation_record(name, type, size, TRUE);

	game_state_globals.gpu_allocation_size+= size;
	pointer = (byte *)game_state_globals.base_address-game_state_globals.gpu_allocation_size+GAME_STATE_SIZE;

	crc_checksum_buffer((unsigned long *)&game_state_globals.allocation_size_checksum, &size, sizeof(size));

	return pointer;
}


/* ---------- port: the image of a saved game

A saved game (a checkpoint's savegame.bin, a core, the saved game of "save
and quit") is the canonical game-state image (game_state_image.c, HWI-076,
ADR-021): no native pointer in it, written and read in chunks, and checked
whole before any of it is taken. The data arrays, memory pools and lruv
caches made at startup, kept here, are its layout: where its pointers are,
and what an image must hold to be taken. */

enum
{
	_game_state_allocation_data,
	_game_state_allocation_memory_pool,
	_game_state_allocation_lruv_cache,

	MAXIMUM_GAME_STATE_ALLOCATIONS = 256,
};

struct game_state_allocation
{
	short kind;
	short maximum_count;
	short element_size;
	void *address;
	long size;
	long page_count;
	long page_size_bits;
	lruv_delete_block_proc delete_block_proc;
	lruv_locked_block_proc locked_block_proc;
	/* a data array's last tick in order this map (NONE: not yet), and
	whether it was reported since (game_state_check_data_arrays) */
	long good_time;
	boolean reported;
	/* (HWI-076) how the image writes it (game_state_image.h), and the
	pointer fields its elements' unit declared */
	short encoding;
	short pointer_field_count;
	short pointer_fields[GAME_STATE_IMAGE_MAXIMUM_POINTER_FIELDS];
};

static struct game_state_allocation game_state_allocations[MAXIMUM_GAME_STATE_ALLOCATIONS];
static long game_state_allocation_count;
static struct game_state_image_allocation game_state_image_allocations[MAXIMUM_GAME_STATE_ALLOCATIONS];

/* (HWI-076) the pointer words of the game state's globals their units
declared (game_state_port_pointer_field), in ascending offset */
enum
{
	MAXIMUM_GAME_STATE_POINTER_WORDS = 64,
};

static struct game_state_image_pointer_word game_state_pointer_words[MAXIMUM_GAME_STATE_POINTER_WORDS];
static long game_state_pointer_word_count;

static struct game_state_allocation *game_state_allocation_new(
	short kind,
	void *address)
{
	struct game_state_allocation *allocation;

	match_assert("game_state.c", 0, game_state_allocation_count < MAXIMUM_GAME_STATE_ALLOCATIONS);
	if (game_state_allocation_count >= MAXIMUM_GAME_STATE_ALLOCATIONS)
		return NULL;
	allocation = &game_state_allocations[game_state_allocation_count++];
	csmemset(allocation, 0, sizeof(*allocation));
	allocation->kind = kind;
	allocation->address = address;

	return allocation;
}

/* the game state as this build makes it, for game_state_image.c */
static void game_state_image_layout_get(
	struct game_state_image_layout *layout)
{
	long index;

	for (index = 0; index < game_state_allocation_count; index++)
	{
		struct game_state_allocation const *allocation = &game_state_allocations[index];
		struct game_state_image_allocation *image = &game_state_image_allocations[index];

		csmemset(image, 0, sizeof(*image));
		image->offset = (long)((byte const *)allocation->address - (byte const *)game_state_globals.base_address);
		image->encoding = allocation->encoding;
		image->maximum_count = allocation->maximum_count;
		image->element_size = allocation->element_size;
		image->pointer_field_count = allocation->pointer_field_count;
		csmemcpy(image->pointer_fields, allocation->pointer_fields, sizeof(image->pointer_fields));
		switch (allocation->kind)
		{
		case _game_state_allocation_data:
			image->kind = _game_state_image_data;
			image->size = data_allocation_size(allocation->maximum_count, allocation->element_size);
			break;
		case _game_state_allocation_memory_pool:
			image->kind = _game_state_image_memory_pool;
			image->pool_size = allocation->size;
			image->size = memory_pool_allocation_size(allocation->size);
			break;
		case _game_state_allocation_lruv_cache:
			image->kind = _game_state_image_lruv_cache;
			image->size = lruv_allocation_size(allocation->maximum_count);
			image->page_count = allocation->page_count;
			image->page_size_bits = allocation->page_size_bits;
			image->delete_block_proc = allocation->delete_block_proc;
			image->locked_block_proc = allocation->locked_block_proc;
			break;
		}
	}
	csmemset(layout, 0, sizeof(*layout));
	layout->base = game_state_globals.base_address;
	layout->size = GAME_STATE_SIZE;
	layout->cpu_used = game_state_globals.cpu_allocation_size;
	layout->gpu_used = game_state_globals.gpu_allocation_size;
	layout->allocation_size_checksum = (unsigned long)game_state_globals.allocation_size_checksum;
	layout->tags = physical_memory_get_tag_cache_base_address();
	layout->tags_size = layout->tags ? TAG_CACHE_SIZE : 0;
	layout->pointer_word_count = game_state_pointer_word_count;
	layout->pointer_words = game_state_pointer_words;
	layout->allocation_count = game_state_allocation_count;
	layout->allocations = game_state_image_allocations;
}

/* port (HWI-076): a pointer in each element in use of a data array of the
game state (one whose identifier is not 0), at that byte offset, which the
image is to write as a place: into the game state or the tag cache.
Pointers the game state's own structures hold (data arrays, memory pools,
lruv caches, script threads) need none */
void game_state_port_data_pointer_field(
	struct data_array *data,
	short offset)
{
	long index;

	for (index = 0; index < game_state_allocation_count; index++)
	{
		struct game_state_allocation *allocation = &game_state_allocations[index];

		if (allocation->address == data && allocation->kind == _game_state_allocation_data)
		{
			match_assert("game_state.c", 0, allocation->pointer_field_count < GAME_STATE_IMAGE_MAXIMUM_POINTER_FIELDS);
			if (allocation->pointer_field_count < GAME_STATE_IMAGE_MAXIMUM_POINTER_FIELDS)
				allocation->pointer_fields[allocation->pointer_field_count++] = offset;
			return;
		}
	}
	match_assert("game_state.c", 0, FALSE);
}

/* port (HWI-076): a pointer word of a game state global outside the data
arrays, memory pools and lruv caches (a game_state_malloc'd structure's),
which the image is to write as a place: kind is game_state_image.h's, a
pointer, or a union's word that holds a pointer or a value that reads as
none */
void game_state_port_pointer_field(
	void const *field,
	short kind)
{
	long offset = (long)((byte const *)field - (byte const *)game_state_globals.base_address);
	long index;

	match_assert("game_state.c", 0, game_state_pointer_word_count < MAXIMUM_GAME_STATE_POINTER_WORDS &&
		offset >= 0 && offset <= game_state_globals.cpu_allocation_size - 4 && !(offset & 3));
	if (game_state_pointer_word_count >= MAXIMUM_GAME_STATE_POINTER_WORDS || offset < 0 ||
		offset > game_state_globals.cpu_allocation_size - 4 || (offset & 3))
	{
		return;
	}
	/* (kept in ascending offset) */
	for (index = game_state_pointer_word_count; index > 0 && game_state_pointer_words[index - 1].offset > offset; index--)
		game_state_pointer_words[index] = game_state_pointer_words[index - 1];
	game_state_pointer_words[index].offset = offset;
	game_state_pointer_words[index].kind = kind;
	game_state_pointer_word_count++;
}

/* port (HWI-076): the game state written as an image */
boolean game_state_port_image_write(
	struct game_state_image_file const *file)
{
	struct game_state_image_layout layout;

	game_state_image_layout_get(&layout);

	return game_state_image_write(&layout, game_state_globals.header, file);
}

/* port (HWI-076): an image checked whole, then taken (game_state_image.h's
results); with check_header, only one of this map, build and players */
short game_state_port_image_load(
	struct game_state_image_file const *file,
	boolean check_header)
{
	struct game_state_image_layout layout;
	short result;

	game_state_image_layout_get(&layout);
	result = game_state_image_load(&layout, check_header ? game_state_globals.header : NULL, file,
		game_state_call_before_load_procs);
	if (result == _game_state_image_taken)
		error(_error_silent, "the saved game is taken");

	return result;
}

/* port (HWI-076): the SHA-256 the game state's image would have now (a
harness's check that a load gives back what was saved) */
boolean game_state_port_image_digest(
	unsigned long digest[8])
{
	struct game_state_image_layout layout;

	game_state_image_layout_get(&layout);

	return game_state_image_digest(&layout, game_state_globals.header, digest);
}

/* port (HWI-079): the places of the game state's pointers, as the image
finds them (game_state_image_pointer_places), for a harness's digest */
long game_state_port_image_pointer_places(
	long *offsets,
	long capacity)
{
	struct game_state_image_layout layout;

	game_state_image_layout_get(&layout);

	return game_state_image_pointer_places(&layout, offsets, capacity);
}

/* port (HWI-076): whether an image would be taken (nothing is) */
boolean game_state_port_image_check(
	struct game_state_image_file const *file,
	boolean check_header)
{
	struct game_state_image_layout layout;

	game_state_image_layout_get(&layout);

	return game_state_image_check(&layout, check_header ? game_state_globals.header : NULL, file);
}

struct data_array *game_state_data_new(
	const char *name,
	short maximum_count,
	short size)
{
	struct data_array *data;
	struct game_state_allocation *allocation;

	data = game_state_malloc(name, "data array", data_allocation_size(maximum_count, size));
	data_initialize(data, name, maximum_count, size);
	allocation = game_state_allocation_new(_game_state_allocation_data, data);
	if (allocation)
	{
		allocation->maximum_count = maximum_count;
		allocation->element_size = size;
		allocation->good_time = NONE;
		/* (HWI-076) hs_runtime.c's script threads, whose frames point into
		their own stacks */
		if (!csstrcmp(name, "hs thread") && size == 0x218)
			allocation->encoding = _game_state_image_encoding_script_threads;
	}

	return data;
}

/* port: a data array in order: an array (its signature, its own elements)
made valid for the map, with an identifier to give out and counts that fit
(data.c's data_usable) */
static boolean game_state_data_array_good(
	struct data_array const *data)
{
	return data->signature == 'd@t@' && data->data == (void *)(data + 1) && data->valid && data->next_identifier &&
		data->count >= 0 && data->count <= data->maximum_count &&
		data->actual_count >= 0 && data->actual_count <= data->count &&
		data->first_free_absolute_index >= 0 && data->first_free_absolute_index <= data->maximum_count;
}

static void game_state_data_arrays_new_map(
	void)
{
	long index;

	for (index = 0; index < game_state_allocation_count; index++)
	{
		game_state_allocations[index].good_time = NONE;
		game_state_allocations[index].reported = FALSE;
	}
}

/* port: each tick, every data array that was in order this map and no
longer is, reported once a map to halo.log (which a crash report carries):
when it went wrong (between its last tick in order and this one), how, and
what last rewrote the game state. A lights array found made for no map
(Sentry NATIVE-7) was the first sign of it; data.c now gives out no datum
from such an array, so this says where it came from. */
void game_state_check_data_arrays(
	void)
{
	long now = game_time_get();
	long index;

	for (index = 0; index < game_state_allocation_count; index++)
	{
		struct game_state_allocation *allocation = &game_state_allocations[index];
		struct data_array const *data = allocation->address;

		if (allocation->kind != _game_state_allocation_data)
			continue;
		if (game_state_data_array_good(data))
		{
			allocation->good_time = now;
			continue;
		}
		if (allocation->good_time == NONE || allocation->reported)
			continue;
		allocation->reported = TRUE;
		platform_log("game state: %.31s went wrong between ticks %ld and %ld of %.255s: signature %08lx, %s, "
			"next identifier %04x, count %d (%d used, first free %d) of %d; last rewritten by %s at tick %ld",
			data->name, allocation->good_time, now, game_state_globals.header->map_name,
			(unsigned long)data->signature, data->valid ? "valid" : "not valid",
			(unsigned short)data->next_identifier, data->count, data->actual_count,
			data->first_free_absolute_index, data->maximum_count, game_state_last_event,
			game_state_last_event_time);
		error(_error_silent, "game state: %.31s went wrong between ticks %ld and %ld (last rewritten by %s at tick %ld)",
			data->name, allocation->good_time, now, game_state_last_event, game_state_last_event_time);
	}
}

struct memory_pool *game_state_memory_pool_new(
	const char *name,
	long size)
{
	struct memory_pool *pool;

	pool = game_state_malloc(name, "memory pool", memory_pool_allocation_size(size));
	memory_pool_initialize(pool, name, size);
	{
		struct game_state_allocation *allocation = game_state_allocation_new(_game_state_allocation_memory_pool, pool);

		if (allocation)
			allocation->size = size;
	}

	return pool;
}

struct lruv_cache *game_state_lruv_cache_new(
	const char *name,
	long page_count,
	long page_size_bits,
	long maximum_block_count,
	void (*delete_block_proc)(long),
	boolean (*locked_block_proc)(long))
{
	struct lruv_cache *cache;

	cache = game_state_malloc(name, "lruv cache", lruv_allocation_size(maximum_block_count));
	lruv_initialize(cache, name, page_count, page_size_bits, maximum_block_count, delete_block_proc, locked_block_proc);
	{
		struct game_state_allocation *allocation = game_state_allocation_new(_game_state_allocation_lruv_cache, cache);

		if (allocation)
		{
			allocation->maximum_count = (short)maximum_block_count;
			allocation->page_count = page_count;
			allocation->page_size_bits = page_size_bits;
			allocation->delete_block_proc = delete_block_proc;
			allocation->locked_block_proc = locked_block_proc;
		}
	}

	return cache;
}

void game_state_try_and_load_from_persistent_storage(
	void)
{
	struct game_state_header header;

	if (game_state_read_header_from_persistent_storage(
			&header,
			&header.checksum,
			sizeof(header),
			GAME_STATE_SIZE,
			NULL)
		&& game_state_header_valid(&header, FALSE)
		&& (main_get_difficulty() == header.difficulty ||
			(error(_error_silent, "the saved game is not taken: it is of difficulty %d, this game of %d",
				header.difficulty, main_get_difficulty()), FALSE)))
	{
		game_state_note_event("saved game loaded");
		/* port (HWI-076): checked whole first (game_state_port_image_load) */
		if (game_state_read_from_persistent_storage(
			game_state_globals.base_address,
			GAME_STATE_SIZE))
		{
			game_difficulty_level_set(main_get_difficulty());
			game_state_call_after_load_procs();
			game_state_save();
		}
	}

	return;
}

void game_state_load_core(
	const char *name)
{
	struct game_state_header header;

	/* port (HWI-076): a core of another map, build, player count or byte
	order is refused, not halted on (game_state_header_valid says why in
	the log), and the game state is left as it is */
	if (game_state_read_core_header(name, &header, sizeof(header))
		&& game_state_header_valid(&header, FALSE))
	{
		/* port (HWI-076): checked whole first (game_state_port_image_load);
		one that is not taken leaves the game state as it is */
		if (game_state_read_core(
			name,
			game_state_globals.base_address,
			GAME_STATE_SIZE))
		{
			game_state_note_event("core loaded");
			console_printf(FALSE, "loaded '%s'", name);
			game_state_call_after_load_procs();
		}
		else
		{
			console_printf(FALSE, "couldn't load '%s'", name);
		}
	}
	else
	{
		console_printf(FALSE, "couldn't open '%s'", name);
	}

	return;
}

void game_state_initialize(
	void)
{
	crc_new(&game_state_globals.allocation_size_checksum);
	/* the native builds place their larger game state above the tag cache
	(halo_port_capacity.h, cache/physical_memory_map.c) */
	game_state_globals.base_address = game_state_allocate_buffer(HALO_PORT_GAME_STATE_BASE_ADDRESS, GAME_STATE_CPU_SIZE, GAME_STATE_GPU_SIZE);
	game_state_create_or_open_file();
	game_state_globals.header = game_state_malloc("header", NULL, sizeof(*game_state_globals.header));

	return;
}

/* ---------- private code */
