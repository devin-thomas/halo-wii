/* HWI-085: the typed game-state image's production codec
(source/saved games/game_state_schema.c and game_state_schema_classes.c),
compiled for the host as the engine builds it and checked against wire
bytes written out by hand below, not made by the codec:

- the same instance laid out little-endian and big-endian (native bytes
  given byte by byte) encodes to the same, independently specified, wire
  bytes, and decodes back to either layout;
- mixed widths, padding, reals' bits, datum handles, game-state and tag
  pointers, a script frame pointer at a 2-byte offset, a discriminated union;
- malformed schemas are refused with their reason, identities are
  deterministic and independent of padding and offsets;
- the completeness gate counts typed, unresolved (HS cells) and unclaimed
  (no schema, a memory pool's blocks, the GPU part) bytes exactly, and a
  partial schema set neither writes nor reads a stream;
- the engine's globals of scalars (director scripting, HUD switches, the
  ambient reflection tint, lights, structure decals, decals, breakable
  surfaces) are found by their whole allocation name, kind and size only,
  type every byte, encode to the same specified wire bytes from either byte
  order (a breakable surface bsp's flags as 8 longs), drop and zero only
  their pad bytes, and leave the gate shut beside one global without one;
- a whole typed stream of the engine's own classes (the game state header,
  a data array with free elements, an lruv cache) is the same from either
  byte order, is checked without being taken, is taken at another base, and
  is refused when damaged; transfers are bounded by the codec's scratch.

Exit 0 and "ok N cases" when every case holds. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char byte;
typedef unsigned char boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))
#define GAME_STATE_SCHEMA_STANDALONE
#include "saved games/game_state_schema.c"
#include "saved games/game_state_schema_classes.c"

static int failures;
static int cases;

#define CHECK(condition, ...) \
	do \
	{ \
		cases++; \
		if (!(condition)) \
		{ \
			failures++; \
			fprintf(stderr, "FAIL line %d: ", __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

#define BASE 0x80400000UL
#define TAGS 0x81000000UL
#define OTHER_BASE 0x90000000UL
#define OTHER_TAGS 0x91000000UL

/* ---------- native bytes, by hand */

static void put16(byte *at, unsigned long value, boolean big_endian)
{
	if (big_endian)
	{
		at[0] = (byte)(value >> 8);
		at[1] = (byte)value;
	}
	else
	{
		at[0] = (byte)value;
		at[1] = (byte)(value >> 8);
	}
}

static void put32(byte *at, unsigned long value, boolean big_endian)
{
	if (big_endian)
	{
		put16(at, value >> 16, TRUE);
		put16(at + 2, value & 0xFFFF, TRUE);
	}
	else
	{
		put16(at, value & 0xFFFF, FALSE);
		put16(at + 2, value >> 16, FALSE);
	}
}

static struct game_state_schema_state make_state(byte *memory, long size, unsigned long base, unsigned long tags,
	boolean big_endian)
{
	struct game_state_schema_state state;

	memset(&state, 0, sizeof(state));
	state.memory = memory;
	state.base_address = base;
	state.size = size;
	state.cpu_used = size;
	state.tags_address = tags;
	state.tags_size = 0x1000;
	state.big_endian = big_endian;

	return state;
}

static void dump_difference(char const *what, byte const *got, byte const *expected, long size)
{
	long index;

	for (index = 0; index < size; index++)
	{
		if (got[index] != expected[index])
		{
			fprintf(stderr, "  %s: first difference at %ld: %02X, expected %02X\n", what, index, got[index], expected[index]);
			return;
		}
	}
}

/* ---------- a class of every scalar type */

static struct game_state_schema_field const mixed_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("flags", _game_state_schema_u8, 0x2, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x3, 1),
	GAME_STATE_SCHEMA_FIELD("delta", _game_state_schema_s16, 0x4, 1),
	GAME_STATE_SCHEMA_FIELD("mask", _game_state_schema_u16, 0x6, 1),
	GAME_STATE_SCHEMA_FIELD("scale", _game_state_schema_real, 0x8, 1),
	GAME_STATE_SCHEMA_FIELD("owner", _game_state_schema_datum_index, 0xC, 1),
	GAME_STATE_SCHEMA_FIELD("next", _game_state_schema_state_pointer, 0x10, 1),
	GAME_STATE_SCHEMA_FIELD("definition", _game_state_schema_state_or_tag_pointer, 0x14, 1),
	GAME_STATE_SCHEMA_FIELD("tag", _game_state_schema_bytes, 0x18, 4),
	GAME_STATE_SCHEMA_FIELD("ticks", _game_state_schema_s32, 0x1C, 1),
};
static struct game_state_schema const mixed_schema = GAME_STATE_SCHEMA_DEFINE("mixed", 0x20, mixed_fields);

static byte const mixed_wire[31] =
{
	0x34, 0x12, 0xA5, 0xFE, 0xFF, 0xEF, 0xBE, 0x00, 0x00, 0xC0, 0x3F, 0x03, 0x00, 0x07, 0x00,
	0x00, 0x01, 0x00, 0x40, 0x20, 0x00, 0x00, 0x80, 0x48, 0x61, 0x6C, 0x6F, 0xFF, 0xFF, 0xFF, 0xFF
};
/* (next: BASE+0x100, definition: TAGS+0x20; the padding byte is 0x77 in
these, 0 when taken) */
static byte const mixed_little[0x20] =
{
	0x34, 0x12, 0xA5, 0x77, 0xFE, 0xFF, 0xEF, 0xBE, 0x00, 0x00, 0xC0, 0x3F, 0x03, 0x00, 0x07, 0x00,
	0x00, 0x01, 0x40, 0x80, 0x20, 0x00, 0x00, 0x81, 0x48, 0x61, 0x6C, 0x6F, 0xFF, 0xFF, 0xFF, 0xFF
};
static byte const mixed_big[0x20] =
{
	0x12, 0x34, 0xA5, 0x77, 0xFF, 0xFE, 0xBE, 0xEF, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x07, 0x00, 0x03,
	0x80, 0x40, 0x01, 0x00, 0x81, 0x00, 0x00, 0x20, 0x48, 0x61, 0x6C, 0x6F, 0xFF, 0xFF, 0xFF, 0xFF
};

static void check_mixed(void)
{
	byte memory[0x200];
	byte wire[31];
	byte native[0x20];
	byte expected[0x20];
	short endian;

	CHECK(game_state_schema_validate(&mixed_schema, NULL) == _game_state_schema_error_none, "mixed schema valid");
	CHECK(game_state_schema_wire_size(&mixed_schema) == 31, "mixed wire size %ld", game_state_schema_wire_size(&mixed_schema));
	for (endian = 0; endian < 2; endian++)
	{
		struct game_state_schema_state state = make_state(memory, sizeof(memory), BASE, TAGS, (boolean)endian);
		byte const *source = endian ? mixed_big : mixed_little;
		short error;

		memset(wire, 0xEE, sizeof(wire));
		error = game_state_schema_encode(&mixed_schema, &state, source, BASE + 0x40, wire, sizeof(wire));
		CHECK(error == _game_state_schema_stream_ok, "mixed encodes (%s): %s", endian ? "big" : "little",
			game_state_schema_stream_error_name(error));
		CHECK(!memcmp(wire, mixed_wire, sizeof(wire)), "mixed wire (%s) is the specified bytes", endian ? "big" : "little");
		dump_difference("mixed wire", wire, mixed_wire, sizeof(wire));

		/* back into this byte order: the same, the padding zero */
		memcpy(expected, source, sizeof(expected));
		expected[3] = 0;
		memset(native, 0xCC, sizeof(native));
		error = game_state_schema_decode(&mixed_schema, &state, mixed_wire, sizeof(mixed_wire), native, BASE + 0x40);
		CHECK(error == _game_state_schema_stream_ok && !memcmp(native, expected, sizeof(native)),
			"mixed decodes (%s): %s", endian ? "big" : "little", game_state_schema_stream_error_name(error));
	}
	/* taken at another base: its pointers are made there */
	{
		byte relocated[0x20];
		struct game_state_schema_state state = make_state(memory, sizeof(memory), OTHER_BASE, OTHER_TAGS, TRUE);
		short error = game_state_schema_decode(&mixed_schema, &state, mixed_wire, sizeof(mixed_wire), relocated, OTHER_BASE);

		CHECK(error == _game_state_schema_stream_ok &&
			relocated[16] == 0x90 && relocated[17] == 0x00 && relocated[18] == 0x01 && relocated[19] == 0x00 &&
			relocated[20] == 0x91 && relocated[21] == 0x00 && relocated[22] == 0x00 && relocated[23] == 0x20,
			"mixed pointers made at another base");
	}
	/* pointers that are no place */
	{
		struct game_state_schema_state state = make_state(memory, sizeof(memory), BASE, TAGS, FALSE);
		byte damaged[0x20];
		byte bad_wire[31];

		memcpy(damaged, mixed_little, sizeof(damaged));
		put32(damaged + 0x10, 0x12345678, FALSE);
		CHECK(game_state_schema_encode(&mixed_schema, &state, damaged, BASE, wire, sizeof(wire)) ==
			_game_state_schema_stream_pointer, "a pointer outside the state and the tags is refused");
		memcpy(damaged, mixed_little, sizeof(damaged));
		put32(damaged + 0x10, TAGS + 4, FALSE);
		CHECK(game_state_schema_encode(&mixed_schema, &state, damaged, BASE, wire, sizeof(wire)) ==
			_game_state_schema_stream_pointer, "a tag pointer where only a state pointer may be is refused");
		memcpy(bad_wire, mixed_wire, sizeof(bad_wire));
		bad_wire[18] = 0x7F; /* (next: 0x7F000100, neither place) */
		CHECK(game_state_schema_decode(&mixed_schema, &state, bad_wire, sizeof(bad_wire), NULL, BASE) ==
			_game_state_schema_stream_pointer, "a wire pointer that is no place is refused");
		memcpy(bad_wire, mixed_wire, sizeof(bad_wire));
		bad_wire[16] = 0x10; /* (next: 0x40001000, past the 0x200-byte state) */
		CHECK(game_state_schema_decode(&mixed_schema, &state, bad_wire, sizeof(bad_wire), NULL, BASE) ==
			_game_state_schema_stream_pointer, "a wire place past the state is refused");
		CHECK(game_state_schema_encode(&mixed_schema, &state, mixed_little, BASE, wire, 30) ==
			_game_state_schema_stream_layout, "a wire buffer not of the wire size is refused");
	}
}

/* ---------- a discriminated union */

static struct game_state_schema_field const arm_a_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u32, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("b", _game_state_schema_u32, 0x4, 1),
};
static struct game_state_schema const arm_a = GAME_STATE_SCHEMA_DEFINE("arm_a", 8, arm_a_fields);
static struct game_state_schema_field const arm_b_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("x", _game_state_schema_s16, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x2, 2),
	GAME_STATE_SCHEMA_FIELD("r", _game_state_schema_real, 0x4, 1),
};
static struct game_state_schema const arm_b = GAME_STATE_SCHEMA_DEFINE("arm_b", 8, arm_b_fields);
static struct game_state_schema_union_arm const tagged_arms[] = { { 1, &arm_a }, { 2, &arm_b } };
static struct game_state_schema_union const tagged_union = { 0, 2, tagged_arms };
static struct game_state_schema_field const tagged_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("kind", _game_state_schema_s16, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x2, 2),
	{ "value", _game_state_schema_union, 8, 0x4, NULL, &tagged_union, 0, 0 },
};
static struct game_state_schema const tagged_schema = GAME_STATE_SCHEMA_DEFINE("tagged", 12, tagged_fields);

static void check_union(void)
{
	static byte const b_wire[10] = { 0x02, 0x00, 0xFD, 0xFF, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00 };
	static byte const b_little[12] = { 0x02, 0x00, 0x00, 0x00, 0xFD, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0 };
	static byte const b_big[12] = { 0x00, 0x02, 0x00, 0x00, 0xFF, 0xFD, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00 };
	static byte const a_wire[10] = { 0x01, 0x00, 0x04, 0x03, 0x02, 0x01, 0xD0, 0xC0, 0xB0, 0xA0 };
	static byte const a_big[12] = { 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0 };
	byte memory[0x40];
	byte wire[10];
	byte native[12];
	struct game_state_schema_state little = make_state(memory, sizeof(memory), BASE, TAGS, FALSE);
	struct game_state_schema_state big = make_state(memory, sizeof(memory), BASE, TAGS, TRUE);
	byte bad[12];

	CHECK(game_state_schema_validate(&tagged_schema, NULL) == _game_state_schema_error_none, "union schema valid");
	CHECK(game_state_schema_wire_size(&tagged_schema) == 10, "union wire size: the widest arm");
	CHECK(game_state_schema_encode(&tagged_schema, &little, b_little, BASE, wire, 10) == _game_state_schema_stream_ok &&
		!memcmp(wire, b_wire, 10), "arm b from little-endian: the specified bytes, zero to the widest arm");
	CHECK(game_state_schema_encode(&tagged_schema, &big, b_big, BASE, wire, 10) == _game_state_schema_stream_ok &&
		!memcmp(wire, b_wire, 10), "arm b from big-endian: the same bytes");
	CHECK(game_state_schema_encode(&tagged_schema, &big, a_big, BASE, wire, 10) == _game_state_schema_stream_ok &&
		!memcmp(wire, a_wire, 10), "arm a from big-endian: the specified bytes");
	CHECK(game_state_schema_decode(&tagged_schema, &little, b_wire, 10, native, BASE) == _game_state_schema_stream_ok &&
		!memcmp(native, b_little, 12), "arm b taken little-endian");
	CHECK(game_state_schema_decode(&tagged_schema, &big, a_wire, 10, native, BASE) == _game_state_schema_stream_ok &&
		!memcmp(native, a_big, 12), "arm a taken big-endian");
	memcpy(bad, b_little, 12);
	bad[0] = 9;
	CHECK(game_state_schema_encode(&tagged_schema, &little, bad, BASE, wire, 10) == _game_state_schema_stream_union,
		"a discriminator of no arm is refused (written)");
	memcpy(wire, b_wire, 10);
	wire[0] = 9;
	CHECK(game_state_schema_decode(&tagged_schema, &little, wire, 10, NULL, BASE) == _game_state_schema_stream_union,
		"a discriminator of no arm is refused (read)");
	memcpy(wire, b_wire, 10);
	wire[9] = 1;
	CHECK(game_state_schema_decode(&tagged_schema, &little, wire, 10, NULL, BASE) == _game_state_schema_stream_padding,
		"a narrower arm's rest not zero is refused");
}

/* ---------- a script frame pointer, and unresolved fields */

static struct game_state_schema_field const frame_fields[] =
{
	GAME_STATE_SCHEMA_FRAME_POINTER("stack", 0x0, 0x8, 0x10),
	GAME_STATE_SCHEMA_FIELD("count", _game_state_schema_s32, 0x4, 1),
	GAME_STATE_SCHEMA_FIELD("stack_data", _game_state_schema_bytes, 0x8, 0x10),
};
static struct game_state_schema const frame_schema = GAME_STATE_SCHEMA_DEFINE("framed", 0x18, frame_fields);

/* (hs_runtime.c's hs_global_datum, as the engine declares its schema) */
static struct game_state_schema_field const cell_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("identifier", _game_state_schema_identifier, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("unused", _game_state_schema_s16, 0x2, 1),
	GAME_STATE_SCHEMA_FIELD("value", _game_state_schema_hs_cell, 0x4, 1),
};
static struct game_state_schema const cell_schema = GAME_STATE_SCHEMA_DEFINE("hs_global_datum", 8, cell_fields);

static void check_frames_and_cells(void)
{
	byte memory[0x40];
	struct game_state_schema_state big = make_state(memory, sizeof(memory), BASE, TAGS, TRUE);
	byte native[0x18];
	byte wire[0x18];
	byte taken[0x18];
	unsigned long instance = BASE + 0x20;

	memset(native, 0, sizeof(native));
	put32(native, instance + 8 + 6, TRUE);
	CHECK(game_state_schema_wire_size(&frame_schema) == 0x18, "frame wire size");
	CHECK(game_state_schema_encode(&frame_schema, &big, native, instance, wire, 0x18) == _game_state_schema_stream_ok &&
		wire[0] == 6 && wire[1] == 0 && wire[2] == 0 && wire[3] == 0, "a frame pointer is its offset in its stack");
	CHECK(game_state_schema_decode(&frame_schema, &big, wire, 0x18, taken, OTHER_BASE) == _game_state_schema_stream_ok &&
		taken[0] == 0x90 && taken[1] == 0x00 && taken[2] == 0x00 && taken[3] == 0x0E,
		"a frame pointer made in its instance at another address");
	put32(native, 0, TRUE);
	CHECK(game_state_schema_encode(&frame_schema, &big, native, instance, wire, 0x18) == _game_state_schema_stream_ok &&
		wire[0] == 0xFF && wire[1] == 0xFF && wire[2] == 0xFF && wire[3] == 0xFF, "no frame is 0xFFFFFFFF");
	put32(native, instance + 8 + 7, TRUE);
	CHECK(game_state_schema_encode(&frame_schema, &big, native, instance, wire, 0x18) ==
		_game_state_schema_stream_frame_pointer, "a frame at an odd offset is refused");
	put32(native, instance + 8 + 0x10, TRUE);
	CHECK(game_state_schema_encode(&frame_schema, &big, native, instance, wire, 0x18) ==
		_game_state_schema_stream_frame_pointer, "a frame past its stack is refused");
	memset(wire, 0, sizeof(wire));
	wire[0] = 0x10;
	CHECK(game_state_schema_decode(&frame_schema, &big, wire, 0x18, NULL, instance) ==
		_game_state_schema_stream_frame_pointer, "a wire frame past its stack is refused");
	{
		byte cell[8] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0xAB, 0xCD };
		byte cell_wire[8];

		CHECK(game_state_schema_encode(&cell_schema, &big, cell, BASE, cell_wire, 8) == _game_state_schema_stream_unresolved,
			"an HS cell is never written");
		memset(cell_wire, 0, sizeof(cell_wire));
		CHECK(game_state_schema_decode(&cell_schema, &big, cell_wire, 8, NULL, BASE) == _game_state_schema_stream_unresolved,
			"an HS cell is never taken");
	}
}

/* ---------- malformed schemas */

static struct game_state_schema_field const one_u32[] = { GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u32, 0, 1) };
static struct game_state_schema_field const bad_type[] = { GAME_STATE_SCHEMA_FIELD("a", 99, 0, 4) };
static struct game_state_schema_field const bad_count[] = { GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u32, 0, 0) };
static struct game_state_schema_field const bad_name[] = { GAME_STATE_SCHEMA_FIELD(NULL, _game_state_schema_u32, 0, 1) };
static struct game_state_schema_field const bad_bounds[] =
{
	GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u32, 0, 1),
	GAME_STATE_SCHEMA_FIELD("b", _game_state_schema_u32, 4, 2),
};
static struct game_state_schema_field const bad_overlap[] =
{
	GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u32, 0, 1),
	GAME_STATE_SCHEMA_FIELD("b", _game_state_schema_u16, 2, 1),
	GAME_STATE_SCHEMA_FIELD("c", _game_state_schema_u16, 6, 1),
};
static struct game_state_schema_field const bad_hole[] =
{
	GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u16, 0, 1),
	GAME_STATE_SCHEMA_FIELD("b", _game_state_schema_u32, 4, 1),
};
static struct game_state_schema_field const bad_tail[] = { GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u32, 0, 1) };
static struct game_state_schema_field const bad_alignment[] =
{
	GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_u16, 0, 1),
	GAME_STATE_SCHEMA_FIELD("b", _game_state_schema_u32, 2, 1),
	GAME_STATE_SCHEMA_FIELD("c", _game_state_schema_u16, 6, 1),
};
static struct game_state_schema_field const bad_nested[] = { GAME_STATE_SCHEMA_STRUCT("a", NULL, 0, 1) };
static struct game_state_schema_field const bad_stray_schema[] =
{
	{ "a", _game_state_schema_u32, 1, 0, &arm_a, NULL, 0, 0 },
};
extern struct game_state_schema const recursive_schema;
static struct game_state_schema_field const recursive_fields[] = { GAME_STATE_SCHEMA_STRUCT("self", &recursive_schema, 0, 1) };
struct game_state_schema const recursive_schema = GAME_STATE_SCHEMA_DEFINE("recursive", 8, recursive_fields);
static struct game_state_schema_union const union_late = { 1, 2, tagged_arms };
static struct game_state_schema_field const bad_union_order[] =
{
	GAME_STATE_SCHEMA_FIELD("kind", _game_state_schema_s16, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x2, 2),
	{ "value", _game_state_schema_union, 8, 0x4, NULL, &union_late, 0, 0 },
};
static struct game_state_schema_union const union_by_real = { 0, 2, tagged_arms };
static struct game_state_schema_field const bad_union_discriminator[] =
{
	GAME_STATE_SCHEMA_FIELD("kind", _game_state_schema_real, 0x0, 1),
	{ "value", _game_state_schema_union, 8, 0x4, NULL, &union_by_real, 0, 0 },
};
static struct game_state_schema_union_arm const duplicate_arms[] = { { 1, &arm_a }, { 1, &arm_b } };
static struct game_state_schema_union const union_duplicate = { 0, 2, duplicate_arms };
static struct game_state_schema_field const bad_union_duplicate[] =
{
	GAME_STATE_SCHEMA_FIELD("kind", _game_state_schema_s32, 0x0, 1),
	{ "value", _game_state_schema_union, 8, 0x4, NULL, &union_duplicate, 0, 0 },
};
static struct game_state_schema_union const union_small = { 0, 2, tagged_arms };
static struct game_state_schema_field const bad_union_size[] =
{
	GAME_STATE_SCHEMA_FIELD("kind", _game_state_schema_s32, 0x0, 1),
	{ "value", _game_state_schema_union, 12, 0x4, NULL, &union_small, 0, 0 },
};
static struct game_state_schema_union const union_empty = { 0, 0, tagged_arms };
static struct game_state_schema_field const bad_union_empty[] =
{
	GAME_STATE_SCHEMA_FIELD("kind", _game_state_schema_s32, 0x0, 1),
	{ "value", _game_state_schema_union, 8, 0x4, NULL, &union_empty, 0, 0 },
};
static struct game_state_schema_field const bad_frame[] =
{
	GAME_STATE_SCHEMA_FRAME_POINTER("stack", 0x0, 0x4, 0x10),
	GAME_STATE_SCHEMA_FIELD("data", _game_state_schema_bytes, 0x4, 4),
};
static struct game_state_schema_field const no_identifier[] =
{
	GAME_STATE_SCHEMA_FIELD("a", _game_state_schema_s16, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("b", _game_state_schema_s16, 0x2, 1),
};

static void check_malformed(void)
{
	struct case_
	{
		char const *what;
		struct game_state_schema schema;
		short error;
		short field;
	};
	static struct case_ const malformed[] =
	{
		{ "no name", { NULL, 4, 1, one_u32 }, _game_state_schema_error_name, NONE },
		{ "size 0", { "s", 0, 1, one_u32 }, _game_state_schema_error_size, NONE },
		{ "too large", { "s", GAME_STATE_SCHEMA_MAXIMUM_SIZE + 4, 1, one_u32 }, _game_state_schema_error_size, NONE },
		{ "no fields", { "s", 4, 0, one_u32 }, _game_state_schema_error_fields, NONE },
		{ "fields NULL", { "s", 4, 1, NULL }, _game_state_schema_error_fields, NONE },
		{ "unknown type", { "s", 4, 1, bad_type }, _game_state_schema_error_unknown_type, 0 },
		{ "count 0", { "s", 4, 1, bad_count }, _game_state_schema_error_count, 0 },
		{ "field without name", { "s", 4, 1, bad_name }, _game_state_schema_error_name, 0 },
		{ "past its end", { "s", 8, 2, bad_bounds }, _game_state_schema_error_bounds, 1 },
		{ "overlap", { "s", 8, 3, bad_overlap }, _game_state_schema_error_overlap, 1 },
		{ "a hole", { "s", 8, 2, bad_hole }, _game_state_schema_error_unclaimed, 1 },
		{ "a tail", { "s", 8, 1, bad_tail }, _game_state_schema_error_unclaimed, NONE },
		{ "misaligned", { "s", 8, 3, bad_alignment }, _game_state_schema_error_alignment, 1 },
		{ "struct without schema", { "s", 8, 1, bad_nested }, _game_state_schema_error_nested, 0 },
		{ "scalar with a schema", { "s", 4, 1, bad_stray_schema }, _game_state_schema_error_nested, 0 },
		{ "itself", { "s", 8, 1, recursive_fields }, _game_state_schema_error_nested, 0 },
		{ "discriminator after", { "s", 12, 3, bad_union_order }, _game_state_schema_error_union, 2 },
		{ "real discriminator", { "s", 12, 2, bad_union_discriminator }, _game_state_schema_error_union, 1 },
		{ "duplicate arms", { "s", 12, 2, bad_union_duplicate }, _game_state_schema_error_union, 1 },
		{ "arm not the union's size", { "s", 16, 2, bad_union_size }, _game_state_schema_error_union, 1 },
		{ "no arms", { "s", 12, 2, bad_union_empty }, _game_state_schema_error_union, 1 },
		{ "frame range over its own field", { "s", 8, 2, bad_frame }, _game_state_schema_error_frame_range, 0 },
	};
	size_t index;
	short field;

	CHECK(game_state_schema_validate(NULL, &field) == _game_state_schema_error_null, "NULL schema");
	for (index = 0; index < NUMBEROF(malformed); index++)
	{
		short error = game_state_schema_validate(&malformed[index].schema, &field);

		CHECK(error == malformed[index].error && field == malformed[index].field, "%s: %s at field %d (expected %s at %d)",
			malformed[index].what, game_state_schema_error_name(error), field,
			game_state_schema_error_name(malformed[index].error), malformed[index].field);
		CHECK(game_state_schema_wire_size(&malformed[index].schema) == NONE && !game_state_schema_identity(&malformed[index].schema),
			"%s: no wire size or identity", malformed[index].what);
	}
	{
		struct game_state_schema const element = GAME_STATE_SCHEMA_DEFINE("element", 4, no_identifier);

		CHECK(game_state_schema_validate_element(&element, 4, &field) == _game_state_schema_error_element,
			"an element schema without its identifier first");
		CHECK(game_state_schema_validate_element(&cell_schema, 12, &field) == _game_state_schema_error_element,
			"an element schema not of the element's size");
		CHECK(game_state_schema_validate_element(&cell_schema, 8, &field) == _game_state_schema_error_none,
			"an element schema of its size, its identifier first");
	}
}

/* ---------- identity */

static struct game_state_schema_field const authored_one[] =
{
	GAME_STATE_SCHEMA_FIELD("flag", _game_state_schema_u8, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x1, 3),
	GAME_STATE_SCHEMA_FIELD("value", _game_state_schema_s32, 0x4, 1),
};
static struct game_state_schema_field const authored_two[] =
{
	GAME_STATE_SCHEMA_FIELD("flag", _game_state_schema_u8, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x1, 3),
	GAME_STATE_SCHEMA_FIELD("value", _game_state_schema_s32, 0x4, 1),
};
static struct game_state_schema_field const authored_wide[] =
{
	GAME_STATE_SCHEMA_FIELD("flag", _game_state_schema_u8, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x1, 7),
	GAME_STATE_SCHEMA_FIELD("value", _game_state_schema_s32, 0x8, 1),
};
static struct game_state_schema_field const authored_unsigned[] =
{
	GAME_STATE_SCHEMA_FIELD("flag", _game_state_schema_u8, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x1, 3),
	GAME_STATE_SCHEMA_FIELD("value", _game_state_schema_u32, 0x4, 1),
};

static void check_identity(void)
{
	struct game_state_schema const one = GAME_STATE_SCHEMA_DEFINE("pair", 8, authored_one);
	struct game_state_schema const two = GAME_STATE_SCHEMA_DEFINE("pair", 8, authored_two);
	struct game_state_schema const wide = GAME_STATE_SCHEMA_DEFINE("pair", 12, authored_wide);
	struct game_state_schema const other = GAME_STATE_SCHEMA_DEFINE("pair", 8, authored_unsigned);
	struct game_state_schema const renamed = GAME_STATE_SCHEMA_DEFINE("couple", 8, authored_one);

	CHECK(game_state_schema_identity(&one) && game_state_schema_identity(&one) == game_state_schema_identity(&two) &&
		game_state_schema_layout_digest(&one) == game_state_schema_layout_digest(&two),
		"schemas authored alike: one identity, one layout");
	CHECK(game_state_schema_identity(&one) == game_state_schema_identity(&wide) &&
		game_state_schema_layout_digest(&one) != game_state_schema_layout_digest(&wide),
		"another layout of the same wire: one identity, another layout digest");
	CHECK(game_state_schema_identity(&one) != game_state_schema_identity(&other), "another type: another identity");
	CHECK(game_state_schema_identity(&one) != game_state_schema_identity(&renamed), "another class name: another identity");
	CHECK(game_state_schema_identity(&game_state_schema_data_array_header) ==
		game_state_schema_identity(&game_state_schema_data_array_header), "identity is deterministic");
}

/* ---------- the gate */

static void check_gate(void)
{
	static byte memory[0x1000];
	struct game_state_schema_region regions[6];
	struct game_state_schema_state state = make_state(memory, sizeof(memory), BASE, TAGS, TRUE);
	struct game_state_schema_coverage coverage;
	long at = 0;
	long index;

	memset(regions, 0, sizeof(regions));
	regions[0].name = "header";
	regions[0].kind = _game_state_schema_region_global;
	regions[0].size = 0x14C;
	regions[0].schema = game_state_schema_class_find("header", _game_state_schema_region_global, 0x14C);
	regions[1].name = "lightnings";
	regions[1].kind = _game_state_schema_region_data_array;
	regions[1].maximum_count = 4;
	regions[1].element_size = 8;
	regions[1].size = 0x58;
	regions[1].schema = game_state_schema_class_find("lightnings", _game_state_schema_region_data_array, 8);
	regions[2].name = "hs globals";
	regions[2].kind = _game_state_schema_region_data_array;
	regions[2].maximum_count = 4;
	regions[2].element_size = 8;
	regions[2].size = 0x58;
	regions[2].schema = &cell_schema;
	regions[3].name = "objects";
	regions[3].kind = _game_state_schema_region_memory_pool;
	regions[3].pool_size = 0x100;
	regions[3].size = 0x138;
	regions[4].name = "ai globals";
	regions[4].kind = _game_state_schema_region_global;
	regions[4].size = 0x40;
	for (index = 0; index < 5; index++)
	{
		regions[index].offset = at;
		at += regions[index].size;
	}
	regions[5].name = "decal vertices";
	regions[5].kind = _game_state_schema_region_global;
	regions[5].gpu = TRUE;
	regions[5].size = 0x100;
	regions[5].offset = sizeof(memory) - 0x100;
	state.cpu_used = at;
	state.gpu_used = 0x100;
	state.regions = regions;
	state.region_count = 6;

	CHECK(regions[0].schema && regions[1].schema, "the named classes are found");
	CHECK(!game_state_schema_class_find("header", _game_state_schema_region_global, 0x150) &&
		!game_state_schema_class_find("header", _game_state_schema_region_data_array, 0x14C) &&
		!game_state_schema_class_find("headers", _game_state_schema_region_global, 0x14C),
		"a class is found only by its name, kind and size");
	game_state_schema_coverage(&state, &coverage);
	CHECK(coverage.state_bytes == 0x474, "state bytes %lu", coverage.state_bytes);
	CHECK(coverage.typed_bytes == 0x224, "typed bytes 0x%lx", coverage.typed_bytes);
	CHECK(coverage.unresolved_bytes == 0x10, "unresolved bytes 0x%lx", coverage.unresolved_bytes);
	CHECK(coverage.unclaimed_bytes == 0x240, "unclaimed bytes 0x%lx", coverage.unclaimed_bytes);
	CHECK(coverage.regions == 6 && coverage.typed_regions == 2 && coverage.unresolved_regions == 1 &&
		coverage.unclaimed_regions == 3, "regions %ld: %ld typed, %ld unresolved, %ld unclaimed", coverage.regions,
		coverage.typed_regions, coverage.unresolved_regions, coverage.unclaimed_regions);
	CHECK(coverage.gap_count == 4 && coverage.gaps_kept == 4, "four gaps (%ld)", coverage.gap_count);
	CHECK(coverage.gaps[0].reason == _game_state_schema_gap_hs_cell && coverage.gaps[0].offset == 0x14C + 0x58 + 0x38 + 4 &&
		coverage.gaps[0].size == 16 && !strcmp(coverage.gaps[0].field, "value"), "the HS cells' gap");
	CHECK(coverage.gaps[1].reason == _game_state_schema_gap_pool_payload && coverage.gaps[1].size == 0x100 &&
		!strcmp(coverage.gaps[1].owner, "objects"), "the memory pool's blocks' gap");
	CHECK(coverage.gaps[2].reason == _game_state_schema_gap_unclaimed && coverage.gaps[2].size == 0x40 &&
		!strcmp(coverage.gaps[2].owner, "ai globals"), "the unclaimed global's gap");
	CHECK(coverage.gaps[3].reason == _game_state_schema_gap_unclaimed && coverage.gaps[3].offset == 0xF00 &&
		!strcmp(coverage.gaps[3].owner, "decal vertices"), "the GPU part's gap");
	CHECK(!coverage.complete, "a partial schema set does not pass the gate");
	{
		struct game_state_schema_coverage page;

		game_state_schema_coverage_page(&state, 2, &page);
		CHECK(page.gap_count == 4 && page.gaps_first == 2 && page.gaps_kept == 2 &&
			page.gaps[0].reason == _game_state_schema_gap_unclaimed && !strcmp(page.gaps[0].owner, "ai globals") &&
			page.unclaimed_bytes == coverage.unclaimed_bytes, "the gaps a page at a time");
	}
	{
		/* neither written nor read: nothing asked of the stream */
		byte scratch[0x400];
		struct game_state_schema_stream stream;

		memset(&stream, 0, sizeof(stream));
		stream.write = NULL;
		stream.read = NULL;
		stream.scratch = scratch;
		stream.scratch_size = sizeof(scratch);
		memset(memory, 0x5A, sizeof(memory));
		CHECK(game_state_schema_stream_write(&state, &stream) == _game_state_schema_stream_incomplete,
			"a partial schema set refuses the stream's write");
		CHECK(game_state_schema_stream_read(&state, &stream, TRUE, NULL) == _game_state_schema_stream_incomplete,
			"a partial schema set refuses the stream's read");
		for (index = 0; index < (long)sizeof(memory) && memory[index] == 0x5A; index++)
			;
		CHECK(index == (long)sizeof(memory), "nothing of the state taken");
	}
	/* a pointer version 2 was told of, which the schema does not type as one */
	regions[0].declared_pointer_count = 1;
	regions[0].declared_pointers[0] = 0x4;
	game_state_schema_coverage(&state, &coverage);
	CHECK(coverage.conflicts == 1 && coverage.gaps[0].reason == _game_state_schema_gap_pointer &&
		coverage.typed_regions == 1, "a declared pointer the schema types otherwise is a conflict");
	regions[0].declared_pointer_count = 0;
	/* regions out of order: nothing trusted */
	regions[1].offset += 4;
	game_state_schema_coverage(&state, &coverage);
	CHECK(!coverage.complete && coverage.unclaimed_bytes >= coverage.state_bytes, "regions out of order cover nothing");
	regions[1].offset -= 4;
	/* a region not of its kind's size */
	regions[1].size += 4;
	regions[2].offset += 4;
	regions[3].offset += 4;
	regions[4].offset += 4;
	state.cpu_used += 4;
	game_state_schema_coverage(&state, &coverage);
	CHECK(coverage.gaps[0].reason == _game_state_schema_gap_size && !strcmp(coverage.gaps[0].owner, "lightnings"),
		"a data array not of its elements' size is not covered");
	/* too many regions */
	state.region_count = GAME_STATE_SCHEMA_MAXIMUM_REGIONS + 1;
	game_state_schema_coverage(&state, &coverage);
	CHECK(!coverage.complete && coverage.gap_count == 1 && coverage.gaps[0].reason == _game_state_schema_gap_outside,
		"more regions than the gate walks");
}

/* ---------- the engine's globals of scalars (game_state_schema_classes.c):
found by their whole allocation name, every byte typed, the same wire bytes
from either byte order, padding not written and taken as zero */

struct named_global
{
	char const *name;
	long size;
	long wire_size;
};

static struct named_global const named_globals[] =
{
	{ "director scripting", 0x4, 1 },
	{ "hud scripted globals", 0x4, 2 },
	{ "rasterizer model ambient reflection tint", 0x10, 16 },
	{ "lights globals", 0x4, 1 },
	{ "structure decals", 0x4, 1 },
	{ "decal globals", 0x280C, 0x280C },
	/* enabled, 16 bsps' 8 longs of flags, 16 x 256 vitalities (not the 3 pad bytes) */
	{ "breakable surface globals", 0x4204, 1 + 16 * 32 + 16 * 256 * 4 },
};

enum
{
	DECAL_GLOBALS_BYTES = 0x280C,
	BREAKABLE_BYTES = 0x4204,
	BREAKABLE_FLAGS_AT = 0x1,
	BREAKABLE_VITALITY_AT = 0x204,
	BREAKABLE_WIRE_VITALITY_AT = 1 + 16 * 32,
};

/* an instance of the named global from the native bytes of either byte
order: the same wire bytes, checked against what is given, and taken back
into either order as the native bytes given there */
static void check_named_round_trip(char const *name, long size, byte const *little, byte const *big,
	byte const *expected_wire, byte const *taken_big, byte *wire_scratch, byte *wire_other, byte *native_scratch)
{
	struct game_state_schema const *schema = game_state_schema_class_find(name, _game_state_schema_region_global, size);
	struct game_state_schema_state little_state = make_state(native_scratch, size, BASE, TAGS, FALSE);
	struct game_state_schema_state big_state = make_state(native_scratch, size, OTHER_BASE, OTHER_TAGS, TRUE);
	long wire_size = schema ? game_state_schema_wire_size(schema) : NONE;
	short error;

	CHECK(schema && wire_size > 0, "%s: found, valid", name);
	if (!schema || wire_size <= 0)
		return;
	error = game_state_schema_encode(schema, &little_state, little, BASE, wire_scratch, wire_size);
	CHECK(error == _game_state_schema_stream_ok, "%s: little-endian encodes (%s)", name, game_state_schema_stream_error_name(error));
	error = game_state_schema_encode(schema, &big_state, big, OTHER_BASE, wire_other, wire_size);
	CHECK(error == _game_state_schema_stream_ok, "%s: big-endian encodes (%s)", name, game_state_schema_stream_error_name(error));
	CHECK(!memcmp(wire_scratch, wire_other, (size_t)wire_size), "%s: the same wire from either byte order", name);
	dump_difference(name, wire_other, wire_scratch, wire_size);
	CHECK(!memcmp(wire_scratch, expected_wire, (size_t)wire_size), "%s: the wire bytes as specified", name);
	dump_difference(name, wire_scratch, expected_wire, wire_size);
	memset(native_scratch, 0xCC, (size_t)size);
	error = game_state_schema_decode(schema, &big_state, wire_scratch, wire_size, NULL, OTHER_BASE);
	CHECK(error == _game_state_schema_stream_ok, "%s: the wire checks (%s)", name, game_state_schema_stream_error_name(error));
	CHECK(native_scratch[0] == 0xCC && native_scratch[size - 1] == 0xCC, "%s: a check takes nothing", name);
	error = game_state_schema_decode(schema, &big_state, wire_scratch, wire_size, native_scratch, OTHER_BASE);
	CHECK(error == _game_state_schema_stream_ok && !memcmp(native_scratch, taken_big, (size_t)size),
		"%s: taken big-endian, as specified (%s)", name, game_state_schema_stream_error_name(error));
	dump_difference(name, native_scratch, taken_big, size);
	memset(native_scratch, 0xCC, (size_t)size);
	little_state.big_endian = FALSE;
	error = game_state_schema_decode(schema, &little_state, wire_scratch, wire_size, native_scratch, BASE);
	CHECK(error == _game_state_schema_stream_ok, "%s: taken little-endian (%s)", name, game_state_schema_stream_error_name(error));
	error = game_state_schema_encode(schema, &little_state, native_scratch, BASE, wire_other, wire_size);
	CHECK(error == _game_state_schema_stream_ok && !memcmp(wire_other, expected_wire, (size_t)wire_size),
		"%s: what little-endian took encodes as it was", name);
	CHECK(game_state_schema_decode(schema, &big_state, wire_scratch, wire_size - 1, NULL, OTHER_BASE) ==
		_game_state_schema_stream_layout, "%s: a wire of another size is refused", name);
}

/* (bad: the flags' longs as fields of the globals' own schema, 1 byte in) */
static struct game_state_schema_field const breakable_flags_unaligned_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("enabled", _game_state_schema_boolean, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("breakable_surface_flags", _game_state_schema_u32, 0x1, 16 * 8),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x201, 3),
	GAME_STATE_SCHEMA_FIELD("breakable_surfaces", _game_state_schema_real, 0x204, 16 * 256),
};
/* (wrong: the flags as the bytes they are declared as; valid, but their
meaning, bits of longs, is not the same bytes on either byte order) */
static struct game_state_schema_field const breakable_flags_as_bytes_fields[] =
{
	GAME_STATE_SCHEMA_FIELD("enabled", _game_state_schema_boolean, 0x0, 1),
	GAME_STATE_SCHEMA_FIELD("breakable_surface_flags", _game_state_schema_bytes, 0x1, 16 * 32),
	GAME_STATE_SCHEMA_FIELD("(alignment)", _game_state_schema_padding, 0x201, 3),
	GAME_STATE_SCHEMA_FIELD("breakable_surfaces", _game_state_schema_real, 0x204, 16 * 256),
};

static void check_named_globals(void)
{
	static byte little[BREAKABLE_BYTES];
	static byte big[BREAKABLE_BYTES];
	static byte taken_big[BREAKABLE_BYTES];
	static byte native[BREAKABLE_BYTES];
	static byte wire[BREAKABLE_BYTES];
	static byte other[BREAKABLE_BYTES];
	static byte expected[BREAKABLE_BYTES];
	long index;

	/* each: found by its whole name, kind and size; valid; every byte typed */
	for (index = 0; index < (long)NUMBEROF(named_globals); index++)
	{
		struct named_global const *entry = &named_globals[index];
		struct game_state_schema const *schema =
			game_state_schema_class_find(entry->name, _game_state_schema_region_global, entry->size);
		struct game_state_schema_region region;
		struct game_state_schema_state state = make_state(native, entry->size, BASE, TAGS, TRUE);
		struct game_state_schema_coverage coverage;
		short field_index;

		CHECK(schema && !game_state_schema_validate(schema, &field_index) && schema->size == entry->size,
			"%s: found by name, kind and size, and valid", entry->name);
		if (!schema)
			continue;
		CHECK(game_state_schema_wire_size(schema) == entry->wire_size, "%s: %ld wire bytes (%ld)", entry->name,
			entry->wire_size, game_state_schema_wire_size(schema));
		CHECK(!game_state_schema_class_find(entry->name, _game_state_schema_region_global, entry->size + 4) &&
			!game_state_schema_class_find(entry->name, _game_state_schema_region_data_array, entry->size),
			"%s: not found at another size or kind", entry->name);
		memset(&region, 0, sizeof(region));
		region.name = entry->name;
		region.kind = _game_state_schema_region_global;
		region.size = entry->size;
		region.schema = schema;
		state.region_count = 1;
		state.regions = &region;
		game_state_schema_coverage(&state, &coverage);
		CHECK(coverage.complete && coverage.typed_bytes == (unsigned long)entry->size && !coverage.unresolved_bytes &&
			!coverage.gap_count && coverage.typed_regions == 1, "%s: every byte typed (%lu typed, %lu unresolved, %ld gaps)",
			entry->name, coverage.typed_bytes, coverage.unresolved_bytes, coverage.gap_count);
	}
	/* the name a region keeps is cut to 31 characters: the class is found by
	the whole one (game_state_region_record), never by the cut one */
	CHECK(!game_state_schema_class_find("rasterizer model ambient reflec", _game_state_schema_region_global, 0x10) &&
		!game_state_schema_class_find("rasterizer model ambient reflection tin", _game_state_schema_region_global, 0x10) &&
		!game_state_schema_class_find("director scripting ", _game_state_schema_region_global, 0x4) &&
		!game_state_schema_class_find(NULL, _game_state_schema_region_global, 0x4),
		"a class is not found by a name cut short, lengthened or none");

	/* booleans and their pad bytes: the pad bytes are not written, taken as zero */
	{
		static byte const director_native[4] = { 0x01, 0xAA, 0xBB, 0xCC };
		static byte const director_wire[1] = { 0x01 };
		static byte const director_taken[4] = { 0x01, 0x00, 0x00, 0x00 };
		static byte const hud_native[4] = { 0x00, 0x01, 0x55, 0x66 };
		static byte const hud_wire[2] = { 0x00, 0x01 };
		static byte const hud_taken[4] = { 0x00, 0x01, 0x00, 0x00 };

		check_named_round_trip("director scripting", 4, director_native, director_native, director_wire, director_taken,
			wire, other, native);
		check_named_round_trip("hud scripted globals", 4, hud_native, hud_native, hud_wire, hud_taken, wire, other, native);
		check_named_round_trip("lights globals", 4, director_native, director_native, director_wire, director_taken,
			wire, other, native);
		check_named_round_trip("structure decals", 4, director_native, director_native, director_wire, director_taken,
			wire, other, native);
	}

	/* the tint: four reals' bits, a NaN's payload kept */
	{
		static byte const tint_little[16] =
		{
			0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xC0, 0x23, 0x01, 0xC0, 0x7F
		};
		static byte const tint_big[16] =
		{
			0x3F, 0x80, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x7F, 0xC0, 0x01, 0x23
		};

		check_named_round_trip("rasterizer model ambient reflection tint", 16, tint_little, tint_big, tint_little, tint_big,
			wire, other, native);
	}

	/* decal globals: datum handles (NONE: none) and counts */
	{
		static byte const spots[][5] =
		{
			/* wire offset (2 bytes, little-endian), then 3 of the bytes there */
			{ 0x00, 0x00, 0x03, 0x00, 0x01 }, /* [0][0]: 0xE0010003 */
			{ 0x04, 0x00, 0xFF, 0xFF, 0xFF }, /* [0][1]: NONE */
			{ 0xFC, 0x27, 0x05, 0x00, 0x22 }, /* [4][511]: 0xE0220005 */
			{ 0x00, 0x28, 0x07, 0x00, 0x34 }, /* first disconnected: 0x12340007 */
			{ 0x04, 0x28, 0xFD, 0xFF, 0xFF }, /* locked: -3 */
			{ 0x08, 0x28, 0x02, 0x01, 0x00 }, /* permanent: 0x102 */
		};

		for (index = 0; index < 5 * 512 + 1; index++)
		{
			put32(little + index * 4, 0xFFFFFFFFUL, FALSE);
			put32(big + index * 4, 0xFFFFFFFFUL, TRUE);
		}
		put32(little + 0x0, 0xE0010003UL, FALSE);
		put32(big + 0x0, 0xE0010003UL, TRUE);
		put32(little + 0x27FC, 0xE0220005UL, FALSE);
		put32(big + 0x27FC, 0xE0220005UL, TRUE);
		put32(little + 0x2800, 0x12340007UL, FALSE);
		put32(big + 0x2800, 0x12340007UL, TRUE);
		put32(little + 0x2804, 0xFFFFFFFDUL, FALSE);
		put32(big + 0x2804, 0xFFFFFFFDUL, TRUE);
		put32(little + 0x2808, 0x102UL, FALSE);
		put32(big + 0x2808, 0x102UL, TRUE);
		/* (every field a word: the wire is the little-endian layout) */
		check_named_round_trip("decal globals", DECAL_GLOBALS_BYTES, little, big, little, big, wire, other, native);
		for (index = 0; index < (long)NUMBEROF(spots); index++)
		{
			long at = spots[index][0] | (spots[index][1] << 8);

			CHECK(wire[at] == spots[index][2] && wire[at + 1] == spots[index][3] && wire[at + 2] == spots[index][4],
				"decal globals: the wire at 0x%lx, as specified", at);
		}
	}

	/* breakable surface globals: surface n of a bsp is bit n % 32 of its
	flags' long n / 32, on either byte order */
	{
		struct game_state_schema const unaligned =
			GAME_STATE_SCHEMA_DEFINE("breakable_surface_globals", BREAKABLE_BYTES, breakable_flags_unaligned_fields);
		struct game_state_schema const as_bytes =
			GAME_STATE_SCHEMA_DEFINE("breakable_surface_globals", BREAKABLE_BYTES, breakable_flags_as_bytes_fields);
		struct game_state_schema_state little_state = make_state(native, BREAKABLE_BYTES, BASE, TAGS, FALSE);
		struct game_state_schema_state big_state = make_state(native, BREAKABLE_BYTES, BASE, TAGS, TRUE);
		short field_index;
		long bsp;
		long word;

		memset(little, 0, sizeof(little));
		memset(big, 0, sizeof(big));
		memset(expected, 0, sizeof(expected));
		little[0] = big[0] = expected[0] = 0x01;
		for (bsp = 0; bsp < 16; bsp++)
		{
			for (word = 0; word < 8; word++)
			{
				/* bsp 0: surfaces 0, 2 and 255 there; bsp 15: all; the others: surface 32 * word + bsp */
				unsigned long value = bsp == 15 ? 0xFFFFFFFFUL : bsp ? 1UL << bsp : 0;

				if (!bsp && word == 0)
					value = 0x5;
				if (!bsp && word == 7)
					value = 0x80000000UL;
				put32(little + BREAKABLE_FLAGS_AT + bsp * 32 + word * 4, value, FALSE);
				put32(big + BREAKABLE_FLAGS_AT + bsp * 32 + word * 4, value, TRUE);
			}
		}
		/* (the 3 pad bytes: whatever was there) */
		little[0x201] = big[0x201] = 0xEE;
		little[0x203] = big[0x203] = 0x11;
		for (index = 0; index < 16 * 256; index++)
		{
			unsigned long value = index == 0 ? 0x3F800000UL : index == 16 * 256 - 1 ? 0x3E800000UL : 0x3F000000UL;

			put32(little + BREAKABLE_VITALITY_AT + index * 4, value, FALSE);
			put32(big + BREAKABLE_VITALITY_AT + index * 4, value, TRUE);
			put32(expected + BREAKABLE_WIRE_VITALITY_AT + index * 4, value, FALSE);
		}
		for (index = 0; index < 16 * 32; index++)
			expected[1 + index] = little[BREAKABLE_FLAGS_AT + index];
		memcpy(taken_big, big, sizeof(taken_big));
		taken_big[0x201] = taken_big[0x203] = 0x00;
		check_named_round_trip("breakable surface globals", BREAKABLE_BYTES, little, big, expected, taken_big, wire, other,
			native);
		{
			/* by hand: bsp 0's first long (surfaces 0 and 2), its last (surface
			255), bsp 1's first (surface 1), the first and last vitality */
			static byte const first[5] = { 0x01, 0x05, 0x00, 0x00, 0x00 };
			static byte const last_word[4] = { 0x00, 0x00, 0x00, 0x80 };
			static byte const bsp1[4] = { 0x02, 0x00, 0x00, 0x00 };
			static byte const vitality_first[4] = { 0x00, 0x00, 0x80, 0x3F };
			static byte const vitality_last[4] = { 0x00, 0x00, 0x80, 0x3E };

			CHECK(!memcmp(wire, first, 5) && !memcmp(wire + 1 + 28, last_word, 4) && !memcmp(wire + 1 + 32, bsp1, 4),
				"breakable surface globals: the flags' longs on the wire, as specified");
			CHECK(!memcmp(wire + BREAKABLE_WIRE_VITALITY_AT, vitality_first, 4) &&
				!memcmp(wire + BREAKABLE_WIRE_VITALITY_AT + (16 * 256 - 1) * 4, vitality_last, 4),
				"breakable surface globals: the vitalities on the wire, as specified");
			CHECK(big[BREAKABLE_FLAGS_AT + 28] == 0x80 && little[BREAKABLE_FLAGS_AT + 31] == 0x80,
				"surface 255's bit lies in another byte on either byte order");
		}
		CHECK(game_state_schema_validate(&unaligned, &field_index) == _game_state_schema_error_alignment && field_index == 1,
			"the flags' longs 1 byte into the globals' own schema are refused (alignment)");
		{
			long size = game_state_schema_wire_size(&as_bytes);

			CHECK(size == 1 + 16 * 32 + 16 * 256 * 4 &&
				game_state_schema_encode(&as_bytes, &little_state, little, BASE, wire, size) == _game_state_schema_stream_ok &&
				game_state_schema_encode(&as_bytes, &big_state, big, BASE, other, size) == _game_state_schema_stream_ok &&
				memcmp(wire, other, (size_t)size), "the flags as bytes are another wire on either byte order (why they are longs)");
		}
	}

	/* the named globals pass the gate together, and one global without a
	schema keeps it from passing: nothing written, nothing taken */
	{
		static byte memory[0x8000];
		struct game_state_schema_region regions[NUMBEROF(named_globals) + 1];
		struct game_state_schema_state state = make_state(memory, sizeof(memory), BASE, TAGS, FALSE);
		struct game_state_schema_coverage coverage;
		struct game_state_schema_stream stream;
		byte scratch[0x100];
		long at = 0;

		memset(regions, 0, sizeof(regions));
		for (index = 0; index < (long)NUMBEROF(named_globals); index++)
		{
			regions[index].name = named_globals[index].name;
			regions[index].kind = _game_state_schema_region_global;
			regions[index].offset = at;
			regions[index].size = named_globals[index].size;
			regions[index].schema = game_state_schema_class_find(named_globals[index].name, _game_state_schema_region_global,
				named_globals[index].size);
			at += named_globals[index].size;
		}
		state.cpu_used = at;
		state.region_count = NUMBEROF(named_globals);
		state.regions = regions;
		game_state_schema_coverage(&state, &coverage);
		CHECK(coverage.complete && coverage.typed_bytes == (unsigned long)at && coverage.typed_regions == (long)NUMBEROF(named_globals),
			"the named globals together pass the gate (%lu of %lu typed)", coverage.typed_bytes, coverage.state_bytes);
		CHECK(game_state_schema_stream_scratch_bytes(&state) == ((BREAKABLE_WIRE_VITALITY_AT + 16 * 256 * 4 + 3) & ~3L),
			"their scratch: the widest one's wire bytes, in words (%ld)", game_state_schema_stream_scratch_bytes(&state));
		regions[index].name = "player control globals";
		regions[index].kind = _game_state_schema_region_global;
		regions[index].offset = at;
		regions[index].size = 0x40;
		state.cpu_used = at + 0x40;
		state.region_count = NUMBEROF(named_globals) + 1;
		game_state_schema_coverage(&state, &coverage);
		CHECK(!coverage.complete && coverage.typed_bytes == (unsigned long)(state.cpu_used - 0x40) &&
			coverage.unclaimed_bytes == 0x40 && coverage.gap_count == 1 &&
			coverage.gaps[0].reason == _game_state_schema_gap_unclaimed && coverage.gaps[0].offset == at &&
			!strcmp(coverage.gaps[0].owner, "player control globals"), "one global without a schema: the gate's one gap");
		memset(&stream, 0, sizeof(stream));
		stream.scratch = scratch;
		stream.scratch_size = sizeof(scratch);
		memset(memory, 0x5A, sizeof(memory));
		CHECK(game_state_schema_stream_write(&state, &stream) == _game_state_schema_stream_incomplete &&
			game_state_schema_stream_read(&state, &stream, TRUE, NULL) == _game_state_schema_stream_incomplete,
			"then the stream is neither written nor read");
		for (index = 0; index < (long)sizeof(memory) && memory[index] == 0x5A; index++)
			;
		CHECK(index == (long)sizeof(memory), "nothing of the state taken");
	}
}

/* ---------- a whole stream */

enum
{
	STATE_BYTES = 0x400,
	HEADER_AT = 0x0,
	MIXED_AT = 0x14C,
	LIGHTNINGS_AT = 0x18C,
	CACHE_AT = 0x1E4,
	CPU_USED = 0x298,
	GPU_MIXED_AT = STATE_BYTES - 0x20,
	STREAM_BYTES = 643,
};

static struct game_state_schema_region stream_regions[5];

static void stream_regions_make(unsigned long delete_proc, unsigned long locked_proc)
{
	memset(stream_regions, 0, sizeof(stream_regions));
	stream_regions[0].name = "header";
	stream_regions[0].offset = HEADER_AT;
	stream_regions[0].size = 0x14C;
	stream_regions[0].schema = game_state_schema_class_find("header", _game_state_schema_region_global, 0x14C);
	stream_regions[1].name = "mixed";
	stream_regions[1].offset = MIXED_AT;
	stream_regions[1].size = 0x40;
	stream_regions[1].schema = &mixed_schema;
	stream_regions[1].declared_pointer_count = 2;
	stream_regions[1].declared_pointers[0] = 0x10;
	stream_regions[1].declared_pointers[1] = 0x34;
	stream_regions[2].name = "lightnings";
	stream_regions[2].kind = _game_state_schema_region_data_array;
	stream_regions[2].offset = LIGHTNINGS_AT;
	stream_regions[2].size = 0x58;
	stream_regions[2].maximum_count = 4;
	stream_regions[2].element_size = 8;
	stream_regions[2].schema = game_state_schema_class_find("lightnings", _game_state_schema_region_data_array, 8);
	stream_regions[3].name = "vertex cache";
	stream_regions[3].kind = _game_state_schema_region_lruv_cache;
	stream_regions[3].offset = CACHE_AT;
	stream_regions[3].size = 0xB4;
	stream_regions[3].maximum_count = 2;
	stream_regions[3].page_count = 64;
	stream_regions[3].page_size_bits = 12;
	stream_regions[3].procedures[0] = delete_proc;
	stream_regions[3].procedures[1] = locked_proc;
	stream_regions[4].name = "gpu mixed";
	stream_regions[4].gpu = TRUE;
	stream_regions[4].offset = GPU_MIXED_AT;
	stream_regions[4].size = 0x20;
	stream_regions[4].schema = &mixed_schema;
}

static void data_array_make(byte *at, unsigned long base, long offset, char const *name, short maximum_count,
	short element_size, short count, short next_identifier, boolean big_endian, byte padding)
{
	memset(at, 0, 0x38);
	strcpy((char *)at, name);
	put16(at + 0x20, (unsigned long)maximum_count, big_endian);
	put16(at + 0x22, (unsigned long)element_size, big_endian);
	at[0x24] = 1;
	at[0x25] = 0;
	at[0x26] = padding;
	at[0x27] = padding;
	put32(at + 0x28, 0x64407440UL, big_endian);
	put16(at + 0x2C, (unsigned long)count, big_endian);
	put16(at + 0x2E, (unsigned long)count, big_endian);
	put16(at + 0x30, (unsigned long)count - 1, big_endian);
	put16(at + 0x32, (unsigned long)next_identifier, big_endian);
	put32(at + 0x34, base + (unsigned long)offset + 0x38, big_endian);
}

static void mixed_make(byte *at, unsigned long base, unsigned long tags, boolean big_endian, byte padding, short variant)
{
	put16(at, 0x1234 + (unsigned long)variant, big_endian);
	at[2] = 0xA5;
	at[3] = padding;
	put16(at + 4, 0xFFFE, big_endian);
	put16(at + 6, 0xBEEF, big_endian);
	put32(at + 8, 0x3FC00000UL, big_endian);
	put32(at + 0xC, 0x00070003UL, big_endian);
	put32(at + 0x10, variant == 1 ? 0 : base + 0x100 + (unsigned long)variant * 4, big_endian);
	put32(at + 0x14, variant == 2 ? base + 0x8 : tags + 0x20, big_endian);
	memcpy(at + 0x18, "Halo", 4);
	put32(at + 0x1C, (unsigned long)-1 - (unsigned long)variant, big_endian);
}

/* the state the stream is of: dirty, as the game leaves it (free elements'
old contents, padding), or as it is taken (those zero) */
static void stream_state_make(byte *memory, unsigned long base, unsigned long tags, unsigned long delete_proc,
	unsigned long locked_proc, boolean big_endian, boolean dirty)
{
	byte junk = dirty ? 0x6B : 0;
	byte *at;

	memset(memory, 0, STATE_BYTES);
	/* the header */
	at = memory + HEADER_AT;
	put32(at, 0xC0FFEE11UL, big_endian);
	strcpy((char *)at + 4, "levels\\a10\\a10");
	strcpy((char *)at + 0x104, "01.10.12.2276");
	put16(at + 0x124, 1, big_endian);
	put16(at + 0x126, 2, big_endian);
	put32(at + 0x128, 0x89ABCDEFUL, big_endian);
	put32(at + 0x12C, 0xFFFFFFFFUL, big_endian);
	put32(at + 0x148, 0x01020304UL, big_endian);
	/* two instances of mixed */
	mixed_make(memory + MIXED_AT, base, tags, big_endian, junk, 0);
	mixed_make(memory + MIXED_AT + 0x20, base, tags, big_endian, junk, 1);
	/* lightnings: 0 and 2 in use, 1 free (its old contents), 3 past the count */
	at = memory + LIGHTNINGS_AT;
	data_array_make(at, base, LIGHTNINGS_AT, "lightnings", 4, 8, 3, (short)0xE004, big_endian, junk);
	put16(at + 0x38, 0xE001, big_endian);
	put16(at + 0x3A, 0, big_endian);
	put32(at + 0x3C, 0x00120034UL, big_endian);
	put16(at + 0x40, 0, big_endian);
	at[0x42] = junk;
	at[0x45] = junk;
	put16(at + 0x48, 0xE003, big_endian);
	put16(at + 0x4A, 0x7FFF, big_endian);
	put32(at + 0x4C, 0xFFFFFFFFUL, big_endian);
	at[0x52] = junk;
	/* the lruv cache: block 0 in use */
	at = memory + CACHE_AT;
	strcpy((char *)at, "vertex cache");
	put32(at + 0x20, delete_proc, big_endian);
	put32(at + 0x24, locked_proc, big_endian);
	put32(at + 0x28, 64, big_endian);
	put32(at + 0x2C, 12, big_endian);
	put32(at + 0x30, 77, big_endian);
	put32(at + 0x34, 0x80010000UL, big_endian);
	put32(at + 0x38, 0x80010000UL, big_endian);
	put32(at + 0x3C, base + CACHE_AT + 0x44, big_endian);
	put32(at + 0x40, 0x77656565UL, big_endian);
	data_array_make(at + 0x44, base, CACHE_AT + 0x44, "vertex cache blocks", 2, 0x1C, 1, (short)0x8002, big_endian, 0);
	put16(at + 0x7C, 0x8001, big_endian);
	put16(at + 0x7E, 3, big_endian);
	put32(at + 0x80, 8, big_endian);
	put32(at + 0x84, 0, big_endian);
	put32(at + 0x88, 0xFFFFFFFFUL, big_endian);
	put32(at + 0x8C, 0xFFFFFFFFUL, big_endian);
	put32(at + 0x90, 76, big_endian);
	put32(at + 0x94, 0, big_endian);
	at[0x9A] = junk; /* (block 1, free: its flags) */
	/* the GPU part */
	mixed_make(memory + GPU_MIXED_AT, base, tags, big_endian, junk, 2);
}

struct buffer
{
	byte *bytes;
	long size;
	long used;
	long largest_transfer;
	long fail_after; /* NONE: never */
};

static boolean buffer_write(void *context, byte const *bytes, long size)
{
	struct buffer *buffer = context;

	if (size > buffer->largest_transfer)
		buffer->largest_transfer = size;
	if (size < 0 || buffer->used + size > buffer->size)
		return FALSE;
	memcpy(buffer->bytes + buffer->used, bytes, (size_t)size);
	buffer->used += size;
	return TRUE;
}

static boolean buffer_read(void *context, byte *bytes, long size)
{
	struct buffer *buffer = context;

	if (size > buffer->largest_transfer)
		buffer->largest_transfer = size;
	if (size < 0 || buffer->used + size > buffer->size || (buffer->fail_after != NONE && buffer->used + size > buffer->fail_after))
		return FALSE;
	memcpy(bytes, buffer->bytes + buffer->used, (size_t)size);
	buffer->used += size;
	return TRUE;
}

static short stream_write_from(byte *memory, unsigned long base, boolean big_endian, byte *out, long out_size,
	long *written, long *largest, long scratch_size)
{
	static byte scratch[0x400];
	struct game_state_schema_state state = make_state(memory, STATE_BYTES, base, TAGS, big_endian);
	struct game_state_schema_stream stream;
	struct buffer buffer;
	short error;

	state.cpu_used = CPU_USED;
	state.gpu_used = 0x20;
	state.region_count = 5;
	state.regions = stream_regions;
	memset(&buffer, 0, sizeof(buffer));
	buffer.bytes = out;
	buffer.size = out_size;
	buffer.fail_after = NONE;
	memset(&stream, 0, sizeof(stream));
	stream.context = &buffer;
	stream.write = buffer_write;
	stream.scratch = scratch;
	stream.scratch_size = scratch_size;
	error = game_state_schema_stream_write(&state, &stream);
	*written = buffer.used;
	if (largest)
		*largest = buffer.largest_transfer;
	return error;
}

static short stream_read_into(byte *memory, byte const *in, long in_size, boolean take, long fail_after, long *offset)
{
	static byte scratch[0x400];
	struct game_state_schema_state state = make_state(memory, STATE_BYTES, OTHER_BASE, OTHER_TAGS, TRUE);
	struct game_state_schema_stream stream;
	struct buffer buffer;
	short error;

	state.cpu_used = CPU_USED;
	state.gpu_used = 0x20;
	state.region_count = 5;
	state.regions = stream_regions;
	memset(&buffer, 0, sizeof(buffer));
	buffer.bytes = (byte *)in;
	buffer.size = in_size;
	buffer.fail_after = fail_after;
	memset(&stream, 0, sizeof(stream));
	stream.context = &buffer;
	stream.read = buffer_read;
	stream.scratch = scratch;
	stream.scratch_size = game_state_schema_stream_scratch_bytes(&state);
	error = game_state_schema_stream_read(&state, &stream, take, offset);
	if (!error && buffer.used != in_size)
		error = _game_state_schema_stream_layout;
	return error;
}

static void check_stream(void)
{
	static byte little[STATE_BYTES];
	static byte big[STATE_BYTES];
	static byte expected[STATE_BYTES];
	static byte taken[STATE_BYTES];
	static byte from_little[0x800];
	static byte from_big[0x800];
	static byte damaged[0x800];
	struct game_state_schema_state probe;
	struct game_state_schema_coverage coverage;
	long little_bytes;
	long big_bytes;
	long largest;
	long scratch_size;
	long offset;
	long index;
	short error;

	stream_regions_make(0x80001234UL, 0x80001250UL);
	stream_state_make(little, BASE, TAGS, 0x80001234UL, 0x80001250UL, FALSE, TRUE);
	stream_state_make(big, BASE, TAGS, 0x80001234UL, 0x80001250UL, TRUE, TRUE);
	probe = make_state(little, STATE_BYTES, BASE, TAGS, FALSE);
	probe.cpu_used = CPU_USED;
	probe.gpu_used = 0x20;
	probe.region_count = 5;
	probe.regions = stream_regions;
	game_state_schema_coverage(&probe, &coverage);
	CHECK(coverage.complete && coverage.typed_bytes == CPU_USED + 0x20 && !coverage.gap_count,
		"a state of typed classes passes the gate (%lu of %lu typed, %ld gaps)", coverage.typed_bytes,
		coverage.state_bytes, coverage.gap_count);
	CHECK(coverage.identity && coverage.identity == game_state_schema_stream_identity(&probe), "the stream's identity");
	scratch_size = game_state_schema_stream_scratch_bytes(&probe);
	CHECK(scratch_size == 332, "scratch: the widest instance's wire bytes (%ld)", scratch_size);

	error = stream_write_from(little, BASE, FALSE, from_little, sizeof(from_little), &little_bytes, &largest, scratch_size);
	CHECK(error == _game_state_schema_stream_ok && little_bytes == STREAM_BYTES, "little-endian stream: %s, %ld bytes",
		game_state_schema_stream_error_name(error), little_bytes);
	CHECK(largest <= scratch_size, "writes at most the scratch at once (%ld)", largest);
	error = stream_write_from(big, BASE, TRUE, from_big, sizeof(from_big), &big_bytes, NULL, scratch_size);
	CHECK(error == _game_state_schema_stream_ok && big_bytes == STREAM_BYTES, "big-endian stream: %s, %ld bytes",
		game_state_schema_stream_error_name(error), big_bytes);
	CHECK(little_bytes == big_bytes && !memcmp(from_little, from_big, (size_t)little_bytes),
		"the same state from either byte order is the same stream");
	dump_difference("stream", from_big, from_little, little_bytes);
	/* spots of it, by hand: the header first, a free element its identifier only */
	CHECK(from_little[0] == 0x11 && from_little[1] == 0xEE && from_little[2] == 0xFF && from_little[3] == 0xC0 &&
		from_little[4] == 'l', "the stream begins with the header's checksum, then its map name");
	CHECK(!memcmp(from_little + 332, mixed_wire, 31), "then the first mixed instance, as specified");
	{
		static byte const lightnings_elements[20] =
		{
			0x01, 0xE0, 0x00, 0x00, 0x34, 0x00, 0x12, 0x00, /* 0: in use */
			0x00, 0x00, /* 1: free */
			0x03, 0xE0, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, /* 2: in use */
			0x00, 0x00 /* 3: past the count */
		};

		CHECK(!memcmp(from_little + 332 + 62 + 54, lightnings_elements, 20), "the lightnings' elements, as specified");
	}
	{
		byte refused[0x800];
		long written;

		error = stream_write_from(little, BASE, FALSE, refused, sizeof(refused), &written, NULL, scratch_size - 4);
		CHECK(error == _game_state_schema_stream_memory && written == 0, "too little scratch: nothing written");
	}

	/* checked, not taken: nothing changes */
	stream_regions_make(0x80005678UL, 0x80005690UL);
	memset(taken, 0xCC, sizeof(taken));
	error = stream_read_into(taken, from_little, little_bytes, FALSE, NONE, &offset);
	CHECK(error == _game_state_schema_stream_ok, "the stream checks: %s at %ld", game_state_schema_stream_error_name(error), offset);
	for (index = 0; index < STATE_BYTES && taken[index] == 0xCC; index++)
		;
	CHECK(index == STATE_BYTES, "a check takes nothing (byte %ld changed)", index);

	/* taken, big-endian, at another base: as the state was, but free
	elements and padding zero, and this build's procedures */
	error = stream_read_into(taken, from_little, little_bytes, TRUE, NONE, &offset);
	stream_state_make(expected, OTHER_BASE, OTHER_TAGS, 0x80005678UL, 0x80005690UL, TRUE, FALSE);
	for (index = CPU_USED; index < GPU_MIXED_AT; index++)
		expected[index] = 0xCC; /* (no region's) */
	CHECK(error == _game_state_schema_stream_ok, "the stream is taken: %s at %ld", game_state_schema_stream_error_name(error), offset);
	CHECK(!memcmp(taken, expected, STATE_BYTES), "taken at another base, big-endian: the state as specified");
	dump_difference("taken", taken, expected, STATE_BYTES);

	/* damaged */
	memcpy(damaged, from_little, (size_t)little_bytes);
	damaged[332 + 18] = 0x7F;
	error = stream_read_into(taken, damaged, little_bytes, FALSE, NONE, &offset);
	CHECK(error == _game_state_schema_stream_pointer && offset == MIXED_AT, "a damaged pointer: %s at %ld",
		game_state_schema_stream_error_name(error), offset);
	memcpy(damaged, from_little, (size_t)little_bytes);
	damaged[332 + 62 + 44] = 0x7F; /* (the lightnings' count) */
	error = stream_read_into(taken, damaged, little_bytes, FALSE, NONE, &offset);
	CHECK(error == _game_state_schema_stream_data_array && offset == LIGHTNINGS_AT, "a damaged count: %s at %ld",
		game_state_schema_stream_error_name(error), offset);
	memcpy(damaged, from_little, (size_t)little_bytes);
	damaged[332 + 62 + 74 + 56] = 0x00; /* (the lruv cache's signature) */
	error = stream_read_into(taken, damaged, little_bytes, FALSE, NONE, &offset);
	CHECK(error == _game_state_schema_stream_lruv_cache && offset == CACHE_AT, "a damaged lruv cache: %s at %ld",
		game_state_schema_stream_error_name(error), offset);
	error = stream_read_into(taken, from_little, little_bytes - 3, FALSE, NONE, &offset);
	CHECK(error == _game_state_schema_stream_read, "a cut stream: %s", game_state_schema_stream_error_name(error));
	/* taking fails late: the state is partly the stream's (why a load checks
	all of it first, and a failure while taking leaves a state that must not
	run: game_state_image.c) */
	memset(taken, 0xCC, sizeof(taken));
	error = stream_read_into(taken, from_little, little_bytes, TRUE, little_bytes - 8, &offset);
	CHECK(error == _game_state_schema_stream_read && !memcmp(taken, expected, 0x14C) && taken[GPU_MIXED_AT] == 0xCC,
		"a stream failing while taken leaves the state partly taken");
}

int main(void)
{
	check_mixed();
	check_union();
	check_frames_and_cells();
	check_malformed();
	check_identity();
	check_gate();
	check_named_globals();
	check_stream();
	if (failures)
	{
		fprintf(stderr, "%d of %d cases failed\n", failures, cases);
		return 1;
	}
	printf("ok %d cases\n", cases);
	return 0;
}
