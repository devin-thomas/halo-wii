/*
GAME_STATE_SCHEMA.H

port (HWI-085, under HWI-080; HWI-046 stays open): the typed, portable
game-state image's schema and codec (game_state_schema.c).

The canonical image of version 2 (game_state_image.c) writes the game state
as native 32-bit words: a field narrower than a word keeps its place in the
word of the port that saved it, so an image is taken only by a port of the
same byte order. Version 3 (the typed image) writes each field by itself, by
the meaning a schema gives it, so that a little-endian and a big-endian port
that lay the same state out encode it to the same bytes.

A schema is a state class's fields: each a type, a byte offset in the
class's native layout and a count. Every byte of the class is claimed by a
field: padding is a field too (no bytes on the wire, zero when taken). A
field whose meaning the source does not establish (an HS cell whose upper
bits are kept from what wrote it, a script stack's frames, a memory pool's
blocks) is declared as such: its bytes are counted as unresolved, and the
completeness gate (game_state_schema_coverage) does not pass while any is in
the live game state. Nothing untyped is ever written as host memory.

Version 3 is written only when the gate passes for the whole live game
state; until then saves stay version 2 (GAME_STATE_IMAGE_WRITE_VERSION), and
a version-3 image is refused before anything is taken.
*/

#ifndef __GAME_STATE_SCHEMA_H
#define __GAME_STATE_SCHEMA_H
#pragma once

/* ---------- constants */

enum
{
	GAME_STATE_SCHEMA_MAXIMUM_FIELDS = 256,
	GAME_STATE_SCHEMA_MAXIMUM_SIZE = 0x10000,
	GAME_STATE_SCHEMA_MAXIMUM_DEPTH = 4,
	GAME_STATE_SCHEMA_MAXIMUM_ARMS = 16,
	/* the gaps a coverage report keeps (all are counted) */
	GAME_STATE_SCHEMA_MAXIMUM_GAPS = 48,
	/* the regions a game state may have */
	GAME_STATE_SCHEMA_MAXIMUM_REGIONS = 512,
};

/* a wire pointer: none 0; into the game state this plus its offset; into
the tag cache the other plus its offset (version 2's places) */
#define GAME_STATE_SCHEMA_POINTER_TAG 0x40000000UL
#define GAME_STATE_SCHEMA_TAG_POINTER_TAG 0x80000000UL
#define GAME_STATE_SCHEMA_POINTER_TAG_MASK 0xC0000000UL
#define GAME_STATE_SCHEMA_POINTER_OFFSET_MASK 0x3FFFFFFFUL
/* a script frame pointer on the wire: its byte offset in its range, or
this for none */
#define GAME_STATE_SCHEMA_FRAME_NONE 0xFFFFFFFFUL

/* what a field is: how it is written, and what it means */
enum game_state_schema_type
{
	/* bytes no code reads (a compiler's alignment hole): none on the wire,
	zero when taken */
	_game_state_schema_padding,
	/* integers of their width, written little-endian by value */
	_game_state_schema_u8,
	_game_state_schema_s8,
	_game_state_schema_boolean, /* (cseries.h's byte: kept as it is) */
	_game_state_schema_u16,
	_game_state_schema_s16,
	_game_state_schema_u32,
	_game_state_schema_s32,
	/* data.h's datum_header identifier: 0 marks a free datum */
	_game_state_schema_identifier,
	/* IEEE-754 single precision: its 32 bits, kept (NaN payloads too) */
	_game_state_schema_real,
	/* a datum (or tag) handle, identifier<<16 | absolute index, NONE for
	none: a value, never a pointer, kept as it is */
	_game_state_schema_datum_index,
	/* bytes (a name, a string): written as they are, count of them */
	_game_state_schema_bytes,
	/* a pointer into the game state, written as its place */
	_game_state_schema_state_pointer,
	/* a pointer into the game state or the tag cache, written as its place */
	_game_state_schema_state_or_tag_pointer,
	/* a pointer into a range of its own instance (relative_offset,
	relative_size), at a 2-byte offset there (hs_runtime.c's script frames):
	written as that offset, GAME_STATE_SCHEMA_FRAME_NONE for none */
	_game_state_schema_frame_pointer,
	/* a procedure (lruv_cache.h's): none on the wire, this build's when taken */
	_game_state_schema_procedure,
	/* memory_pool.h's block reference, made again from the block: none on
	the wire */
	_game_state_schema_pool_reference,
	/* another schema, count of them */
	_game_state_schema_struct,
	/* one of a union's arms, chosen by an earlier integer field; count is
	the union's bytes */
	_game_state_schema_union,
	/* hs_runtime.c's 32-bit value cell: a value of any HS type stored by
	the width of that type, its other bits kept from what was there before.
	Which bits mean what (and on which byte order) is not established:
	unresolved, never written */
	_game_state_schema_hs_cell,
	/* bytes of known extent whose meaning is not established (a union
	without its discriminator, a script stack): unresolved, never written */
	_game_state_schema_unresolved,

	NUMBER_OF_GAME_STATE_SCHEMA_TYPES
};

/* why a schema is not valid (game_state_schema_error_name) */
enum game_state_schema_error
{
	_game_state_schema_error_none,
	_game_state_schema_error_null,
	_game_state_schema_error_size,
	_game_state_schema_error_fields,
	_game_state_schema_error_name,
	_game_state_schema_error_unknown_type,
	_game_state_schema_error_count,
	_game_state_schema_error_bounds,
	_game_state_schema_error_overlap,
	_game_state_schema_error_alignment,
	_game_state_schema_error_unclaimed,
	_game_state_schema_error_nested,
	_game_state_schema_error_depth,
	_game_state_schema_error_union,
	_game_state_schema_error_frame_range,
	_game_state_schema_error_element,

	NUMBER_OF_GAME_STATE_SCHEMA_ERRORS
};

/* why a typed stream is not written or taken
(game_state_schema_stream_error_name) */
enum game_state_schema_stream_error
{
	_game_state_schema_stream_ok,
	_game_state_schema_stream_incomplete, /* the gate does not pass */
	_game_state_schema_stream_layout,
	_game_state_schema_stream_read,
	_game_state_schema_stream_write,
	_game_state_schema_stream_pointer,
	_game_state_schema_stream_frame_pointer,
	_game_state_schema_stream_union,
	_game_state_schema_stream_padding,
	_game_state_schema_stream_unresolved,
	_game_state_schema_stream_data_array,
	_game_state_schema_stream_lruv_cache,
	_game_state_schema_stream_memory,

	NUMBER_OF_GAME_STATE_SCHEMA_STREAM_ERRORS
};

/* what a region of the game state is */
enum game_state_schema_region_kind
{
	/* a game_state_malloc'd structure (or array of structures): schema's */
	_game_state_schema_region_global,
	/* a data array (data.h): its header, then its elements (schema's) */
	_game_state_schema_region_data_array,
	/* a memory pool (memory_pool.h): its header, then blocks of its owner's */
	_game_state_schema_region_memory_pool,
	/* an lruv cache (lruv_cache.h): its header, then its blocks' data array */
	_game_state_schema_region_lruv_cache,

	NUMBER_OF_GAME_STATE_SCHEMA_REGION_KINDS
};

/* why bytes of the game state are not covered */
enum game_state_schema_gap_reason
{
	_game_state_schema_gap_unclaimed,     /* no schema */
	_game_state_schema_gap_size,          /* a schema not of the region's (element's) size */
	_game_state_schema_gap_invalid,       /* a schema that does not validate */
	_game_state_schema_gap_hs_cell,       /* an HS cell (unresolved) */
	_game_state_schema_gap_unresolved,    /* declared, meaning not established */
	_game_state_schema_gap_pool_payload,  /* a memory pool's blocks' contents */
	_game_state_schema_gap_pointer,       /* a declared pointer the schema does not type as one */
	_game_state_schema_gap_outside,       /* game-state bytes in no region, or regions out of order */

	NUMBER_OF_GAME_STATE_SCHEMA_GAP_REASONS
};

/* ---------- macros */

/* a field of a schema's table: a scalar, bytes, padding, unresolved bytes */
#define GAME_STATE_SCHEMA_FIELD(name, type, offset, count) { (name), (type), (count), (offset), NULL, NULL, 0, 0 }
/* a struct's field: count instances of schema */
#define GAME_STATE_SCHEMA_STRUCT(name, schema, offset, count) { (name), _game_state_schema_struct, (count), (offset), (schema), NULL, 0, 0 }
/* a frame pointer into [range_offset, range_offset+range_size) of its instance */
#define GAME_STATE_SCHEMA_FRAME_POINTER(name, offset, range_offset, range_size) { (name), _game_state_schema_frame_pointer, 1, (offset), NULL, NULL, (range_offset), (range_size) }
#define GAME_STATE_SCHEMA_DEFINE(name, size, fields) { (name), (size), (short)(sizeof(fields) / sizeof((fields)[0])), (fields) }

/* ---------- structures */

struct game_state_schema;
struct game_state_schema_union;

struct game_state_schema_field
{
	char const *name;
	short type;
	short count; /* of the type (of bytes, for padding, bytes, unions and unresolved) */
	long offset; /* in the class's native layout */
	struct game_state_schema const *schema; /* a struct's */
	struct game_state_schema_union const *union_definition; /* a union's */
	long relative_offset; /* a frame pointer's range, in the instance */
	long relative_size;
};

struct game_state_schema
{
	char const *name;
	long size; /* the class's native size */
	short field_count; /* ascending offset, every byte claimed */
	struct game_state_schema_field const *fields;
};

struct game_state_schema_union_arm
{
	long value; /* of the discriminator */
	struct game_state_schema const *schema; /* at most the union's size */
};

struct game_state_schema_union
{
	/* the field (of the same schema, before the union) whose value chooses
	the arm: an integer field of count 1 */
	short discriminator_field;
	short arm_count;
	struct game_state_schema_union_arm const *arms;
};

/* a region of the game state and its schema */
struct game_state_schema_region
{
	char const *name;
	short kind;
	boolean gpu; /* in the GPU part */
	long offset; /* from the game state's base */
	long size;
	short maximum_count; /* a data array's, or an lruv cache's blocks' */
	short element_size;
	long pool_size;
	long page_count; /* an lruv cache's */
	long page_size_bits;
	/* a global's schema (of its size, or a divisor of it: an array of it),
	or a data array's elements' */
	struct game_state_schema const *schema;
	/* an lruv cache's procedures, this build's (32-bit addresses) */
	unsigned long procedures[2];
	/* the pointers version 2 was told of in it, cross-checked against the
	schema: a global's region offsets (game_state_port_pointer_field), a data
	array's element offsets (game_state_port_data_pointer_field) */
	short declared_pointer_count;
	long declared_pointers[4];
};

/* the game state the codec reads and writes: memory, at 32-bit address
base_address, of the port's byte order */
struct game_state_schema_state
{
	byte *memory;
	unsigned long base_address;
	long size;
	long cpu_used;
	long gpu_used;
	unsigned long tags_address;
	long tags_size;
	boolean big_endian;
	/* in the order game_state.c allocated them: the CPU part's up from its
	base, the GPU part's down from its end (the stream's order) */
	long region_count;
	struct game_state_schema_region const *regions;
};

struct game_state_schema_gap
{
	long offset; /* from the game state's base */
	long size;
	short reason;
	char const *owner; /* the region's name */
	char const *field;
};

/* what of the game state the schemas cover (game_state_schema_coverage) */
struct game_state_schema_coverage
{
	unsigned long state_bytes;
	unsigned long typed_bytes;
	unsigned long unresolved_bytes; /* declared, meaning not established */
	unsigned long unclaimed_bytes;  /* no (valid, fitting) schema */
	long regions;
	long typed_regions;      /* every byte typed */
	long unresolved_regions; /* schema'd, with unresolved bytes */
	long unclaimed_regions;  /* with unclaimed bytes */
	long conflicts;          /* declared pointers a schema does not type as one */
	long gap_count;          /* all of them */
	long gaps_first;         /* the first one kept (game_state_schema_coverage_page) */
	long gaps_kept;
	struct game_state_schema_gap gaps[GAME_STATE_SCHEMA_MAXIMUM_GAPS];
	/* the first schema that does not validate: its error, field and owner */
	short schema_error;
	short schema_error_field;
	char const *schema_error_owner;
	unsigned long identity; /* the stream's (game_state_schema_stream_identity) */
	boolean complete;
};

/* where the typed stream goes and comes from: whole transfers, and scratch
the codec uses (at least game_state_schema_stream_scratch_bytes) */
struct game_state_schema_stream
{
	void *context;
	boolean (*write)(void *context, byte const *bytes, long size);
	boolean (*read)(void *context, byte *bytes, long size);
	byte *scratch;
	long scratch_size;
};

/* ---------- prototypes */

short game_state_schema_validate(
	struct game_state_schema const *schema,
	short *field_index);
/* a data array's element schema: valid, of element_size, its first field
the datum's identifier */
short game_state_schema_validate_element(
	struct game_state_schema const *schema,
	short element_size,
	short *field_index);
char const *game_state_schema_error_name(
	short error);
char const *game_state_schema_stream_error_name(
	short error);
char const *game_state_schema_gap_reason_name(
	short reason);
/* the schema's wire identity: CRC-32 of its names, types, counts and
nested identities in order (not its native offsets) */
unsigned long game_state_schema_identity(
	struct game_state_schema const *schema);
/* its native layout's digest: the identity, its size and every offset */
unsigned long game_state_schema_layout_digest(
	struct game_state_schema const *schema);
/* the bytes an instance has on the wire; NONE if the schema is not valid */
long game_state_schema_wire_size(
	struct game_state_schema const *schema);
/* an instance (native, of state's byte order, at 32-bit address
instance_address) to its wire bytes: a game_state_schema_stream_error */
short game_state_schema_encode(
	struct game_state_schema const *schema,
	struct game_state_schema_state const *state,
	byte const *native,
	unsigned long instance_address,
	byte *wire,
	long wire_size);
/* wire bytes checked, and (native not NULL) taken into an instance */
short game_state_schema_decode(
	struct game_state_schema const *schema,
	struct game_state_schema_state const *state,
	byte const *wire,
	long wire_size,
	byte *native,
	unsigned long instance_address);
/* the completeness gate: what the regions' schemas cover of the game state */
void game_state_schema_coverage(
	struct game_state_schema_state const *state,
	struct game_state_schema_coverage *coverage);
/* the same, keeping the gaps from the first_gap'th on (all of them are
listed a page at a time) */
void game_state_schema_coverage_page(
	struct game_state_schema_state const *state,
	long first_gap,
	struct game_state_schema_coverage *coverage);
/* the identity of the stream the regions make: their kinds, sizes, counts
and schemas' identities and layouts */
unsigned long game_state_schema_stream_identity(
	struct game_state_schema_state const *state);
/* the scratch the stream needs */
long game_state_schema_stream_scratch_bytes(
	struct game_state_schema_state const *state);
/* the typed stream: refused (nothing written) unless the gate passes */
short game_state_schema_stream_write(
	struct game_state_schema_state const *state,
	struct game_state_schema_stream const *stream);
/* the typed stream read: checked, and with take, taken into state's memory */
short game_state_schema_stream_read(
	struct game_state_schema_state const *state,
	struct game_state_schema_stream const *stream,
	boolean take,
	long *error_offset);

/* the engine's own classes (game_state_schema_classes.c) */
extern struct game_state_schema const game_state_schema_data_array_header;
extern struct game_state_schema const game_state_schema_memory_pool_header;
extern struct game_state_schema const game_state_schema_memory_pool_block;
extern struct game_state_schema const game_state_schema_lruv_cache_header;
extern struct game_state_schema const game_state_schema_lruv_cache_block;
/* the schema of a state class by the name game_state.c allocated it under,
its kind and its size (a global's, or a data array's element's), or NULL */
struct game_state_schema const *game_state_schema_class_find(
	char const *name,
	short kind,
	long size);

#endif // __GAME_STATE_SCHEMA_H
