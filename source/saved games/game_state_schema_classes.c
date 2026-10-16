/*
GAME_STATE_SCHEMA_CLASSES.C

port (HWI-085): the schemas (game_state_schema.h) of the engine's own state
classes whose definitions are public: the containers every data array,
memory pool and lruv cache of the game state has, and the globals and
elements game_state.c finds by the name they were allocated under. A class
whose definition is its unit's own declares its schema there
(game_state_port_schema: game_time.c, hs_runtime.c), or (a structure of
scalars only, its unit's definition checked against the schema there) here.

Each schema follows its structure's definition, checked against it here
field by field (offsetof) where the engine is built. Nothing here gives a
field a meaning its definition and its users do not: a field no code reads
or writes beyond datum_initialize's clearing (lightning.h's __unknown2, an
lruv block's user_data) keeps the type it is declared with, and only bytes
no code reads or writes at all (an allocation's or a structure's pad bytes)
are padding.
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
#include "math/real_math.h"
#include "interface/hud.h"
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

/* ---------- director.c: whether the camera is scripted ("director
scripting"): a boolean, in an allocation of 4 bytes whose other 3 no code
reads or writes (director_camera_scripted is a boolean *) */

static struct game_state_schema_field const director_scripting_fields[] =
{
	SCHEMA_FIELD("camera_scripted", _game_state_schema_boolean, 0x0, 1),
	SCHEMA_FIELD("(allocation)", _game_state_schema_padding, 0x1, 3),
};
static struct game_state_schema const director_scripting_schema =
	SCHEMA("director_scripting", 0x4, director_scripting_fields);

SCHEMA_CHECK(director_scripting_schema_boolean, sizeof(boolean) == 1);

/* ---------- hud.h: the HUD's scripted switches ("hud scripted globals") */

static struct game_state_schema_field const hud_scripted_globals_fields[] =
{
	SCHEMA_FIELD("show_hud", _game_state_schema_boolean, 0x0, 1),
	SCHEMA_FIELD("show_hud_help_text", _game_state_schema_boolean, 0x1, 1),
	SCHEMA_FIELD("pad", _game_state_schema_padding, 0x2, 2),
};
static struct game_state_schema const hud_scripted_globals_schema =
	SCHEMA("hud_scripted_globals", 0x4, hud_scripted_globals_fields);

SCHEMA_CHECK(hud_scripted_globals_schema_size, sizeof(struct hud_scripted_globals_definition) == 0x4);
SCHEMA_CHECK(hud_scripted_globals_schema_show_hud, offsetof(struct hud_scripted_globals_definition, show_hud) == 0x0);
SCHEMA_CHECK(hud_scripted_globals_schema_help_text, offsetof(struct hud_scripted_globals_definition, show_hud_help_text) == 0x1);
SCHEMA_CHECK(hud_scripted_globals_schema_pad, offsetof(struct hud_scripted_globals_definition, pad) == 0x2);

/* ---------- rasterizer.c: the scripted ambient reflection tint of models
("rasterizer model ambient reflection tint"): real_math.h's real_argb_color */

static struct game_state_schema_field const real_argb_color_fields[] =
{
	SCHEMA_FIELD("alpha", _game_state_schema_real, 0x0, 1),
	SCHEMA_FIELD("red", _game_state_schema_real, 0x4, 1),
	SCHEMA_FIELD("green", _game_state_schema_real, 0x8, 1),
	SCHEMA_FIELD("blue", _game_state_schema_real, 0xC, 1),
};
static struct game_state_schema const real_argb_color_schema =
	SCHEMA("real_argb_color", 0x10, real_argb_color_fields);

SCHEMA_CHECK(real_argb_color_schema_size, sizeof(real_argb_color) == 0x10);
SCHEMA_CHECK(real_argb_color_schema_alpha, offsetof(real_argb_color, alpha) == 0x0);
SCHEMA_CHECK(real_argb_color_schema_red, offsetof(real_argb_color, red) == 0x4);
SCHEMA_CHECK(real_argb_color_schema_green, offsetof(real_argb_color, green) == 0x8);
SCHEMA_CHECK(real_argb_color_schema_blue, offsetof(real_argb_color, blue) == 0xC);

/* ---------- object_lights.c: whether dynamic lights are drawn ("lights
globals"; its definition checked against this there) */

static struct game_state_schema_field const lights_game_globals_fields[] =
{
	SCHEMA_FIELD("render_lights", _game_state_schema_boolean, 0x0, 1),
	SCHEMA_FIELD("reserved01", _game_state_schema_padding, 0x1, 3),
};
static struct game_state_schema const lights_game_globals_schema =
	SCHEMA("lights_game_globals", 0x4, lights_game_globals_fields);

/* ---------- structure_runtime_decals.c: whether the structure's decals are
to be connected again ("structure decals"; checked there) */

static struct game_state_schema_field const structure_decals_globals_fields[] =
{
	SCHEMA_FIELD("reconnect_to_structure_bsp", _game_state_schema_boolean, 0x0, 1),
	SCHEMA_FIELD("pad", _game_state_schema_padding, 0x1, 3),
};
static struct game_state_schema const structure_decals_globals_schema =
	SCHEMA("structure_decals_globals", 0x4, structure_decals_globals_fields);

/* ---------- decals.c: each layer's and cluster's first decal, the first
disconnected one (decal datum handles, NONE none) and the counts ("decal
globals"; checked there) */

static struct game_state_schema_field const decal_globals_fields[] =
{
	/* [NUMBER_OF_DECAL_LAYERS 5][MAXIMUM_CLUSTERS_PER_STRUCTURE 512] */
	SCHEMA_FIELD("first_decal_indices", _game_state_schema_datum_index, 0x0, 5 * 512),
	SCHEMA_FIELD("first_disconnected_decal_index", _game_state_schema_datum_index, 0x2800, 1),
	SCHEMA_FIELD("locked_count", _game_state_schema_s32, 0x2804, 1),
	SCHEMA_FIELD("permanent_count", _game_state_schema_s32, 0x2808, 1),
};
static struct game_state_schema const decal_globals_schema =
	SCHEMA("decal_globals", 0x280C, decal_globals_fields);

/* ---------- breakable_surfaces.c: the breakable surfaces of each structure
bsp ("breakable surface globals"; checked there). A bsp's flags are 32 bytes
every user reads and writes as 8 longs (cseries.h's BIT_VECTOR_TEST_FLAG and
BIT_VECTOR_SET_FLAG through a long *: surface n is bit n % 32 of long n / 32),
so their value is those longs. The structure lays them out 1 byte in, so
they are a schema of their own here, its longs aligned in its instance (the
codec reads a field byte by byte, at any address) */

static struct game_state_schema_field const breakable_surface_flags_fields[] =
{
	SCHEMA_FIELD("bit_vector", _game_state_schema_u32, 0x0, 8),
};
static struct game_state_schema const breakable_surface_flags_schema =
	SCHEMA("breakable_surface_flags", 0x20, breakable_surface_flags_fields);

static struct game_state_schema_field const breakable_surface_datum_fields[] =
{
	SCHEMA_FIELD("vitality", _game_state_schema_real, 0x0, 1),
};
static struct game_state_schema const breakable_surface_datum_schema =
	SCHEMA("breakable_surface_datum", 0x4, breakable_surface_datum_fields);

static struct game_state_schema_field const breakable_surface_globals_fields[] =
{
	SCHEMA_FIELD("enabled", _game_state_schema_boolean, 0x0, 1),
	/* [MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO 16] */
	GAME_STATE_SCHEMA_STRUCT("breakable_surface_flags", &breakable_surface_flags_schema, 0x1, 16),
	SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x201, 3),
	/* [16][MAXIMUM_BREAKABLE_SURFACES_PER_MAP 256] */
	GAME_STATE_SCHEMA_STRUCT("breakable_surfaces", &breakable_surface_datum_schema, 0x204, 16 * 256),
};
static struct game_state_schema const breakable_surface_globals_schema =
	SCHEMA("breakable_surface_globals", 0x4204, breakable_surface_globals_fields);

/* ---------- game_sound.c: a looping sound's datum ("object looping sounds";
checked there): its state, its flags (a long of bits, TEST_FLAG and SET_FLAG
of the whole), its definition's tag handle and its object's datum handle
(NONE: unattached), the update it was last heard in (NONE: never) and where on
its object it plays. No pointer, no union, nothing the compiler pads */

static struct game_state_schema_field const game_looping_sound_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("state", _game_state_schema_s16, 0x2, 1),
	SCHEMA_FIELD("flags", _game_state_schema_u32, 0x4, 1),
	SCHEMA_FIELD("scale", _game_state_schema_real, 0x8, 1),
	SCHEMA_FIELD("definition_index", _game_state_schema_datum_index, 0xC, 1),
	SCHEMA_FIELD("object_index", _game_state_schema_datum_index, 0x10, 1),
	SCHEMA_FIELD("last_audible_frame_index", _game_state_schema_s32, 0x14, 1),
	SCHEMA_FIELD("function_index", _game_state_schema_s16, 0x18, 1),
	SCHEMA_FIELD("node_index", _game_state_schema_s16, 0x1A, 1),
	SCHEMA_FIELD("position", _game_state_schema_real, 0x1C, 3),
	SCHEMA_FIELD("forward", _game_state_schema_real, 0x28, 3),
};
static struct game_state_schema const game_looping_sound_schema =
	SCHEMA("game_looping_sound_datum", 0x34, game_looping_sound_fields);

/* ---------- game_sound.c: the update counter, and the music's looping sound
(a datum handle, NONE none) ("game sound globals"; checked there) */

static struct game_state_schema_field const game_sound_globals_fields[] =
{
	SCHEMA_FIELD("update_index", _game_state_schema_s32, 0x0, 1),
	SCHEMA_FIELD("music_looping_sound_index", _game_state_schema_datum_index, 0x4, 1),
};
static struct game_state_schema const game_sound_globals_schema =
	SCHEMA("game_sound_globals", 0x8, game_sound_globals_fields);

/* ---------- sound_classes.c: each of the 51 sound classes' gain, the gain it
goes to and the ticks left to get there ("sound classes", an allocation of
0x264 bytes; checked there). The 2 bytes the compiler puts after ticks are
named by no code (every user reads and writes a datum's fields by name, none
copies or compares a whole one) */

static struct game_state_schema_field const sound_class_datum_fields[] =
{
	SCHEMA_FIELD("desired_gain", _game_state_schema_real, 0x0, 1),
	SCHEMA_FIELD("gain", _game_state_schema_real, 0x4, 1),
	SCHEMA_FIELD("ticks", _game_state_schema_s16, 0x8, 1),
	SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0xA, 2),
};
static struct game_state_schema const sound_class_datum_schema =
	SCHEMA("sound_class_datum", 0xC, sound_class_datum_fields);

static struct game_state_schema_field const sound_classes_fields[] =
{
	/* [NUMBER_OF_SOUND_CLASSES 51] */
	GAME_STATE_SCHEMA_STRUCT("sound_classes", &sound_class_datum_schema, 0x0, 51),
};
static struct game_state_schema const sound_classes_schema =
	SCHEMA("sound_classes", 0x264, sound_classes_fields);

/* ---------- reference_lists.h: a reference list's datum (reference_list_new's
"%s reference": object_lists.c's "list object reference", and each cluster
partition's two, cluster_partitions.c's "cluster %s reference" and "%s cluster
reference"; checked there): what it refers to and the next reference in its
list (a datum handle of the same array, NONE the last). A reference of an
object list or of a cluster's list refers to a datum (an object's or a
light's datum handle); a reference of a datum's list of clusters refers to a
cluster, its index (cluster_partition_reconnect's, a short kept as a long).
The pad word is named by no code: datum_initialize clears it, and it is only
ever copied with its whole datum from another such (reference_list_copy), so
it is always zero */

static struct game_state_schema_field const data_reference_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("pad", _game_state_schema_padding, 0x2, 2),
	SCHEMA_FIELD("datum_index", _game_state_schema_datum_index, 0x4, 1),
	SCHEMA_FIELD("next_reference_index", _game_state_schema_datum_index, 0x8, 1),
};
static struct game_state_schema const data_reference_schema =
	SCHEMA("data_reference", 0xC, data_reference_fields);

static struct game_state_schema_field const cluster_reference_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("pad", _game_state_schema_padding, 0x2, 2),
	SCHEMA_FIELD("cluster_index", _game_state_schema_s32, 0x4, 1),
	SCHEMA_FIELD("next_reference_index", _game_state_schema_datum_index, 0x8, 1),
};
static struct game_state_schema const cluster_reference_schema =
	SCHEMA("cluster_reference", 0xC, cluster_reference_fields);

/* ---------- cluster_partitions.c: each of a structure's 512 clusters' first
reference in its partition's list of datums (a datum handle of "cluster %s
reference", NONE none), allocated under the partition's name ("collideable
object", "noncollideable object", "light"; checked there) */

static struct game_state_schema_field const cluster_first_references_fields[] =
{
	/* [MAXIMUM_CLUSTERS_PER_STRUCTURE 512] */
	SCHEMA_FIELD("cluster_first_data_references", _game_state_schema_datum_index, 0x0, 512),
};
static struct game_state_schema const cluster_first_references_schema =
	SCHEMA("cluster_first_data_references", 0x800, cluster_first_references_fields);

/* ---------- object_lists.h: an object list's datum ("object list header";
checked in object_lists.c): the scripts' references to it, its objects'
count and its first reference (a datum handle of "list object reference",
NONE none). Its pad word, as a reference's, is named by no code and only
cleared by datum_initialize */

static struct game_state_schema_field const object_list_header_fields[] =
{
	SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	SCHEMA_FIELD("pad", _game_state_schema_padding, 0x2, 2),
	SCHEMA_FIELD("reference_count", _game_state_schema_s16, 0x4, 1),
	SCHEMA_FIELD("count", _game_state_schema_s16, 0x6, 1),
	SCHEMA_FIELD("first_reference_index", _game_state_schema_datum_index, 0x8, 1),
};
static struct game_state_schema const object_list_header_schema =
	SCHEMA("object_list_header_datum", 0xC, object_list_header_fields);

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
	{ "director scripting", _game_state_schema_region_global, &director_scripting_schema },
	{ "hud scripted globals", _game_state_schema_region_global, &hud_scripted_globals_schema },
	{ "rasterizer model ambient reflection tint", _game_state_schema_region_global, &real_argb_color_schema },
	{ "lights globals", _game_state_schema_region_global, &lights_game_globals_schema },
	{ "structure decals", _game_state_schema_region_global, &structure_decals_globals_schema },
	{ "decal globals", _game_state_schema_region_global, &decal_globals_schema },
	{ "breakable surface globals", _game_state_schema_region_global, &breakable_surface_globals_schema },
	{ "object looping sounds", _game_state_schema_region_data_array, &game_looping_sound_schema },
	{ "game sound globals", _game_state_schema_region_global, &game_sound_globals_schema },
	{ "sound classes", _game_state_schema_region_global, &sound_classes_schema },
	{ "object list header", _game_state_schema_region_data_array, &object_list_header_schema },
	{ "list object reference", _game_state_schema_region_data_array, &data_reference_schema },
	{ "collideable object", _game_state_schema_region_global, &cluster_first_references_schema },
	{ "cluster collideable object reference", _game_state_schema_region_data_array, &data_reference_schema },
	{ "collideable object cluster reference", _game_state_schema_region_data_array, &cluster_reference_schema },
	{ "noncollideable object", _game_state_schema_region_global, &cluster_first_references_schema },
	{ "cluster noncollideable object reference", _game_state_schema_region_data_array, &data_reference_schema },
	{ "noncollideable object cluster reference", _game_state_schema_region_data_array, &cluster_reference_schema },
	{ "light", _game_state_schema_region_global, &cluster_first_references_schema },
	{ "cluster light reference", _game_state_schema_region_data_array, &data_reference_schema },
	{ "light cluster reference", _game_state_schema_region_data_array, &cluster_reference_schema },
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
