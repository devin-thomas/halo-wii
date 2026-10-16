/*
GAME_STATE_SCHEMA.C

port (HWI-085, under HWI-080; HWI-046 stays open): the typed game-state
image's schemas, their checks and identity, the completeness gate, and the
codec of the typed stream (version 3's payload, game_state_image.c frames
it in its chunks).

---------- an instance on the wire

An instance's wire bytes are its fields', in the schema's order (ascending
native offset), each by its type (game_state_schema.h):
- an integer, a datum handle, a real's bits: little-endian, of its width,
  read from the instance by value in the port's own byte order;
- bytes: as they are;
- a pointer: its place (0 none, 0x40000000 plus a game-state offset,
  0x80000000 plus a tag-cache offset where the type allows one); a pointer
  out of both is refused (version 3 has no image bound to its base);
- a frame pointer: its offset in its range of the instance, 0xFFFFFFFF none;
- padding, a procedure, a pool block's reference: no bytes (taken as zero;
  an lruv cache's procedures are then this build's);
- a struct: its instances' in turn;
- a union: its chosen arm's (each arm of the union's whole size), then zero
  to the widest arm's wire size;
- an HS cell, unresolved bytes: never (refused: the gate does not pass).

---------- the stream

The regions in order (the CPU part's up from its base, the GPU part's down
from its end), each:
- a global: each instance's wire bytes (a region of a whole number of its
  schema's instances);
- a data array: its header's, then for each element up to its maximum count
  its identifier (2 bytes, little-endian) and, for one in use (below its
  count, identifier not 0), the rest of its wire bytes. A free element is its
  identifier only: datum_initialize (data.c) clears an element whole before
  it is used again, so nothing reads what a free one held; taken, it is zero
  but for its identifier;
- an lruv cache: its header's, then its blocks' data array as above;
- a memory pool: never. Its blocks' contents are its owner's (the objects'),
  which no schema types: the gate does not pass while one is in the state.

The gate (game_state_schema_coverage) passes only when every byte of the
game state's CPU and GPU parts lies in a region whose schema validates, fits
and has nothing unresolved, and every pointer version 2 was told of is one
the schema types as a pointer. The stream is neither written nor read
otherwise.
*/

/* ---------- headers */

#ifndef GAME_STATE_SCHEMA_STANDALONE
#include "cseries/cseries.h"
#endif
#include "saved games/game_state_schema.h"

/* ---------- constants */

enum
{
	DATA_ARRAY_HEADER_BYTES = 0x38,
	DATA_ARRAY_MAXIMUM_COUNT = 0x20,
	DATA_ARRAY_SIZE = 0x22,
	DATA_ARRAY_VALID = 0x24,
	DATA_ARRAY_SIGNATURE = 0x28,
	DATA_ARRAY_FIRST_FREE = 0x2C,
	DATA_ARRAY_COUNT = 0x2E,
	DATA_ARRAY_ACTUAL_COUNT = 0x30,
	DATA_ARRAY_NEXT_IDENTIFIER = 0x32,
	DATA_ARRAY_DATA = 0x34,
	DATA_ARRAY_NAME_BYTES = 32,

	MEMORY_POOL_HEADER_BYTES = 0x38,

	LRUV_CACHE_HEADER_BYTES = 0x44,
	LRUV_CACHE_NAME_BYTES = 32,
	LRUV_CACHE_DELETE_PROC = 0x20,
	LRUV_CACHE_LOCKED_PROC = 0x24,
	LRUV_CACHE_PAGE_COUNT = 0x28,
	LRUV_CACHE_PAGE_SIZE_BITS = 0x2C,
	LRUV_CACHE_BLOCKS = 0x3C,
	LRUV_CACHE_SIGNATURE = 0x40,
	LRUV_CACHE_BLOCK_BYTES = 0x1C,

	IDENTIFIER_BYTES = 2,
};

/* (data.h's and lruv_cache.h's four-character constants: 'd@t@', 'weee') */
#define DATA_ARRAY_SIGNATURE_VALUE 0x64407440UL
#define LRUV_CACHE_SIGNATURE_VALUE 0x77656565UL
#define WORD_MASK 0xFFFFFFFFUL

/* ---------- private code: bytes */

static void schema_bytes_copy(
	byte *destination,
	byte const *source,
	long size)
{
	long index;

	for (index = 0; index < size; index++)
		destination[index] = source[index];
}

static void schema_bytes_zero(
	byte *destination,
	long size)
{
	long index;

	for (index = 0; index < size; index++)
		destination[index] = 0;
}

/* a value of width 1, 2 or 4 bytes in the port's byte order */
static unsigned long schema_native_get(
	byte const *bytes,
	long width,
	boolean big_endian)
{
	unsigned long value = 0;
	long index;

	for (index = 0; index < width; index++)
	{
		long at = big_endian ? index : width - 1 - index;

		value = (value << 8) | bytes[at];
	}

	return value & WORD_MASK;
}

static void schema_native_put(
	byte *bytes,
	long width,
	unsigned long value,
	boolean big_endian)
{
	long index;

	for (index = 0; index < width; index++)
	{
		long at = big_endian ? width - 1 - index : index;

		bytes[at] = (byte)(value & 0xFF);
		value >>= 8;
	}
}

static unsigned long schema_wire_get(
	byte const *bytes,
	long width)
{
	return schema_native_get(bytes, width, FALSE);
}

static void schema_wire_put(
	byte *bytes,
	long width,
	unsigned long value)
{
	schema_native_put(bytes, width, value, FALSE);
}

/* ---------- private code: CRC-32 (crc.c's: polynomial 0xEDB88320, from
0xFFFFFFFF, not inverted at the end) */

static unsigned long schema_crc_bytes(
	unsigned long crc,
	byte const *bytes,
	long size)
{
	long index;

	for (index = 0; index < size; index++)
	{
		short bit;

		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
	}

	return crc & WORD_MASK;
}

static unsigned long schema_crc_word(
	unsigned long crc,
	unsigned long value)
{
	byte bytes[4];

	schema_wire_put(bytes, 4, value);

	return schema_crc_bytes(crc, bytes, 4);
}

static unsigned long schema_crc_string(
	unsigned long crc,
	char const *string)
{
	long length = 0;

	if (string)
	{
		while (string[length])
			length++;
		crc = schema_crc_bytes(crc, (byte const *)string, length);
	}

	return schema_crc_word(crc, (unsigned long)length);
}

/* ---------- private code: types */

static boolean schema_type_integer(
	short type)
{
	switch (type)
	{
	case _game_state_schema_u8:
	case _game_state_schema_s8:
	case _game_state_schema_boolean:
	case _game_state_schema_u16:
	case _game_state_schema_s16:
	case _game_state_schema_u32:
	case _game_state_schema_s32:
		return TRUE;
	}

	return FALSE;
}

static boolean schema_type_signed(
	short type)
{
	return type == _game_state_schema_s8 || type == _game_state_schema_s16 || type == _game_state_schema_s32 ||
		type == _game_state_schema_identifier || type == _game_state_schema_datum_index;
}

static boolean schema_type_pointer(
	short type)
{
	return type == _game_state_schema_state_pointer || type == _game_state_schema_state_or_tag_pointer;
}

/* a scalar type's width (of each of count), 0 for the others */
static long schema_type_width(
	short type)
{
	switch (type)
	{
	case _game_state_schema_u8:
	case _game_state_schema_s8:
	case _game_state_schema_boolean:
	case _game_state_schema_bytes:
		return 1;
	case _game_state_schema_u16:
	case _game_state_schema_s16:
	case _game_state_schema_identifier:
		return 2;
	case _game_state_schema_u32:
	case _game_state_schema_s32:
	case _game_state_schema_real:
	case _game_state_schema_datum_index:
	case _game_state_schema_state_pointer:
	case _game_state_schema_state_or_tag_pointer:
	case _game_state_schema_frame_pointer:
	case _game_state_schema_procedure:
	case _game_state_schema_pool_reference:
	case _game_state_schema_hs_cell:
		return 4;
	}

	return 0;
}

/* where a field may lie in its instance: a frame pointer at a 2-byte
offset (a script frame's own), the others of their width */
static long schema_type_alignment(
	short type)
{
	if (type == _game_state_schema_frame_pointer || type == _game_state_schema_hs_cell)
		return 2;
	if (type == _game_state_schema_bytes)
		return 1;

	return schema_type_width(type) ? schema_type_width(type) : 1;
}

/* a field's native bytes; NONE if not known (a struct without its schema) */
static long schema_field_native_size(
	struct game_state_schema_field const *field)
{
	switch (field->type)
	{
	case _game_state_schema_padding:
	case _game_state_schema_union:
	case _game_state_schema_unresolved:
		return field->count;
	case _game_state_schema_struct:
		return field->schema && field->schema->size > 0 ? (long)field->count * field->schema->size : NONE;
	}

	return (long)field->count * schema_type_width(field->type);
}

/* ---------- private code: validation */

static short schema_validate_depth(
	struct game_state_schema const *schema,
	short depth,
	short *field_index);

static short schema_validate_union(
	struct game_state_schema const *schema,
	short index,
	short depth)
{
	struct game_state_schema_field const *field = &schema->fields[index];
	struct game_state_schema_union const *definition = field->union_definition;
	struct game_state_schema_field const *discriminator;
	short arm;
	short other;

	if (!definition || field->schema || !definition->arms || definition->arm_count < 1 ||
		definition->arm_count > GAME_STATE_SCHEMA_MAXIMUM_ARMS ||
		definition->discriminator_field < 0 || definition->discriminator_field >= index)
	{
		return _game_state_schema_error_union;
	}
	discriminator = &schema->fields[definition->discriminator_field];
	if (!schema_type_integer(discriminator->type) || discriminator->count != 1)
		return _game_state_schema_error_union;
	for (arm = 0; arm < definition->arm_count; arm++)
	{
		short ignored;

		if (schema_validate_depth(definition->arms[arm].schema, (short)(depth + 1), &ignored) ||
			definition->arms[arm].schema->size != field->count)
		{
			return _game_state_schema_error_union;
		}
		for (other = 0; other < arm; other++)
		{
			if (definition->arms[other].value == definition->arms[arm].value)
				return _game_state_schema_error_union;
		}
	}

	return _game_state_schema_error_none;
}

static short schema_validate_depth(
	struct game_state_schema const *schema,
	short depth,
	short *field_index)
{
	long end = 0;
	short index;

	*field_index = NONE;
	if (!schema)
		return _game_state_schema_error_null;
	if (depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return _game_state_schema_error_depth;
	if (!schema->name)
		return _game_state_schema_error_name;
	if (schema->size <= 0 || schema->size > GAME_STATE_SCHEMA_MAXIMUM_SIZE)
		return _game_state_schema_error_size;
	if (schema->field_count <= 0 || schema->field_count > GAME_STATE_SCHEMA_MAXIMUM_FIELDS || !schema->fields)
		return _game_state_schema_error_fields;
	for (index = 0; index < schema->field_count; index++)
	{
		struct game_state_schema_field const *field = &schema->fields[index];
		long size;

		*field_index = index;
		if (!field->name)
			return _game_state_schema_error_name;
		if (field->type < 0 || field->type >= NUMBER_OF_GAME_STATE_SCHEMA_TYPES)
			return _game_state_schema_error_unknown_type;
		if (field->count < 1)
			return _game_state_schema_error_count;
		if (field->type == _game_state_schema_struct)
		{
			short nested;

			if (field->union_definition || schema_validate_depth(field->schema, (short)(depth + 1), &nested))
				return _game_state_schema_error_nested;
			*field_index = index;
		}
		else if (field->type == _game_state_schema_union)
		{
			short error = schema_validate_union(schema, index, depth);

			if (error)
				return error;
		}
		else if (field->schema || field->union_definition)
		{
			return _game_state_schema_error_nested;
		}
		size = schema_field_native_size(field);
		if (field->offset < end)
			return _game_state_schema_error_overlap;
		if (size <= 0 || size > GAME_STATE_SCHEMA_MAXIMUM_SIZE || field->offset > schema->size - size)
			return _game_state_schema_error_bounds;
		if (field->offset > end)
			return _game_state_schema_error_unclaimed;
		if (field->offset % schema_type_alignment(field->type))
			return _game_state_schema_error_alignment;
		if (field->type == _game_state_schema_frame_pointer)
		{
			if (field->relative_offset < 0 || field->relative_size < 2 || (field->relative_size & 1) ||
				field->relative_offset > schema->size - field->relative_size)
			{
				return _game_state_schema_error_frame_range;
			}
		}
		else if (field->relative_offset || field->relative_size)
		{
			return _game_state_schema_error_frame_range;
		}
		end = field->offset + size;
	}
	*field_index = NONE;
	if (end != schema->size)
		return _game_state_schema_error_unclaimed;

	return _game_state_schema_error_none;
}

/* ---------- private code: identity and sizes (of valid schemas) */

static unsigned long schema_identity_depth(
	struct game_state_schema const *schema,
	short depth,
	boolean layout)
{
	unsigned long crc = 0xFFFFFFFFUL;
	short index;

	if (!schema || depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return 0;
	crc = schema_crc_string(crc, schema->name);
	if (layout)
	{
		crc = schema_crc_word(crc, (unsigned long)schema->field_count);
		crc = schema_crc_word(crc, (unsigned long)schema->size);
	}
	for (index = 0; index < schema->field_count; index++)
	{
		struct game_state_schema_field const *field = &schema->fields[index];

		/* (padding is nothing on the wire: only the layout's) */
		if (!layout && field->type == _game_state_schema_padding)
			continue;
		crc = schema_crc_string(crc, field->name);
		crc = schema_crc_word(crc, (unsigned long)field->type);
		crc = schema_crc_word(crc, (unsigned long)field->count);
		if (layout)
		{
			crc = schema_crc_word(crc, (unsigned long)field->offset);
			crc = schema_crc_word(crc, (unsigned long)field->relative_offset);
		}
		if (field->type == _game_state_schema_frame_pointer)
			crc = schema_crc_word(crc, (unsigned long)field->relative_size);
		if (field->type == _game_state_schema_struct)
			crc = schema_crc_word(crc, schema_identity_depth(field->schema, (short)(depth + 1), layout));
		if (field->type == _game_state_schema_union && field->union_definition)
		{
			struct game_state_schema_union const *definition = field->union_definition;
			short arm;

			/* (the discriminator by name: its index counts padding) */
			crc = schema_crc_string(crc, schema->fields[definition->discriminator_field].name);
			crc = schema_crc_word(crc, (unsigned long)definition->arm_count);
			for (arm = 0; arm < definition->arm_count; arm++)
			{
				crc = schema_crc_word(crc, (unsigned long)definition->arms[arm].value);
				crc = schema_crc_word(crc, schema_identity_depth(definition->arms[arm].schema, (short)(depth + 1), layout));
			}
		}
	}
	/* (the end of the fields) */
	crc = schema_crc_word(crc, WORD_MASK);

	return crc & WORD_MASK;
}

static long schema_wire_size_depth(
	struct game_state_schema const *schema,
	short depth);

static long schema_field_wire_size(
	struct game_state_schema_field const *field,
	short depth)
{
	switch (field->type)
	{
	case _game_state_schema_padding:
	case _game_state_schema_procedure:
	case _game_state_schema_pool_reference:
		return 0;
	case _game_state_schema_unresolved:
		return field->count;
	case _game_state_schema_struct:
		return (long)field->count * schema_wire_size_depth(field->schema, (short)(depth + 1));
	case _game_state_schema_union:
	{
		long widest = 0;
		short arm;

		for (arm = 0; arm < field->union_definition->arm_count; arm++)
		{
			long size = schema_wire_size_depth(field->union_definition->arms[arm].schema, (short)(depth + 1));

			if (size > widest)
				widest = size;
		}
		return widest;
	}
	}

	return (long)field->count * schema_type_width(field->type);
}

static long schema_wire_size_depth(
	struct game_state_schema const *schema,
	short depth)
{
	long size = 0;
	short index;

	if (depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return 0;
	for (index = 0; index < schema->field_count; index++)
		size += schema_field_wire_size(&schema->fields[index], depth);

	return size;
}

/* the wire offset of a field in its instance's wire bytes */
static long schema_field_wire_offset(
	struct game_state_schema const *schema,
	short field_index,
	short depth)
{
	long offset = 0;
	short index;

	for (index = 0; index < field_index; index++)
		offset += schema_field_wire_size(&schema->fields[index], depth);

	return offset;
}

static unsigned long schema_unresolved_bytes(
	struct game_state_schema const *schema,
	short depth);

static unsigned long schema_field_unresolved_bytes(
	struct game_state_schema_field const *field,
	short depth)
{
	switch (field->type)
	{
	case _game_state_schema_hs_cell:
	case _game_state_schema_unresolved:
		return (unsigned long)schema_field_native_size(field);
	case _game_state_schema_struct:
		return (unsigned long)field->count * schema_unresolved_bytes(field->schema, (short)(depth + 1));
	case _game_state_schema_union:
	{
		short arm;

		/* (an arm that leaves anything unresolved leaves the union so) */
		for (arm = 0; arm < field->union_definition->arm_count; arm++)
		{
			if (schema_unresolved_bytes(field->union_definition->arms[arm].schema, (short)(depth + 1)))
				return (unsigned long)field->count;
		}
		return 0;
	}
	}

	return 0;
}

static unsigned long schema_unresolved_bytes(
	struct game_state_schema const *schema,
	short depth)
{
	unsigned long bytes = 0;
	short index;

	if (depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return 0;
	for (index = 0; index < schema->field_count; index++)
		bytes += schema_field_unresolved_bytes(&schema->fields[index], depth);

	return bytes;
}

/* whether the schema types the word at offset as a pointer (in every arm
of a union it lies in) */
static boolean schema_pointer_at(
	struct game_state_schema const *schema,
	long offset,
	short depth)
{
	short index;

	if (depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return FALSE;
	for (index = 0; index < schema->field_count; index++)
	{
		struct game_state_schema_field const *field = &schema->fields[index];
		long size = schema_field_native_size(field);
		long at = offset - field->offset;

		if (at < 0 || at >= size)
			continue;
		if (schema_type_pointer(field->type))
			return (at % 4) == 0;
		if (field->type == _game_state_schema_struct)
			return schema_pointer_at(field->schema, at % field->schema->size, (short)(depth + 1));
		if (field->type == _game_state_schema_union)
		{
			short arm;

			for (arm = 0; arm < field->union_definition->arm_count; arm++)
			{
				if (!schema_pointer_at(field->union_definition->arms[arm].schema, at, (short)(depth + 1)))
					return FALSE;
			}
			return TRUE;
		}
		return FALSE;
	}

	return FALSE;
}

/* ---------- private code: the codec */

struct schema_codec
{
	struct game_state_schema_state const *state;
	byte const *wire_in;
	byte *wire_out;
	long wire_at;
	long wire_size;
};

static unsigned long schema_sign_extend(
	unsigned long value,
	long width)
{
	if (width < 4 && (value & (1UL << (width * 8 - 1))))
		value |= WORD_MASK << (width * 8);

	return value & WORD_MASK;
}

static short schema_encode_pointer(
	struct game_state_schema_state const *state,
	short type,
	unsigned long value,
	unsigned long *wire)
{
	unsigned long offset = (value - state->base_address) & WORD_MASK;

	if (!value)
	{
		*wire = 0;
		return _game_state_schema_stream_ok;
	}
	if (offset < (unsigned long)state->size)
	{
		*wire = GAME_STATE_SCHEMA_POINTER_TAG | offset;
		return _game_state_schema_stream_ok;
	}
	offset = (value - state->tags_address) & WORD_MASK;
	if (type == _game_state_schema_state_or_tag_pointer && state->tags_size > 0 && offset < (unsigned long)state->tags_size)
	{
		*wire = GAME_STATE_SCHEMA_TAG_POINTER_TAG | offset;
		return _game_state_schema_stream_ok;
	}

	return _game_state_schema_stream_pointer;
}

static short schema_decode_pointer(
	struct game_state_schema_state const *state,
	short type,
	unsigned long wire,
	unsigned long *value)
{
	unsigned long offset = wire & GAME_STATE_SCHEMA_POINTER_OFFSET_MASK;

	if (!wire)
	{
		*value = 0;
		return _game_state_schema_stream_ok;
	}
	if ((wire & GAME_STATE_SCHEMA_POINTER_TAG_MASK) == GAME_STATE_SCHEMA_POINTER_TAG && offset < (unsigned long)state->size)
	{
		*value = (state->base_address + offset) & WORD_MASK;
		return _game_state_schema_stream_ok;
	}
	if (type == _game_state_schema_state_or_tag_pointer &&
		(wire & GAME_STATE_SCHEMA_POINTER_TAG_MASK) == GAME_STATE_SCHEMA_TAG_POINTER_TAG &&
		state->tags_size > 0 && offset < (unsigned long)state->tags_size)
	{
		*value = (state->tags_address + offset) & WORD_MASK;
		return _game_state_schema_stream_ok;
	}

	return _game_state_schema_stream_pointer;
}

static struct game_state_schema const *schema_union_arm(
	struct game_state_schema_union const *definition,
	unsigned long discriminator)
{
	short arm;

	for (arm = 0; arm < definition->arm_count; arm++)
	{
		if (((unsigned long)definition->arms[arm].value & WORD_MASK) == discriminator)
			return definition->arms[arm].schema;
	}

	return NULL;
}

static short schema_encode_instance(
	struct schema_codec *codec,
	struct game_state_schema const *schema,
	byte const *native,
	unsigned long address,
	short depth)
{
	boolean big_endian = codec->state->big_endian;
	short index;

	if (depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return _game_state_schema_stream_layout;
	for (index = 0; index < schema->field_count; index++)
	{
		struct game_state_schema_field const *field = &schema->fields[index];
		byte const *bytes = native + field->offset;
		long width = schema_type_width(field->type);
		long wire_size = schema_field_wire_size(field, depth);
		short element;

		if (codec->wire_at > codec->wire_size - wire_size)
			return _game_state_schema_stream_layout;
		switch (field->type)
		{
		case _game_state_schema_padding:
		case _game_state_schema_procedure:
		case _game_state_schema_pool_reference:
			break;
		case _game_state_schema_hs_cell:
		case _game_state_schema_unresolved:
			return _game_state_schema_stream_unresolved;
		case _game_state_schema_struct:
			for (element = 0; element < field->count; element++)
			{
				short error = schema_encode_instance(codec, field->schema, bytes + (long)element * field->schema->size,
					(address + field->offset + (unsigned long)element * field->schema->size) & WORD_MASK, (short)(depth + 1));

				if (error)
					return error;
			}
			break;
		case _game_state_schema_union:
		{
			struct game_state_schema_field const *discriminator = &schema->fields[field->union_definition->discriminator_field];
			long discriminator_width = schema_type_width(discriminator->type);
			unsigned long value = schema_native_get(native + discriminator->offset, discriminator_width, big_endian);
			struct game_state_schema const *arm;
			long start = codec->wire_at;
			short error;

			if (schema_type_signed(discriminator->type))
				value = schema_sign_extend(value, discriminator_width);
			arm = schema_union_arm(field->union_definition, value);
			if (!arm)
				return _game_state_schema_stream_union;
			error = schema_encode_instance(codec, arm, bytes, (address + field->offset) & WORD_MASK, (short)(depth + 1));
			if (error)
				return error;
			schema_bytes_zero(codec->wire_out + codec->wire_at, start + wire_size - codec->wire_at);
			codec->wire_at = start + wire_size;
			break;
		}
		case _game_state_schema_bytes:
		case _game_state_schema_u8:
		case _game_state_schema_s8:
		case _game_state_schema_boolean:
			schema_bytes_copy(codec->wire_out + codec->wire_at, bytes, field->count);
			codec->wire_at += field->count;
			break;
		default:
			for (element = 0; element < field->count; element++)
			{
				unsigned long value = schema_native_get(bytes + (long)element * width, width, big_endian);

				if (schema_type_pointer(field->type))
				{
					short error = schema_encode_pointer(codec->state, field->type, value, &value);

					if (error)
						return error;
				}
				else if (field->type == _game_state_schema_frame_pointer)
				{
					unsigned long range = (address + field->relative_offset) & WORD_MASK;
					unsigned long at = (value - range) & WORD_MASK;

					if (!value)
						value = GAME_STATE_SCHEMA_FRAME_NONE;
					else if (at >= (unsigned long)field->relative_size || (at & 1))
						return _game_state_schema_stream_frame_pointer;
					else
						value = at;
				}
				schema_wire_put(codec->wire_out + codec->wire_at, width, value);
				codec->wire_at += width;
			}
			break;
		}
	}

	return _game_state_schema_stream_ok;
}

/* (native NULL: only checked) */
static short schema_decode_instance(
	struct schema_codec *codec,
	struct game_state_schema const *schema,
	byte *native,
	unsigned long address,
	short depth)
{
	boolean big_endian = codec->state->big_endian;
	long instance_wire = codec->wire_at;
	short index;

	if (depth >= GAME_STATE_SCHEMA_MAXIMUM_DEPTH)
		return _game_state_schema_stream_layout;
	for (index = 0; index < schema->field_count; index++)
	{
		struct game_state_schema_field const *field = &schema->fields[index];
		byte *bytes = native ? native + field->offset : NULL;
		long width = schema_type_width(field->type);
		long wire_size = schema_field_wire_size(field, depth);
		short element;

		if (codec->wire_at > codec->wire_size - wire_size)
			return _game_state_schema_stream_layout;
		switch (field->type)
		{
		case _game_state_schema_padding:
		case _game_state_schema_procedure:
		case _game_state_schema_pool_reference:
			if (bytes)
				schema_bytes_zero(bytes, schema_field_native_size(field));
			break;
		case _game_state_schema_hs_cell:
		case _game_state_schema_unresolved:
			return _game_state_schema_stream_unresolved;
		case _game_state_schema_struct:
			for (element = 0; element < field->count; element++)
			{
				short error = schema_decode_instance(codec, field->schema,
					bytes ? bytes + (long)element * field->schema->size : NULL,
					(address + field->offset + (unsigned long)element * field->schema->size) & WORD_MASK, (short)(depth + 1));

				if (error)
					return error;
			}
			break;
		case _game_state_schema_union:
		{
			short discriminator_index = field->union_definition->discriminator_field;
			struct game_state_schema_field const *discriminator = &schema->fields[discriminator_index];
			long discriminator_width = schema_type_width(discriminator->type);
			unsigned long value = schema_wire_get(codec->wire_in + instance_wire +
				schema_field_wire_offset(schema, discriminator_index, depth), discriminator_width);
			struct game_state_schema const *arm;
			long start = codec->wire_at;
			short error;

			if (schema_type_signed(discriminator->type))
				value = schema_sign_extend(value, discriminator_width);
			arm = schema_union_arm(field->union_definition, value);
			if (!arm)
				return _game_state_schema_stream_union;
			error = schema_decode_instance(codec, arm, bytes, (address + field->offset) & WORD_MASK, (short)(depth + 1));
			if (error)
				return error;
			/* (the rest to the widest arm is zero) */
			for (; codec->wire_at < start + wire_size; codec->wire_at++)
			{
				if (codec->wire_in[codec->wire_at])
					return _game_state_schema_stream_padding;
			}
			break;
		}
		case _game_state_schema_bytes:
		case _game_state_schema_u8:
		case _game_state_schema_s8:
		case _game_state_schema_boolean:
			if (bytes)
				schema_bytes_copy(bytes, codec->wire_in + codec->wire_at, field->count);
			codec->wire_at += field->count;
			break;
		default:
			for (element = 0; element < field->count; element++)
			{
				unsigned long value = schema_wire_get(codec->wire_in + codec->wire_at, width);

				if (schema_type_pointer(field->type))
				{
					short error = schema_decode_pointer(codec->state, field->type, value, &value);

					if (error)
						return error;
				}
				else if (field->type == _game_state_schema_frame_pointer)
				{
					if (value == GAME_STATE_SCHEMA_FRAME_NONE)
						value = 0;
					else if (value >= (unsigned long)field->relative_size || (value & 1))
						return _game_state_schema_stream_frame_pointer;
					else
						value = (address + field->relative_offset + value) & WORD_MASK;
				}
				if (bytes)
					schema_native_put(bytes + (long)element * width, width, value, big_endian);
				codec->wire_at += width;
			}
			break;
		}
	}

	return _game_state_schema_stream_ok;
}

/* ---------- private code: the coverage */

static void coverage_gap(
	struct game_state_schema_coverage *coverage,
	long offset,
	long size,
	short reason,
	char const *owner,
	char const *field)
{
	if (coverage->gap_count >= coverage->gaps_first && coverage->gaps_kept < GAME_STATE_SCHEMA_MAXIMUM_GAPS)
	{
		struct game_state_schema_gap *gap = &coverage->gaps[coverage->gaps_kept++];

		gap->offset = offset;
		gap->size = size;
		gap->reason = reason;
		gap->owner = owner;
		gap->field = field;
	}
	coverage->gap_count++;
}

static void coverage_unclaimed(
	struct game_state_schema_coverage *coverage,
	long offset,
	long size,
	short reason,
	char const *owner,
	char const *field)
{
	coverage_gap(coverage, offset, size, reason, owner, field);
	coverage->unclaimed_bytes += (unsigned long)size;
}

static void coverage_schema_error(
	struct game_state_schema_coverage *coverage,
	short error,
	short field,
	char const *owner)
{
	if (!coverage->schema_error)
	{
		coverage->schema_error = error;
		coverage->schema_error_field = field;
		coverage->schema_error_owner = owner;
	}
}

/* count instances of a schema at offset: typed, and each unresolved field a
gap (its bytes in every instance); FALSE if it does not validate */
static boolean coverage_instances(
	struct game_state_schema_coverage *coverage,
	struct game_state_schema_region const *region,
	struct game_state_schema const *schema,
	long offset,
	long count,
	long stride,
	boolean element)
{
	short field_index;
	short error = element ?
		game_state_schema_validate_element(schema, (short)stride, &field_index) :
		game_state_schema_validate(schema, &field_index);
	unsigned long unresolved;
	short index;

	if (error)
	{
		coverage_schema_error(coverage, error, field_index, region->name);
		coverage_unclaimed(coverage, offset, count * stride,
			error == _game_state_schema_error_element && schema && schema->size != stride ?
				_game_state_schema_gap_size : _game_state_schema_gap_invalid,
			region->name, schema ? schema->name : NULL);
		return FALSE;
	}
	unresolved = schema_unresolved_bytes(schema, 0);
	coverage->typed_bytes += (unsigned long)count * ((unsigned long)schema->size - unresolved);
	coverage->unresolved_bytes += (unsigned long)count * unresolved;
	for (index = 0; index < schema->field_count; index++)
	{
		struct game_state_schema_field const *field = &schema->fields[index];
		unsigned long bytes = schema_field_unresolved_bytes(field, 0);

		if (bytes)
		{
			coverage_gap(coverage, offset + field->offset, (long)bytes * count,
				field->type == _game_state_schema_hs_cell ? _game_state_schema_gap_hs_cell : _game_state_schema_gap_unresolved,
				region->name, field->name);
		}
	}
	/* the pointers version 2 was told of: the schema's pointers too */
	for (index = 0; index < region->declared_pointer_count && index < (short)NUMBEROF(region->declared_pointers); index++)
	{
		long at = region->declared_pointers[index];

		if (at < 0 || !schema_pointer_at(schema, at % schema->size, 0))
		{
			coverage->conflicts++;
			coverage_gap(coverage, offset + at, 4, _game_state_schema_gap_pointer, region->name, schema->name);
		}
	}
	if (region->declared_pointer_count > (short)NUMBEROF(region->declared_pointers))
	{
		coverage->conflicts++;
		coverage_gap(coverage, offset, 4, _game_state_schema_gap_pointer, region->name, schema->name);
	}

	return unresolved == 0;
}

/* the bytes a region of its kind has, NONE if not one this file knows */
static long coverage_region_size(
	struct game_state_schema_region const *region)
{
	switch (region->kind)
	{
	case _game_state_schema_region_global:
		return region->size;
	case _game_state_schema_region_data_array:
		return DATA_ARRAY_HEADER_BYTES + (long)region->maximum_count * region->element_size;
	case _game_state_schema_region_memory_pool:
		return MEMORY_POOL_HEADER_BYTES + region->pool_size;
	case _game_state_schema_region_lruv_cache:
		return LRUV_CACHE_HEADER_BYTES + DATA_ARRAY_HEADER_BYTES + (long)region->maximum_count * LRUV_CACHE_BLOCK_BYTES;
	}

	return NONE;
}

static void coverage_region(
	struct game_state_schema_coverage *coverage,
	struct game_state_schema_region const *region)
{
	unsigned long unclaimed = coverage->unclaimed_bytes;
	unsigned long unresolved = coverage->unresolved_bytes;
	long conflicts = coverage->conflicts;
	long elements = region->offset + DATA_ARRAY_HEADER_BYTES;

	coverage->regions++;
	if (region->size <= 0 || region->maximum_count < 0 || region->element_size < 0 || region->pool_size < 0 ||
		coverage_region_size(region) != region->size)
	{
		coverage_unclaimed(coverage, region->offset, region->size > 0 ? region->size : 0, _game_state_schema_gap_size,
			region->name, NULL);
	}
	else
	{
		switch (region->kind)
		{
		case _game_state_schema_region_global:
			if (!region->schema)
				coverage_unclaimed(coverage, region->offset, region->size, _game_state_schema_gap_unclaimed, region->name, NULL);
			else if (region->schema->size <= 0 || region->size % region->schema->size)
				coverage_unclaimed(coverage, region->offset, region->size, _game_state_schema_gap_size, region->name, region->schema->name);
			else
				coverage_instances(coverage, region, region->schema, region->offset, region->size / region->schema->size,
					region->schema->size, FALSE);
			break;
		case _game_state_schema_region_data_array:
			coverage->typed_bytes += DATA_ARRAY_HEADER_BYTES;
			if (!region->maximum_count || !region->element_size)
				break;
			if (!region->schema)
			{
				coverage_unclaimed(coverage, elements, (long)region->maximum_count * region->element_size,
					_game_state_schema_gap_unclaimed, region->name, "elements");
			}
			else
			{
				coverage_instances(coverage, region, region->schema, elements, region->maximum_count, region->element_size, TRUE);
			}
			break;
		case _game_state_schema_region_memory_pool:
			coverage->typed_bytes += MEMORY_POOL_HEADER_BYTES;
			if (region->pool_size)
			{
				coverage_unclaimed(coverage, region->offset + MEMORY_POOL_HEADER_BYTES, region->pool_size,
					_game_state_schema_gap_pool_payload, region->name, "blocks");
			}
			break;
		case _game_state_schema_region_lruv_cache:
			coverage->typed_bytes += (unsigned long)region->size;
			break;
		}
	}
	if (coverage->unclaimed_bytes != unclaimed)
		coverage->unclaimed_regions++;
	else if (coverage->unresolved_bytes != unresolved)
		coverage->unresolved_regions++;
	else if (coverage->conflicts == conflicts)
		coverage->typed_regions++;
}

/* ---------- private code: the stream */

struct schema_stream
{
	struct game_state_schema_state const *state;
	struct game_state_schema_stream const *stream;
	long error_offset;
};

static short stream_write_instance(
	struct schema_stream *context,
	struct game_state_schema const *schema,
	long offset,
	long skip)
{
	struct game_state_schema_state const *state = context->state;
	struct schema_codec codec;
	short error;

	codec.state = state;
	codec.wire_in = NULL;
	codec.wire_out = context->stream->scratch;
	codec.wire_at = 0;
	codec.wire_size = schema_wire_size_depth(schema, 0);
	context->error_offset = offset;
	if (codec.wire_size > context->stream->scratch_size)
		return _game_state_schema_stream_memory;
	error = schema_encode_instance(&codec, schema, state->memory + offset,
		(state->base_address + (unsigned long)offset) & WORD_MASK, 0);
	if (error)
		return error;
	if (codec.wire_at != codec.wire_size)
		return _game_state_schema_stream_layout;
	if (codec.wire_size > skip && !context->stream->write(context->stream->context, codec.wire_out + skip, codec.wire_size - skip))
		return _game_state_schema_stream_write;

	return _game_state_schema_stream_ok;
}

/* (the wire bytes are in scratch, the first skip of them already there) */
static short stream_read_instance(
	struct schema_stream *context,
	struct game_state_schema const *schema,
	long offset,
	long skip,
	byte *native)
{
	struct game_state_schema_state const *state = context->state;
	struct schema_codec codec;

	codec.state = state;
	codec.wire_in = context->stream->scratch;
	codec.wire_out = NULL;
	codec.wire_at = 0;
	codec.wire_size = schema_wire_size_depth(schema, 0);
	context->error_offset = offset;
	if (codec.wire_size > context->stream->scratch_size)
		return _game_state_schema_stream_memory;
	if (codec.wire_size > skip &&
		!context->stream->read(context->stream->context, context->stream->scratch + skip, codec.wire_size - skip))
	{
		return _game_state_schema_stream_read;
	}

	return schema_decode_instance(&codec, schema, native, (state->base_address + (unsigned long)offset) & WORD_MASK, 0);
}

/* a data array's header, as the codec reads it: its own, its counts in
range (upstream's game_state_image_data_valid) */
static boolean stream_data_array_valid(
	struct game_state_schema_state const *state,
	byte const *header,
	long offset,
	short maximum_count,
	short element_size)
{
	boolean big_endian = state->big_endian;
	long count = (long)(short)schema_native_get(header + DATA_ARRAY_COUNT, 2, big_endian);
	long actual_count = (long)(short)schema_native_get(header + DATA_ARRAY_ACTUAL_COUNT, 2, big_endian);
	long first_free = (long)(short)schema_native_get(header + DATA_ARRAY_FIRST_FREE, 2, big_endian);

	return schema_native_get(header + DATA_ARRAY_SIGNATURE, 4, big_endian) == DATA_ARRAY_SIGNATURE_VALUE &&
		(short)schema_native_get(header + DATA_ARRAY_MAXIMUM_COUNT, 2, big_endian) == maximum_count &&
		(short)schema_native_get(header + DATA_ARRAY_SIZE, 2, big_endian) == element_size &&
		schema_native_get(header + DATA_ARRAY_DATA, 4, big_endian) ==
			((state->base_address + (unsigned long)offset + DATA_ARRAY_HEADER_BYTES) & WORD_MASK) &&
		count >= 0 && count <= maximum_count && first_free >= 0 && first_free <= maximum_count &&
		actual_count >= 0 && actual_count <= count;
}

static short stream_write_data_array(
	struct schema_stream *context,
	long offset,
	short maximum_count,
	short element_size,
	struct game_state_schema const *schema)
{
	struct game_state_schema_state const *state = context->state;
	byte const *header = state->memory + offset;
	long count = (long)(short)schema_native_get(header + DATA_ARRAY_COUNT, 2, state->big_endian);
	short error;
	long index;

	context->error_offset = offset;
	if (!stream_data_array_valid(state, header, offset, maximum_count, element_size))
		return _game_state_schema_stream_data_array;
	error = stream_write_instance(context, &game_state_schema_data_array_header, offset, 0);
	if (error)
		return error;
	for (index = 0; index < maximum_count; index++)
	{
		long element = offset + DATA_ARRAY_HEADER_BYTES + index * element_size;
		unsigned long identifier = schema_native_get(state->memory + element, IDENTIFIER_BYTES, state->big_endian);

		if (index < count && identifier)
		{
			error = stream_write_instance(context, schema, element, 0);
			if (error)
				return error;
		}
		else
		{
			byte bytes[IDENTIFIER_BYTES];

			schema_wire_put(bytes, IDENTIFIER_BYTES, identifier);
			if (!context->stream->write(context->stream->context, bytes, IDENTIFIER_BYTES))
				return _game_state_schema_stream_write;
		}
	}

	return _game_state_schema_stream_ok;
}

static short stream_read_data_array(
	struct schema_stream *context,
	long offset,
	short maximum_count,
	short element_size,
	struct game_state_schema const *schema,
	boolean take)
{
	struct game_state_schema_state const *state = context->state;
	byte header[DATA_ARRAY_HEADER_BYTES];
	long count;
	short error;
	long index;

	error = stream_read_instance(context, &game_state_schema_data_array_header, offset, 0, header);
	if (error)
		return error;
	context->error_offset = offset;
	header[DATA_ARRAY_NAME_BYTES - 1] = 0;
	if (!stream_data_array_valid(state, header, offset, maximum_count, element_size))
		return _game_state_schema_stream_data_array;
	/* (version 2's: valid is a boolean, a valid array gives out identifiers,
	and an array this game state has valid is not taken as not) */
	header[DATA_ARRAY_VALID] = header[DATA_ARRAY_VALID] != 0;
	header[DATA_ARRAY_VALID + 1] = header[DATA_ARRAY_VALID + 1] != 0;
	if ((header[DATA_ARRAY_VALID] && !schema_native_get(header + DATA_ARRAY_NEXT_IDENTIFIER, 2, state->big_endian)) ||
		(!header[DATA_ARRAY_VALID] && state->memory[offset + DATA_ARRAY_VALID]))
	{
		return _game_state_schema_stream_data_array;
	}
	count = (long)(short)schema_native_get(header + DATA_ARRAY_COUNT, 2, state->big_endian);
	if (take)
		schema_bytes_copy(state->memory + offset, header, DATA_ARRAY_HEADER_BYTES);
	for (index = 0; index < maximum_count; index++)
	{
		long element = offset + DATA_ARRAY_HEADER_BYTES + index * element_size;
		unsigned long identifier;

		context->error_offset = element;
		if (!context->stream->read(context->stream->context, context->stream->scratch, IDENTIFIER_BYTES))
			return _game_state_schema_stream_read;
		identifier = schema_wire_get(context->stream->scratch, IDENTIFIER_BYTES);
		if (index < count && identifier)
		{
			error = stream_read_instance(context, schema, element, IDENTIFIER_BYTES, take ? state->memory + element : NULL);
			if (error)
				return error;
		}
		else if (take)
		{
			schema_bytes_zero(state->memory + element, element_size);
			schema_native_put(state->memory + element, IDENTIFIER_BYTES, identifier, state->big_endian);
		}
	}

	return _game_state_schema_stream_ok;
}

static boolean stream_lruv_cache_valid(
	struct game_state_schema_state const *state,
	struct game_state_schema_region const *region,
	byte const *header)
{
	boolean big_endian = state->big_endian;

	return schema_native_get(header + LRUV_CACHE_SIGNATURE, 4, big_endian) == LRUV_CACHE_SIGNATURE_VALUE &&
		schema_native_get(header + LRUV_CACHE_BLOCKS, 4, big_endian) ==
			((state->base_address + (unsigned long)region->offset + LRUV_CACHE_HEADER_BYTES) & WORD_MASK) &&
		schema_native_get(header + LRUV_CACHE_PAGE_COUNT, 4, big_endian) == ((unsigned long)region->page_count & WORD_MASK) &&
		schema_native_get(header + LRUV_CACHE_PAGE_SIZE_BITS, 4, big_endian) == ((unsigned long)region->page_size_bits & WORD_MASK);
}

static short stream_region(
	struct schema_stream *context,
	struct game_state_schema_region const *region,
	boolean write,
	boolean take)
{
	struct game_state_schema_state const *state = context->state;
	short error = _game_state_schema_stream_ok;

	context->error_offset = region->offset;
	switch (region->kind)
	{
	case _game_state_schema_region_global:
	{
		long instance;

		for (instance = 0; instance < region->size / region->schema->size && !error; instance++)
		{
			long offset = region->offset + instance * region->schema->size;

			error = write ? stream_write_instance(context, region->schema, offset, 0) :
				stream_read_instance(context, region->schema, offset, 0, take ? state->memory + offset : NULL);
		}
		break;
	}
	case _game_state_schema_region_data_array:
		error = write ?
			stream_write_data_array(context, region->offset, region->maximum_count, region->element_size, region->schema) :
			stream_read_data_array(context, region->offset, region->maximum_count, region->element_size, region->schema, take);
		break;
	case _game_state_schema_region_lruv_cache:
	{
		long blocks = region->offset + LRUV_CACHE_HEADER_BYTES;

		if (write)
		{
			if (!stream_lruv_cache_valid(state, region, state->memory + region->offset))
				return _game_state_schema_stream_lruv_cache;
			error = stream_write_instance(context, &game_state_schema_lruv_cache_header, region->offset, 0);
			if (!error)
				error = stream_write_data_array(context, blocks, region->maximum_count, LRUV_CACHE_BLOCK_BYTES,
					&game_state_schema_lruv_cache_block);
		}
		else
		{
			byte header[LRUV_CACHE_HEADER_BYTES];

			error = stream_read_instance(context, &game_state_schema_lruv_cache_header, region->offset, 0, header);
			if (error)
				return error;
			context->error_offset = region->offset;
			header[LRUV_CACHE_NAME_BYTES - 1] = 0;
			if (!stream_lruv_cache_valid(state, region, header))
				return _game_state_schema_stream_lruv_cache;
			/* (this build's procedures) */
			schema_native_put(header + LRUV_CACHE_DELETE_PROC, 4, region->procedures[0], state->big_endian);
			schema_native_put(header + LRUV_CACHE_LOCKED_PROC, 4, region->procedures[1], state->big_endian);
			if (take)
				schema_bytes_copy(state->memory + region->offset, header, LRUV_CACHE_HEADER_BYTES);
			error = stream_read_data_array(context, blocks, region->maximum_count, LRUV_CACHE_BLOCK_BYTES,
				&game_state_schema_lruv_cache_block, take);
		}
		break;
	}
	default:
		/* (a memory pool: the gate does not pass) */
		error = _game_state_schema_stream_incomplete;
		break;
	}

	return error;
}

/* whether the state is one the codec can walk: the gate, and room enough */
static short stream_ready(
	struct game_state_schema_state const *state,
	struct game_state_schema_stream const *stream)
{
	struct game_state_schema_coverage coverage;

	game_state_schema_coverage(state, &coverage);
	if (!coverage.complete)
		return _game_state_schema_stream_incomplete;
	if (!stream || !stream->scratch || stream->scratch_size < game_state_schema_stream_scratch_bytes(state))
		return _game_state_schema_stream_memory;

	return _game_state_schema_stream_ok;
}

/* ---------- public code */

short game_state_schema_validate(
	struct game_state_schema const *schema,
	short *field_index)
{
	short ignored;

	return schema_validate_depth(schema, 0, field_index ? field_index : &ignored);
}

short game_state_schema_validate_element(
	struct game_state_schema const *schema,
	short element_size,
	short *field_index)
{
	short ignored;
	short error;

	if (!field_index)
		field_index = &ignored;
	error = schema_validate_depth(schema, 0, field_index);
	if (error)
		return error;
	*field_index = 0;
	if (schema->size != element_size || schema->fields[0].type != _game_state_schema_identifier ||
		schema->fields[0].offset != 0 || schema->fields[0].count != 1)
	{
		return _game_state_schema_error_element;
	}
	*field_index = NONE;

	return _game_state_schema_error_none;
}

char const *game_state_schema_error_name(
	short error)
{
	static char const *const names[NUMBER_OF_GAME_STATE_SCHEMA_ERRORS] =
	{
		"none", "null", "size", "fields", "name", "unknown_type", "count", "bounds", "overlap", "alignment",
		"unclaimed", "nested", "depth", "union", "frame_range", "element"
	};

	return error >= 0 && error < NUMBER_OF_GAME_STATE_SCHEMA_ERRORS ? names[error] : "unknown";
}

char const *game_state_schema_stream_error_name(
	short error)
{
	static char const *const names[NUMBER_OF_GAME_STATE_SCHEMA_STREAM_ERRORS] =
	{
		"ok", "incomplete", "layout", "read", "write", "pointer", "frame_pointer", "union", "padding", "unresolved",
		"data_array", "lruv_cache", "memory"
	};

	return error >= 0 && error < NUMBER_OF_GAME_STATE_SCHEMA_STREAM_ERRORS ? names[error] : "unknown";
}

char const *game_state_schema_gap_reason_name(
	short reason)
{
	static char const *const names[NUMBER_OF_GAME_STATE_SCHEMA_GAP_REASONS] =
	{
		"unclaimed", "size", "invalid", "hs_cell", "unresolved", "pool_payload", "pointer", "outside"
	};

	return reason >= 0 && reason < NUMBER_OF_GAME_STATE_SCHEMA_GAP_REASONS ? names[reason] : "unknown";
}

unsigned long game_state_schema_identity(
	struct game_state_schema const *schema)
{
	return game_state_schema_validate(schema, NULL) ? 0 : schema_identity_depth(schema, 0, FALSE);
}

unsigned long game_state_schema_layout_digest(
	struct game_state_schema const *schema)
{
	return game_state_schema_validate(schema, NULL) ? 0 : schema_identity_depth(schema, 0, TRUE);
}

long game_state_schema_wire_size(
	struct game_state_schema const *schema)
{
	return game_state_schema_validate(schema, NULL) ? NONE : schema_wire_size_depth(schema, 0);
}

short game_state_schema_encode(
	struct game_state_schema const *schema,
	struct game_state_schema_state const *state,
	byte const *native,
	unsigned long instance_address,
	byte *wire,
	long wire_size)
{
	struct schema_codec codec;
	short error;

	if (!state || !native || !wire || game_state_schema_wire_size(schema) != wire_size)
		return _game_state_schema_stream_layout;
	codec.state = state;
	codec.wire_in = NULL;
	codec.wire_out = wire;
	codec.wire_at = 0;
	codec.wire_size = wire_size;
	error = schema_encode_instance(&codec, schema, native, instance_address & WORD_MASK, 0);
	if (!error && codec.wire_at != wire_size)
		error = _game_state_schema_stream_layout;

	return error;
}

short game_state_schema_decode(
	struct game_state_schema const *schema,
	struct game_state_schema_state const *state,
	byte const *wire,
	long wire_size,
	byte *native,
	unsigned long instance_address)
{
	struct schema_codec codec;
	short error;

	if (!state || !wire || game_state_schema_wire_size(schema) != wire_size)
		return _game_state_schema_stream_layout;
	codec.state = state;
	codec.wire_in = wire;
	codec.wire_out = NULL;
	codec.wire_at = 0;
	codec.wire_size = wire_size;
	error = schema_decode_instance(&codec, schema, native, instance_address & WORD_MASK, 0);
	if (!error && codec.wire_at != wire_size)
		error = _game_state_schema_stream_layout;

	return error;
}

void game_state_schema_coverage(
	struct game_state_schema_state const *state,
	struct game_state_schema_coverage *coverage)
{
	game_state_schema_coverage_page(state, 0, coverage);
}

void game_state_schema_coverage_page(
	struct game_state_schema_state const *state,
	long first_gap,
	struct game_state_schema_coverage *coverage)
{
	long expected = 0;
	boolean gpu = FALSE;
	long index;

	schema_bytes_zero((byte *)coverage, (long)sizeof(*coverage));
	coverage->schema_error_field = NONE;
	coverage->gaps_first = first_gap > 0 ? first_gap : 0;
	if (!state || state->size <= 0 || state->cpu_used < 0 || state->gpu_used < 0 ||
		state->cpu_used > state->size - state->gpu_used || state->region_count < 0 ||
		state->region_count > GAME_STATE_SCHEMA_MAXIMUM_REGIONS || (state->region_count && !state->regions))
	{
		coverage_gap(coverage, 0, 0, _game_state_schema_gap_outside, NULL, NULL);
		return;
	}
	coverage->state_bytes = (unsigned long)(state->cpu_used + state->gpu_used);
	for (index = 0; index < state->region_count; index++)
	{
		struct game_state_schema_region const *region = &state->regions[index];

		if (region->gpu && !gpu)
		{
			/* (the CPU part's end, then down from the GPU part's) */
			if (expected < state->cpu_used)
				coverage_unclaimed(coverage, expected, state->cpu_used - expected, _game_state_schema_gap_outside, NULL, NULL);
			gpu = TRUE;
			expected = state->size;
		}
		if (region->gpu != gpu || region->size < 0 ||
			(gpu ? region->offset + region->size != expected || region->offset < state->size - state->gpu_used :
				region->offset != expected || region->offset > state->cpu_used - region->size))
		{
			/* out of order, overlapping or outside: the walk cannot trust it */
			coverage_gap(coverage, region->offset, region->size, _game_state_schema_gap_outside, region->name, NULL);
			coverage->unclaimed_bytes += coverage->state_bytes;
			return;
		}
		coverage_region(coverage, region);
		expected = gpu ? region->offset : region->offset + region->size;
	}
	if (!gpu && expected < state->cpu_used)
		coverage_unclaimed(coverage, expected, state->cpu_used - expected, _game_state_schema_gap_outside, NULL, NULL);
	if (gpu ? expected > state->size - state->gpu_used : state->gpu_used > 0)
	{
		coverage_unclaimed(coverage, state->size - state->gpu_used,
			gpu ? expected - (state->size - state->gpu_used) : state->gpu_used, _game_state_schema_gap_outside, NULL, NULL);
	}
	coverage->identity = game_state_schema_stream_identity(state);
	coverage->complete = !coverage->gap_count && !coverage->schema_error && !coverage->conflicts &&
		!coverage->unresolved_bytes && !coverage->unclaimed_bytes && coverage->typed_bytes == coverage->state_bytes;
}

unsigned long game_state_schema_stream_identity(
	struct game_state_schema_state const *state)
{
	unsigned long crc = 0xFFFFFFFFUL;
	long index;

	if (!state || state->region_count < 0 || (state->region_count && !state->regions))
		return 0;
	crc = schema_crc_word(crc, (unsigned long)state->size);
	crc = schema_crc_word(crc, (unsigned long)state->cpu_used);
	crc = schema_crc_word(crc, (unsigned long)state->gpu_used);
	crc = schema_crc_word(crc, (unsigned long)state->region_count);
	for (index = 0; index < state->region_count; index++)
	{
		struct game_state_schema_region const *region = &state->regions[index];

		crc = schema_crc_string(crc, region->name);
		crc = schema_crc_word(crc, (unsigned long)region->kind);
		crc = schema_crc_word(crc, (unsigned long)region->gpu);
		crc = schema_crc_word(crc, (unsigned long)region->offset);
		crc = schema_crc_word(crc, (unsigned long)region->size);
		crc = schema_crc_word(crc, (unsigned long)region->maximum_count);
		crc = schema_crc_word(crc, (unsigned long)region->element_size);
		crc = schema_crc_word(crc, (unsigned long)region->pool_size);
		crc = schema_crc_word(crc, (unsigned long)region->page_count);
		crc = schema_crc_word(crc, (unsigned long)region->page_size_bits);
		crc = schema_crc_word(crc, game_state_schema_identity(region->schema));
		crc = schema_crc_word(crc, game_state_schema_layout_digest(region->schema));
	}

	return crc & WORD_MASK;
}

long game_state_schema_stream_scratch_bytes(
	struct game_state_schema_state const *state)
{
	long largest = game_state_schema_wire_size(&game_state_schema_lruv_cache_header);
	long index;

	if (game_state_schema_wire_size(&game_state_schema_data_array_header) > largest)
		largest = game_state_schema_wire_size(&game_state_schema_data_array_header);
	if (game_state_schema_wire_size(&game_state_schema_lruv_cache_block) > largest)
		largest = game_state_schema_wire_size(&game_state_schema_lruv_cache_block);
	for (index = 0; state && index < state->region_count; index++)
	{
		long size = game_state_schema_wire_size(state->regions[index].schema);

		if (size > largest)
			largest = size;
	}

	return (largest + 3) & ~3L;
}

short game_state_schema_stream_write(
	struct game_state_schema_state const *state,
	struct game_state_schema_stream const *stream)
{
	struct schema_stream context;
	short error = stream_ready(state, stream);
	long index;

	if (error)
		return error;
	if (!stream->write)
		return _game_state_schema_stream_write;
	context.state = state;
	context.stream = stream;
	context.error_offset = 0;
	for (index = 0; index < state->region_count && !error; index++)
		error = stream_region(&context, &state->regions[index], TRUE, FALSE);

	return error;
}

short game_state_schema_stream_read(
	struct game_state_schema_state const *state,
	struct game_state_schema_stream const *stream,
	boolean take,
	long *error_offset)
{
	struct schema_stream context;
	short error = stream_ready(state, stream);
	long index;

	context.state = state;
	context.stream = stream;
	context.error_offset = 0;
	if (!error && !stream->read)
		error = _game_state_schema_stream_read;
	for (index = 0; !error && index < state->region_count; index++)
		error = stream_region(&context, &state->regions[index], FALSE, take);
	if (error_offset)
		*error_offset = context.error_offset;

	return error;
}
