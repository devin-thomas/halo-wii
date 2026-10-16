/*
GAME_STATE_SCHEMA_CLASSES.C

port (HWI-085): the schemas (game_state_schema.h) of the engine's own state
classes whose definitions are public: the containers every data array,
memory pool and lruv cache of the game state has, and the globals and
elements game_state.c finds by the name they were allocated under. A class
whose definition is its unit's own declares its schema there
(game_state_port_schema: game_time.c, hs_runtime.c).

Each schema follows its structure's definition, checked against it here
field by field (offsetof) where the engine is built. Nothing here gives a
field a meaning its definition and its users do not: a field no code reads
or writes beyond datum_initialize's clearing (lightning.h's __unknown2, an
lruv block's user_data) keeps the type it is declared with.
*/

/* ---------- headers */

#ifndef GAME_STATE_SCHEMA_STANDALONE
#include "cseries/cseries.h"
#include "memory/data.h"
#include "memory/memory_pool.h"
#include "memory/lruv_cache.h"
#include "saved games/game_state.h"
#include "scenario/scenario.h"
#include "objects/widgets/lightning.h"
#include "objects/widgets/light_volumes.h"
#endif
#include "saved games/game_state_schema.h"

/* ---------- macros */

#define SCHEMA_FIELD GAME_STATE_SCHEMA_FIELD
#define SCHEMA GAME_STATE_SCHEMA_DEFINE

/* where the engine is built, each field's offset and the size, its
structure's (a field's offset is not of its definition: a compile error) */
#ifndef GAME_STATE_SCHEMA_STANDALONE
#define SCHEMA_CHECK(name, condition) typedef char name[(condition) ? 1 : -1]
#else
#define SCHEMA_CHECK(name, condition)
#endif

/* ---------- data.h: a data array's header */

static struct game_state_schema_field const data_array_header_fields[] =
{
	SCHEMA_FIELD("name", _game_state_schema_bytes, 0x0, 32),
	SCHEMA_FIELD("maximum_count", _game_state_schema_s16, 0x20, 1),
	SCHEMA_FIELD("size", _game_state_schema_s16, 0x22, 1),
	SCHEMA_FIELD("valid", _game_state_schema_boolean, 0x24, 1),
	SCHEMA_FIELD("identifier_zero_invalid", _game_state_schema_boolean, 0x25, 1),
	SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x26, 2),
	SCHEMA_FIELD("signature", _game_state_schema_u32, 0x28, 1),
	SCHEMA_FIELD("first_free_absolute_index", _game_state_schema_s16, 0x2C, 1),
	SCHEMA_FIELD("count", _game_state_schema_s16, 0x2E, 1),
	SCHEMA_FIELD("actual_count", _game_state_schema_s16, 0x30, 1),
	SCHEMA_FIELD("next_identifier", _game_state_schema_s16, 0x32, 1),
	SCHEMA_FIELD("data", _game_state_schema_state_pointer, 0x34, 1),
};
struct game_state_schema const game_state_schema_data_array_header =
	SCHEMA("data_array", 0x38, data_array_header_fields);

SCHEMA_CHECK(data_array_schema_size, sizeof(struct data_array) == 0x38);
SCHEMA_CHECK(data_array_schema_name, offsetof(struct data_array, name) == 0x0 && sizeof(((struct data_array *)0)->name) == 32);
SCHEMA_CHECK(data_array_schema_maximum_count, offsetof(struct data_array, maximum_count) == 0x20);
SCHEMA_CHECK(data_array_schema_element_size, offsetof(struct data_array, size) == 0x22);
SCHEMA_CHECK(data_array_schema_valid, offsetof(struct data_array, valid) == 0x24);
SCHEMA_CHECK(data_array_schema_identifier_zero_invalid, offsetof(struct data_array, identifier_zero_invalid) == 0x25);
SCHEMA_CHECK(data_array_schema_signature, offsetof(struct data_array, signature) == 0x28);
SCHEMA_CHECK(data_array_schema_first_free, offsetof(struct data_array, first_free_absolute_index) == 0x2C);
SCHEMA_CHECK(data_array_schema_count, offsetof(struct data_array, count) == 0x2E);
SCHEMA_CHECK(data_array_schema_actual_count, offsetof(struct data_array, actual_count) == 0x30);
SCHEMA_CHECK(data_array_schema_next_identifier, offsetof(struct data_array, next_identifier) == 0x32);
SCHEMA_CHECK(data_array_schema_data, offsetof(struct data_array, data) == 0x34);

/* ---------- memory_pool.h: a memory pool's header, and a block's */

static struct game_state_schema_field const memory_pool_header_fields[] =
{
	SCHEMA_FIELD("signature", _game_state_schema_u32, 0x0, 1),
	SCHEMA_FIELD("name", _game_state_schema_bytes, 0x4, 32),
	SCHEMA_FIELD("base_address", _game_state_schema_state_pointer, 0x24, 1),
	SCHEMA_FIELD("size", _game_state_schema_s32, 0x28, 1),
	SCHEMA_FIELD("free_size", _game_state_schema_s32, 0x2C, 1),
	SCHEMA_FIELD("first_block", _game_state_schema_state_pointer, 0x30, 1),
	SCHEMA_FIELD("last_block", _game_state_schema_state_pointer, 0x34, 1),
};
struct game_state_schema const game_state_schema_memory_pool_header =
	SCHEMA("memory_pool", 0x38, memory_pool_header_fields);

SCHEMA_CHECK(memory_pool_schema_size, sizeof(struct memory_pool) == 0x38);
SCHEMA_CHECK(memory_pool_schema_name, offsetof(struct memory_pool, name) == 0x4);
SCHEMA_CHECK(memory_pool_schema_base_address, offsetof(struct memory_pool, base_address) == 0x24);
SCHEMA_CHECK(memory_pool_schema_pool_size, offsetof(struct memory_pool, size) == 0x28);
SCHEMA_CHECK(memory_pool_schema_free_size, offsetof(struct memory_pool, free_size) == 0x2C);
SCHEMA_CHECK(memory_pool_schema_first_block, offsetof(struct memory_pool, first_block) == 0x30);
SCHEMA_CHECK(memory_pool_schema_last_block, offsetof(struct memory_pool, last_block) == 0x34);

static struct game_state_schema_field const memory_pool_block_fields[] =
{
	SCHEMA_FIELD("header_signature", _game_state_schema_u32, 0x0, 1),
	SCHEMA_FIELD("size", _game_state_schema_s32, 0x4, 1),
	SCHEMA_FIELD("reference", _game_state_schema_pool_reference, 0x8, 1),
	SCHEMA_FIELD("next_block", _game_state_schema_state_pointer, 0xC, 1),
	SCHEMA_FIELD("previous_block", _game_state_schema_state_pointer, 0x10, 1),
	SCHEMA_FIELD("trailer_signature", _game_state_schema_u32, 0x14, 1),
};
struct game_state_schema const game_state_schema_memory_pool_block =
	SCHEMA("memory_pool_block", 0x18, memory_pool_block_fields);

SCHEMA_CHECK(memory_pool_block_schema_size, sizeof(struct memory_pool_block) == 0x18);
SCHEMA_CHECK(memory_pool_block_schema_reference, offsetof(struct memory_pool_block, reference) == 0x8);
SCHEMA_CHECK(memory_pool_block_schema_next, offsetof(struct memory_pool_block, next_block) == 0xC);
SCHEMA_CHECK(memory_pool_block_schema_previous, offsetof(struct memory_pool_block, previous_block) == 0x10);
SCHEMA_CHECK(memory_pool_block_schema_trailer, offsetof(struct memory_pool_block, trailer_signature) == 0x14);

/* ---------- lruv_cache.h: an lruv cache's header, and a block's */

static struct game_state_schema_field const lruv_cache_header_fields[] =
{
	SCHEMA_FIELD("name", _game_state_schema_bytes, 0x0, 32),
	SCHEMA_FIELD("delete_block_proc", _game_state_schema_procedure, 0x20, 1),
	SCHEMA_FIELD("locked_block_proc", _game_state_schema_procedure, 0x24, 1),
	SCHEMA_FIELD("page_count", _game_state_schema_s32, 0x28, 1),
	SCHEMA_FIELD("page_size_bits", _game_state_schema_s32, 0x2C, 1),
	SCHEMA_FIELD("tick", _game_state_schema_s32, 0x30, 1),
	SCHEMA_FIELD("first_block_index", _game_state_schema_s32, 0x34, 1),
	SCHEMA_FIELD("last_block_index", _game_state_schema_s32, 0x38, 1),
	SCHEMA_FIELD("blocks", _game_state_schema_state_pointer, 0x3C, 1),
	SCHEMA_FIELD("signature", _game_state_schema_u32, 0x40, 1),
};
struct game_state_schema const game_state_schema_lruv_cache_header =
	SCHEMA("lruv_cache", 0x44, lruv_cache_header_fields);

SCHEMA_CHECK(lruv_cache_schema_size, sizeof(struct lruv_cache) == 0x44);
SCHEMA_CHECK(lruv_cache_schema_delete, offsetof(struct lruv_cache, delete_block_proc) == 0x20);
SCHEMA_CHECK(lruv_cache_schema_locked, offsetof(struct lruv_cache, locked_block_proc) == 0x24);
SCHEMA_CHECK(lruv_cache_schema_page_count, offsetof(struct lruv_cache, page_count) == 0x28);
SCHEMA_CHECK(lruv_cache_schema_page_size_bits, offsetof(struct lruv_cache, page_size_bits) == 0x2C);
SCHEMA_CHECK(lruv_cache_schema_tick, offsetof(struct lruv_cache, tick) == 0x30);
SCHEMA_CHECK(lruv_cache_schema_first, offsetof(struct lruv_cache, first_block_index) == 0x34);
SCHEMA_CHECK(lruv_cache_schema_last, offsetof(struct lruv_cache, last_block_index) == 0x38);
SCHEMA_CHECK(lruv_cache_schema_blocks, offsetof(struct lruv_cache, blocks) == 0x3C);
SCHEMA_CHECK(lruv_cache_schema_signature, offsetof(struct lruv_cache, signature) == 0x40);

static struct game_state_schema_field const lruv_cache_block_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("flags", _game_state_schema_s16, 0x2, 1),
	SCHEMA_FIELD("page_count", _game_state_schema_s32, 0x4, 1),
	SCHEMA_FIELD("first_page_index", _game_state_schema_s32, 0x8, 1),
	SCHEMA_FIELD("next_block_index", _game_state_schema_s32, 0xC, 1),
	SCHEMA_FIELD("previous_block_index", _game_state_schema_s32, 0x10, 1),
	SCHEMA_FIELD("last_used_tick", _game_state_schema_s32, 0x14, 1),
	SCHEMA_FIELD("user_data", _game_state_schema_s32, 0x18, 1),
};
struct game_state_schema const game_state_schema_lruv_cache_block =
	SCHEMA("lruv_cache_block", 0x1C, lruv_cache_block_fields);

SCHEMA_CHECK(lruv_cache_block_schema_size, sizeof(struct lruv_cache_block) == 0x1C);
SCHEMA_CHECK(lruv_cache_block_schema_flags, offsetof(struct lruv_cache_block, flags) == 0x2);
SCHEMA_CHECK(lruv_cache_block_schema_page_count, offsetof(struct lruv_cache_block, page_count) == 0x4);
SCHEMA_CHECK(lruv_cache_block_schema_first_page, offsetof(struct lruv_cache_block, first_page_index) == 0x8);
SCHEMA_CHECK(lruv_cache_block_schema_next, offsetof(struct lruv_cache_block, next_block_index) == 0xC);
SCHEMA_CHECK(lruv_cache_block_schema_previous, offsetof(struct lruv_cache_block, previous_block_index) == 0x10);
SCHEMA_CHECK(lruv_cache_block_schema_last_used, offsetof(struct lruv_cache_block, last_used_tick) == 0x14);
SCHEMA_CHECK(lruv_cache_block_schema_user_data, offsetof(struct lruv_cache_block, user_data) == 0x18);

/* ---------- game_state.h: the game state's header ("header") */

static struct game_state_schema_field const game_state_header_fields[] =
{
	SCHEMA_FIELD("allocation_size_checksum", _game_state_schema_u32, 0x0, 1),
	SCHEMA_FIELD("map_name", _game_state_schema_bytes, 0x4, 256),
	SCHEMA_FIELD("build_number", _game_state_schema_bytes, 0x104, 32),
	SCHEMA_FIELD("player_count", _game_state_schema_s16, 0x124, 1),
	SCHEMA_FIELD("difficulty", _game_state_schema_s16, 0x126, 1),
	SCHEMA_FIELD("cache_file_checksum", _game_state_schema_u32, 0x128, 1),
	SCHEMA_FIELD("unused", _game_state_schema_s32, 0x12C, 7),
	SCHEMA_FIELD("checksum", _game_state_schema_u32, 0x148, 1),
};
static struct game_state_schema const game_state_header_schema =
	SCHEMA("game_state_header", 0x14C, game_state_header_fields);

SCHEMA_CHECK(game_state_header_schema_size, sizeof(struct game_state_header) == 0x14C);
SCHEMA_CHECK(game_state_header_schema_map_name, offsetof(struct game_state_header, map_name) == 0x4);
SCHEMA_CHECK(game_state_header_schema_build_number, offsetof(struct game_state_header, build_number) == 0x104);
SCHEMA_CHECK(game_state_header_schema_player_count, offsetof(struct game_state_header, player_count) == 0x124);
SCHEMA_CHECK(game_state_header_schema_difficulty, offsetof(struct game_state_header, difficulty) == 0x126);
SCHEMA_CHECK(game_state_header_schema_cache_file_checksum, offsetof(struct game_state_header, cache_file_checksum) == 0x128);
SCHEMA_CHECK(game_state_header_schema_unused, offsetof(struct game_state_header, unused) == 0x12C);
SCHEMA_CHECK(game_state_header_schema_checksum, offsetof(struct game_state_header, checksum) == 0x148);

/* ---------- objects.c: the object name list ("object name list"): for each
of the scenario's object names, the object's datum handle (NONE: none) */

static struct game_state_schema_field const object_name_list_fields[] =
{
	SCHEMA_FIELD("object_index", _game_state_schema_datum_index, 0x0, 0x200),
};
static struct game_state_schema const object_name_list_schema =
	SCHEMA("object_name_list", 0x800, object_name_list_fields);

SCHEMA_CHECK(object_name_list_schema_count, MAXIMUM_OBJECT_NAMES_PER_SCENARIO == 0x200);

/* ---------- lightning.h, light_volumes.h: a lightning's and a light
volume's datum ("lightnings", "light volumes"): its identifier, a short no
code reads or writes, and its definition's tag handle */

static struct game_state_schema_field const lightning_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("__unknown2", _game_state_schema_s16, 0x2, 1),
	SCHEMA_FIELD("definition_index", _game_state_schema_datum_index, 0x4, 1),
};
static struct game_state_schema const lightning_schema =
	SCHEMA("lightning_datum", 0x8, lightning_fields);

SCHEMA_CHECK(lightning_schema_size, sizeof(struct lightning_datum) == 0x8);
SCHEMA_CHECK(lightning_schema_unknown2, offsetof(struct lightning_datum, __unknown2) == 0x2);
SCHEMA_CHECK(lightning_schema_definition, offsetof(struct lightning_datum, definition_index) == 0x4);

static struct game_state_schema_field const light_volume_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("__unknown2", _game_state_schema_s16, 0x2, 1),
	SCHEMA_FIELD("definition_index", _game_state_schema_datum_index, 0x4, 1),
};
static struct game_state_schema const light_volume_schema =
	SCHEMA("light_volume_datum", 0x8, light_volume_fields);

SCHEMA_CHECK(light_volume_schema_size, sizeof(struct light_volume_datum) == 0x8);
SCHEMA_CHECK(light_volume_schema_unknown2, offsetof(struct light_volume_datum, __unknown2) == 0x2);
SCHEMA_CHECK(light_volume_schema_definition, offsetof(struct light_volume_datum, definition_index) == 0x4);

/* ---------- the classes by name */

struct schema_class
{
	char const *name;
	short kind;
	struct game_state_schema const *schema;
};

static struct schema_class const schema_classes[] =
{
	{ "header", _game_state_schema_region_global, &game_state_header_schema },
	{ "object name list", _game_state_schema_region_global, &object_name_list_schema },
	{ "lightnings", _game_state_schema_region_data_array, &lightning_schema },
	{ "light volumes", _game_state_schema_region_data_array, &light_volume_schema },
};

static boolean schema_names_equal(
	char const *a,
	char const *b)
{
	while (*a && *a == *b)
	{
		a++;
		b++;
	}

	return *a == *b;
}

/* ---------- public code */

struct game_state_schema const *game_state_schema_class_find(
	char const *name,
	short kind,
	long size)
{
	long index;

	if (!name)
		return NULL;
	for (index = 0; index < (long)(sizeof(schema_classes) / sizeof(schema_classes[0])); index++)
	{
		struct schema_class const *entry = &schema_classes[index];

		if (entry->kind == kind && entry->schema->size == size && schema_names_equal(entry->name, name))
			return entry->schema;
	}

	return NULL;
}
