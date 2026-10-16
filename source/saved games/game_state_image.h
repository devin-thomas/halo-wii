/*
GAME_STATE_IMAGE.H

port (HWI-076, ADR-021): saved games as the canonical game-state image.

A saved game (a checkpoint, a core, the saved game of "save and quit") is
written and read as an image with no native pointer in it, in little-endian
32-bit words, in chunks of bounded size, each with its CRC, and a SHA-256 of
the whole. It is checked whole, in a first pass over the file, before any of
it is taken; a second pass then takes it into the game state. A pointer is
saved as its place in the game state and made again at the game state's
address, whatever that is. game_state_image.c describes the format.

port (HWI-085): version 3, the typed image, writes each field by itself by
its class's schema (game_state_schema.h), the same bytes on either byte
order. It is written only once the schemas cover the whole game state (the
gate, game_state_image_schema_coverage); until then saves are version 2
(GAME_STATE_IMAGE_WRITE_VERSION), and a version-3 image is refused before
anything of it is taken. A version-2 image is read as it always was.
*/

#ifndef __GAME_STATE_IMAGE_H
#define __GAME_STATE_IMAGE_H
#pragma once

/* ---------- constants */

/* (HWI-085) the version saves are written in: version 2 until the typed
image's gate passes for the live game state (a version-3 save is refused
while it does not) */
#ifndef GAME_STATE_IMAGE_WRITE_VERSION
#define GAME_STATE_IMAGE_WRITE_VERSION GAME_STATE_IMAGE_VERSION
#endif

enum
{
	GAME_STATE_IMAGE_VERSION = 2,
	/* (HWI-085) the typed image: written only when the gate passes */
	GAME_STATE_IMAGE_TYPED_VERSION = 3,
	GAME_STATE_IMAGE_HEADER_BYTES = 512,
	GAME_STATE_IMAGE_CHUNK_HEADER_BYTES = 16,
	GAME_STATE_IMAGE_CHUNK_BYTES = 0x10000,
	/* pointer fields a data array's elements may declare
	(game_state_port_data_pointer_field) */
	GAME_STATE_IMAGE_MAXIMUM_POINTER_FIELDS = 4,
};

enum game_state_image_allocation_kind
{
	_game_state_image_data,
	_game_state_image_memory_pool,
	_game_state_image_lruv_cache,
};

/* a pointer word of the game state outside its data arrays, memory pools
and lruv caches (game_state_port_pointer_field) */
enum game_state_image_pointer_kind
{
	/* none, or into the game state or the tag cache */
	_game_state_image_pointer,
	/* a union's word: such a pointer, or a value that reads as none of them
	(hud_messaging.c's icon or text) */
	_game_state_image_pointer_or_value,
};

struct game_state_image_pointer_word
{
	long offset; /* from the game state's base, 4-aligned */
	short kind;
};

enum game_state_image_encoding
{
	_game_state_image_encoding_plain,
	/* hs_runtime.c's script threads: each holds pointers into its own
	stack, at two-byte offsets */
	_game_state_image_encoding_script_threads,
};

/* what a load comes to */
enum game_state_image_result
{
	_game_state_image_taken,
	/* not taken: the game state is as it was */
	_game_state_image_refused,
	/* the file read differently the second time (its chunks no longer
	those checked): the game state is partly the file's and must not run */
	_game_state_image_failed_while_taking,
};

/* why an image is refused (game_state_image_refusal_name) */
enum game_state_image_refusal
{
	_game_state_image_refusal_none,
	_game_state_image_refusal_read,
	_game_state_image_refusal_magic,
	_game_state_image_refusal_version,
	_game_state_image_refusal_header_checksum,
	_game_state_image_refusal_incomplete,
	_game_state_image_refusal_byte_order,
	_game_state_image_refusal_encoding,
	_game_state_image_refusal_layout,
	_game_state_image_refusal_build,
	_game_state_image_refusal_map,
	_game_state_image_refusal_players,
	_game_state_image_refusal_map_checksum,
	_game_state_image_refusal_chunk,
	_game_state_image_refusal_chunk_checksum,
	_game_state_image_refusal_segment,
	_game_state_image_refusal_length,
	_game_state_image_refusal_digest,
	_game_state_image_refusal_pointer,
	_game_state_image_refusal_data_array,
	_game_state_image_refusal_memory_pool,
	_game_state_image_refusal_lruv_cache,
	_game_state_image_refusal_script_thread,
	_game_state_image_refusal_base,
	_game_state_image_refusal_memory,
	/* (HWI-085) a typed image the schemas do not cover, or of others */
	_game_state_image_refusal_schema,

	NUMBER_OF_GAME_STATE_IMAGE_REFUSALS
};

/* ---------- structures */

struct game_state_header;
struct game_state_schema_region;
struct game_state_schema_coverage;

/* one data array, memory pool or lruv cache of the game state, as
game_state.c made it */
struct game_state_image_allocation
{
	short kind;
	short encoding;
	long offset; /* from the game state's base */
	long size;
	short maximum_count; /* a data array's, or an lruv cache's blocks' */
	short element_size;
	long pool_size; /* a memory pool's */
	long page_count;
	long page_size_bits;
	void (*delete_block_proc)(long);
	boolean (*locked_block_proc)(long);
	/* a data array's: the byte offsets in each element in use of pointers
	into the game state or the tag cache */
	short pointer_field_count;
	short pointer_fields[GAME_STATE_IMAGE_MAXIMUM_POINTER_FIELDS];
};

/* the game state as this build makes it */
struct game_state_image_layout
{
	byte *base;
	long size;     /* the CPU and GPU parts' room */
	long cpu_used; /* from the base */
	long gpu_used; /* down from base+size */
	unsigned long allocation_size_checksum;
	/* the tag cache, which pointers in the game state may point into */
	byte *tags;
	long tags_size;
	/* the pointer words outside the allocations above, in ascending offset */
	long pointer_word_count;
	struct game_state_image_pointer_word const *pointer_words;
	long allocation_count; /* in ascending offset */
	struct game_state_image_allocation const *allocations;
	/* (HWI-085) every allocation with its class's schema, for the typed
	image (game_state_schema.h's order) */
	long region_count;
	struct game_state_schema_region const *regions;
};

/* how an image is read and written: whole transfers of size bytes */
struct game_state_image_file
{
	void *context;
	boolean (*read)(void *context, void *buffer, long size);
	boolean (*write)(void *context, void const *buffer, long size);
	boolean (*seek)(void *context, long offset);
};

/* what the last save or load did (game_state_image_last_report) */
struct game_state_image_report
{
	boolean save;
	short refusal;
	long refusal_offset; /* the game-state offset, or the chunk, it concerns */
	unsigned long state_bytes;
	unsigned long file_bytes;
	unsigned long payload_bytes;
	unsigned long chunks;
	unsigned long chunk_bytes;
	unsigned long digest[8]; /* SHA-256 of the payload, as eight big-endian words */
	unsigned long pointers;          /* pointers made places in the game state or the tag cache */
	unsigned long tag_pointers;      /* of those, places in the tag cache */
	unsigned long references;        /* memory pool blocks' references made again from their blocks */
	unsigned long script_threads;
	unsigned long foreign_pointers;  /* pointers outside the game state, kept as they are */
	/* other words that read as an address in the game state: in what the
	game reads, and the first one's offset; in room nothing reads */
	unsigned long address_like_words;
	long first_address_like_offset;
	unsigned long address_like_dead_words;
	unsigned long peak_bytes;        /* the most taken from the heap at once */
	unsigned long save_base;
	boolean base_bound;
	long passes;
	/* (HWI-085) the image's version; a typed image's stream bytes and
	schema identity, and the codec's error (game_state_schema.h) */
	long version;
	unsigned long typed_bytes;
	unsigned long schema_identity;
	short schema_error;
};

/* ---------- prototypes */

/* writes the game state as an image: the header (with header's map, build,
players and checksums), then the payload in chunks; the header is written
again at the end, marked complete */
boolean game_state_image_write(
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	struct game_state_image_file const *file);
/* (HWI-085) the same, of a version: GAME_STATE_IMAGE_VERSION, or
GAME_STATE_IMAGE_TYPED_VERSION (refused, nothing written, unless the gate
passes) */
boolean game_state_image_write_version(
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	struct game_state_image_file const *file,
	long version);
/* (HWI-085) what the schemas cover of the game state, keeping its gaps
from the first_gap'th on: TRUE when the gate passes (the typed image can be
written and taken) */
boolean game_state_image_schema_coverage(
	struct game_state_image_layout const *layout,
	long first_gap,
	struct game_state_schema_coverage *coverage);
/* reads an image's header; FALSE (and why, in the report) if it is not one
of this format, or not complete */
boolean game_state_image_read_header(
	struct game_state_image_file const *file,
	struct game_state_header *header);
/* checks a whole image against this build's game state, takes nothing */
boolean game_state_image_check(
	struct game_state_image_layout const *layout,
	struct game_state_header const *expected,
	struct game_state_image_file const *file);
/* checks a whole image (the expected header's map, build, players and
checksums, when given) and only then, before_taking called first, takes it
into the game state */
short game_state_image_load(
	struct game_state_image_layout const *layout,
	struct game_state_header const *expected,
	struct game_state_image_file const *file,
	void (*before_taking)(void));
struct game_state_image_report const *game_state_image_last_report(
	void);
char const *game_state_image_refusal_name(
	short refusal);
/* told after each save and load, and after each check (port harnesses
measure them); event: 0 save, 1 load, 2 check, and 3 as each begins */
void game_state_image_set_observer(
	void (*observer)(short event, struct game_state_image_report const *report));
/* the SHA-256 the image of the game state as it is would have, written
nowhere (a harness's check that a load gives back what was saved) */
boolean game_state_image_digest(
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	unsigned long digest[8]);

/* port (HWI-079): the game-state offsets of the pointers the image makes
places (and the declared fields of deleted elements below a data array's
count), ascending, in offsets; how many there are (more than capacity: not
all were stored, nor sorted), or NONE if the layout is not valid */
long game_state_image_pointer_places(
	struct game_state_image_layout const *layout,
	long *offsets,
	long capacity);

#endif // __GAME_STATE_IMAGE_H
