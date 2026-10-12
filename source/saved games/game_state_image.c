/*
GAME_STATE_IMAGE.C

port (HWI-076, ADR-021): the canonical game-state image. Every port saves
and loads its game through it: a checkpoint, a revert to it, a loss's
revert, a core saved and loaded, and the saved game of "save and quit".

The upstream saved game was the game state's memory written out as it was,
pointers and all, and read back over it whole (on the native ports into a
second buffer of the game state's size first, to be checked before it was
taken). This image has no native pointer in it, is written and read in
chunks of bounded size, and is checked whole before any of it is taken.

---------- format (version 2; every integer little-endian)

header, 512 bytes:
	  0 "HWGS"
	  4 version (2)
	  8 header bytes (512)
	 12 flags: 1 complete (written last), 2 bound to its base (below)
	 16 byte order of the port that wrote it: 1 little-endian, 2 big-endian
	 20 payload encoding (1: the words below, zero runs collapsed)
	 24 chunk bytes (65,536)
	 28 chunk count
	 32 payload bytes
	 36 game-state bytes in the image (CPU part used, then GPU part used)
	 40 CPU part used
	 44 GPU part used
	 48 the game state's room (CPU and GPU parts)
	 52 allocation count (data arrays, memory pools, lruv caches)
	 56 layout digest: CRC of each allocation's kind, encoding, offset,
	    size, capacity and element size, and the parts' sizes (below)
	 60 allocation checksum (the upstream header's)
	 64 cache file checksum (the map's)
	 68 player count (16 bits), 70 difficulty (16 bits)
	 72 map name, 256 bytes; 328 build number, 32 bytes (NUL-terminated)
	360 SHA-256 of the payload, 32 bytes
	392 the game state's address when saved (a fact, used only by an image
	    bound to its base)
	396 pointers outside the game state kept as they are
	400 memory pool blocks, 404 script threads, 408 pointers made places
	412 reserved (zero) to 508
	508 CRC of bytes 0-507
chunks, one after another: "HWGC", the chunk's index, its payload bytes
(the chunk bytes, the last one fewer), the CRC of its payload; the payload.
The payloads, one after another, are the payload: segments, each a control
word and its words. A control word with bit 31 set is a run of that many
zero words (bits 0-30, at least 1); otherwise it is followed by that many
literal words (1 to 1,024). A run of 3 or more zero words is written as a
run. The CRCs are the engine's (crc.c: CRC-32, polynomial 0xEDB88320,
starting at 0xFFFFFFFF, not inverted at the end).

The words are the game state's: its CPU part from its base to what is used,
then its GPU part from what is used to its end, each 32-bit word written
little-endian, with its pointers made places:
- a pointer into the game state is 0x40000000 plus its offset there, one
  into the tag cache 0x80000000 plus its offset there (the map's tags, which
  the header's map checksum names); none is 0;
- a data array's pointer to its elements (data.h), a memory pool's to its
  room and its first and last blocks, each block's to the next and previous
  blocks and to its reference (memory_pool.h), an lruv cache's to its blocks
  (lruv_cache.h), in each script thread in use (hs_runtime.c) the pointer
  to its top frame and each frame's to its previous frame and to its result,
  in each element in use of a data array the fields its unit declares
  (game_state_port_data_pointer_field: a recorded animation's place in its
  event stream, in the tag cache; a glow's particles), and the words its
  unit declares in a game state global (game_state_port_pointer_field: the
  HUD's messages, in the tag cache). The full list follows a census of every
  pointer member of the game state's types in the Wii engine's debug
  information (HWI-076's evidence);
- a memory pool block's reference (the element's field that points to the
  block, which the pool keeps up to date) is 0: loading makes it again from
  the block;
- an lruv cache's procedures are 0: loading puts this build's back.
A pointer of these that lies outside the game state (none measured) is
kept as it is, and the image is then bound to its base: it is taken only at
the address it was saved at.

The words are the port's own: a field narrower than 32 bits keeps its place
in its word of the port that saved it, so an image is taken only by a port of
the same byte order (header offset 16). Version 3 is to write each field by
itself (HWI-046).

---------- loading

A load reads the file twice. The first pass checks it all and takes
nothing: the header (its CRC, version, byte order, encoding, the layout
and allocation checksum this build makes, and the expected map, build,
players and map checksum), every chunk's CRC, the segments, the length, the
SHA-256, and every pointer, data array, memory pool, block and script
thread, as upstream's game_state_image_accept checked the image in memory.
Only then does the second pass read it again into the game state: each
chunk's CRC must be the one the first pass checked. Should the file read
differently (the card changed under it), the game state is partly the
file's and the load says so (game_state_image_failed_while_taking). At most
one chunk and its header are held at a time.
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "crc.h"
#include "data.h"
#include "memory_pool.h"
#include "lruv_cache.h"
#include "game_state.h"
#include "game_state_image.h"

/* ---------- constants */

#define IMAGE_MAGIC "HWGS"
#define CHUNK_MAGIC "HWGC"

enum
{
	IMAGE_FLAG_COMPLETE = 1,
	IMAGE_FLAG_BASE_BOUND = 2,

	IMAGE_LITTLE_ENDIAN = 1,
	IMAGE_BIG_ENDIAN = 2,

	IMAGE_ENCODING_WORDS = 1,

	LITERAL_MAXIMUM_WORDS = 1024,
	ZERO_RUN_MINIMUM_WORDS = 3,
	ZERO_RUN_BIT = 0x80000000,

	POINTER_TAG = 0x40000000,
	TAG_POINTER_TAG = 0x80000000,
	POINTER_TAG_MASK = 0xC0000000,
	POINTER_OFFSET_MASK = 0x3FFFFFFF,

	/* hs_runtime.c's script thread (hs_thread_datum) and stack frame
	(hs_stack_frame) */
	SCRIPT_THREAD_BYTES = 0x218,
	SCRIPT_THREAD_STACK = 0x10,
	SCRIPT_THREAD_STACK_DATA = 0x18,
	SCRIPT_THREAD_STACK_BYTES = 0x200,
	SCRIPT_FRAME_PREVIOUS = 0,
	SCRIPT_FRAME_RESULT = 8,
	SCRIPT_FRAME_SIZE = 12,
	SCRIPT_FRAME_HEADER_BYTES = 14,
	SCRIPT_THREAD_MAXIMUM_FRAMES = 64,

	MAXIMUM_POOL_BLOCKS = 0x10000,
};

/* (data.h's, memory_pool.h's and lruv_cache.h's, the engine's four-character
constants: 'd@t@', 'pool', 'head', 'tail', 'weee') */
#define DATA_ARRAY_SIGNATURE 0x64407440UL
#define MEMORY_POOL_SIGNATURE 0x706F6F6CUL
#define BLOCK_HEADER_SIGNATURE 0x68656164UL
#define BLOCK_TRAILER_SIGNATURE 0x7461696CUL
#define LRUV_CACHE_SIGNATURE 0x77656565UL

enum
{
	HEADER_MAGIC = 0,
	HEADER_VERSION = 4,
	HEADER_BYTES = 8,
	HEADER_FLAGS = 12,
	HEADER_BYTE_ORDER = 16,
	HEADER_ENCODING = 20,
	HEADER_CHUNK_BYTES = 24,
	HEADER_CHUNK_COUNT = 28,
	HEADER_PAYLOAD_BYTES = 32,
	HEADER_STATE_BYTES = 36,
	HEADER_CPU_USED = 40,
	HEADER_GPU_USED = 44,
	HEADER_STATE_SIZE = 48,
	HEADER_ALLOCATION_COUNT = 52,
	HEADER_LAYOUT_DIGEST = 56,
	HEADER_ALLOCATION_CHECKSUM = 60,
	HEADER_CACHE_FILE_CHECKSUM = 64,
	HEADER_PLAYER_COUNT = 68,
	HEADER_DIFFICULTY = 70,
	HEADER_MAP_NAME = 72,
	HEADER_BUILD_NUMBER = 328,
	HEADER_DIGEST = 360,
	HEADER_SAVE_BASE = 392,
	HEADER_FOREIGN_POINTERS = 396,
	HEADER_POOL_BLOCKS = 400,
	HEADER_SCRIPT_THREADS = 404,
	HEADER_POINTERS = 408,
	HEADER_CHECKSUM = 508,
};

/* ---------- structures */

typedef unsigned int image_word;
typedef char image_word_size_check[sizeof(image_word) == 4 ? 1 : -1];
typedef char image_pointer_size_check[sizeof(void *) == 4 ? 1 : -1];
typedef char image_header_size_check[GAME_STATE_IMAGE_HEADER_BYTES == HEADER_CHECKSUM + 4 ? 1 : -1];
typedef char image_data_array_size_check[sizeof(struct data_array) == 0x38 ? 1 : -1];
typedef char image_memory_pool_block_size_check[sizeof(struct memory_pool_block) == 24 ? 1 : -1];

struct sha256
{
	image_word state[8];
	image_word low_bits, high_bits;
	byte block[64];
	long used;
};

struct image_writer
{
	struct game_state_image_layout const *layout;
	struct game_state_image_file const *file;
	byte *chunk; /* a chunk's header and payload */
	long chunk_used;
	unsigned long chunk_count;
	unsigned long payload_bytes;
	image_word *literal;
	long literal_count;
	long zero_run;
	struct sha256 sha;
	/* a copy of a data array's element (one with declared pointer fields) */
	byte *element;
	/* the memory pools' blocks' references, ascending, and the next one */
	long *references;
	long reference_count;
	long next_reference;
	/* the next declared pointer word */
	long next_pointer_word;
	/* whether the words being written are room nothing reads: a memory
	pool's outside its blocks, a script thread's above its top frame */
	boolean dead;
	boolean failed;
	struct game_state_image_report *report;
};

struct image_reader
{
	struct game_state_image_layout const *layout;
	struct game_state_image_file const *file;
	boolean taking;
	byte *chunk;
	long chunk_bytes;        /* the header's */
	unsigned long chunk_count;
	unsigned long chunk_index; /* of the next chunk to read */
	long chunk_size;         /* the current chunk's payload */
	long chunk_position;
	unsigned long payload_left; /* not yet read into a chunk */
	unsigned long *chunk_checksums; /* the first pass's, which the second must read again */
	long segment_left;
	boolean segment_zero;
	unsigned long state_words_left;
	struct sha256 sha;
	byte digest[32];
	boolean base_bound;
	unsigned long save_base;
	short refusal;
	long refusal_offset;
	struct game_state_image_report *report;
	/* the first pass's header, which the second must read again, and
	whether the game state has begun to change */
	byte first_header[GAME_STATE_IMAGE_HEADER_BYTES];
	boolean writing;
	/* while reading a declared pointer field, which may point into the tag
	cache */
	boolean allow_tag_pointers;
	byte *element;
	/* the next declared pointer word */
	long next_pointer_word;
};

/* ---------- prototypes */

static image_word writer_declared_word(
	struct image_writer *writer,
	image_word word,
	short kind);
static boolean reader_pointer_field(
	struct image_reader *reader,
	byte *field,
	long *offset,
	long at);

/* ---------- globals */

static struct game_state_image_report image_report;
static void (*image_observer)(short event, struct game_state_image_report const *report);
static unsigned long image_heap_in_use;

/* ---------- private code: memory */

static void *image_allocate(
	unsigned long size)
{
	unsigned long *block = malloc(size + 8);

	if (!block)
		return NULL;
	block[0] = size;
	image_heap_in_use += size;
	if (image_heap_in_use > image_report.peak_bytes)
		image_report.peak_bytes = image_heap_in_use;

	return block + 2;
}

static void image_free(
	void *pointer)
{
	unsigned long *block = pointer;

	if (!block)
		return;
	block -= 2;
	image_heap_in_use -= block[0];
	free(block);
}

/* ---------- private code: words */

static boolean image_port_big_endian(
	void)
{
	image_word probe = 1;
	byte first;

	csmemcpy(&first, &probe, 1);

	return first == 0;
}

static image_word get_native_word(
	byte const *bytes)
{
	image_word word;

	csmemcpy(&word, bytes, sizeof(word));

	return word;
}

static void put_native_word(
	byte *bytes,
	image_word word)
{
	csmemcpy(bytes, &word, sizeof(word));
}

static void put_le32(
	byte *bytes,
	image_word value)
{
	bytes[0] = (byte)value;
	bytes[1] = (byte)(value >> 8);
	bytes[2] = (byte)(value >> 16);
	bytes[3] = (byte)(value >> 24);
}

static image_word get_le32(
	byte const *bytes)
{
	return (image_word)bytes[0] | (image_word)bytes[1] << 8 | (image_word)bytes[2] << 16 | (image_word)bytes[3] << 24;
}

static void put_le16(
	byte *bytes,
	short value)
{
	bytes[0] = (byte)value;
	bytes[1] = (byte)((unsigned short)value >> 8);
}

static short get_le16(
	byte const *bytes)
{
	return (short)(unsigned short)(bytes[0] | bytes[1] << 8);
}

static unsigned long image_crc(
	void const *buffer,
	long size)
{
	unsigned long crc;

	crc_new(&crc);
	crc_checksum_buffer(&crc, buffer, size);

	return crc & 0xFFFFFFFFUL;
}

/* ---------- private code: SHA-256 (FIPS 180-4) */

static image_word const sha256_constants[64] =
{
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTATE(value, bits) (((value) >> (bits)) | ((value) << (32 - (bits))))

static void sha256_begin(
	struct sha256 *sha)
{
	static image_word const initial[8] =
	{
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	csmemcpy(sha->state, initial, sizeof(initial));
	sha->low_bits = 0;
	sha->high_bits = 0;
	sha->used = 0;
}

static void sha256_block(
	struct sha256 *sha,
	byte const *block)
{
	image_word schedule[64];
	image_word a, b, c, d, e, f, g, h;
	long index;

	for (index = 0; index < 16; index++)
	{
		schedule[index] = (image_word)block[index * 4] << 24 | (image_word)block[index * 4 + 1] << 16 |
			(image_word)block[index * 4 + 2] << 8 | (image_word)block[index * 4 + 3];
	}
	for (index = 16; index < 64; index++)
	{
		image_word s0 = SHA256_ROTATE(schedule[index - 15], 7) ^ SHA256_ROTATE(schedule[index - 15], 18) ^
			(schedule[index - 15] >> 3);
		image_word s1 = SHA256_ROTATE(schedule[index - 2], 17) ^ SHA256_ROTATE(schedule[index - 2], 19) ^
			(schedule[index - 2] >> 10);

		schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
	}
	a = sha->state[0];
	b = sha->state[1];
	c = sha->state[2];
	d = sha->state[3];
	e = sha->state[4];
	f = sha->state[5];
	g = sha->state[6];
	h = sha->state[7];
	for (index = 0; index < 64; index++)
	{
		image_word s1 = SHA256_ROTATE(e, 6) ^ SHA256_ROTATE(e, 11) ^ SHA256_ROTATE(e, 25);
		image_word choice = (e & f) ^ (~e & g);
		image_word t1 = h + s1 + choice + sha256_constants[index] + schedule[index];
		image_word s0 = SHA256_ROTATE(a, 2) ^ SHA256_ROTATE(a, 13) ^ SHA256_ROTATE(a, 22);
		image_word majority = (a & b) ^ (a & c) ^ (b & c);
		image_word t2 = s0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	sha->state[0] += a;
	sha->state[1] += b;
	sha->state[2] += c;
	sha->state[3] += d;
	sha->state[4] += e;
	sha->state[5] += f;
	sha->state[6] += g;
	sha->state[7] += h;
}

static void sha256_add(
	struct sha256 *sha,
	byte const *bytes,
	long size)
{
	image_word bits = (image_word)size << 3;

	sha->low_bits += bits;
	if (sha->low_bits < bits)
		sha->high_bits++;
	sha->high_bits += (image_word)((unsigned long)size >> 29);
	while (size > 0)
	{
		long take = 64 - sha->used;

		if (take > size)
			take = size;
		csmemcpy(sha->block + sha->used, bytes, (unsigned long)take);
		sha->used += take;
		bytes += take;
		size -= take;
		if (sha->used == 64)
		{
			sha256_block(sha, sha->block);
			sha->used = 0;
		}
	}
}

static void sha256_end(
	struct sha256 *sha,
	byte digest[32])
{
	image_word low_bits = sha->low_bits;
	image_word high_bits = sha->high_bits;
	byte pad = 0x80;
	byte zero = 0;
	byte length[8];
	long index;

	sha256_add(sha, &pad, 1);
	while (sha->used != 56)
		sha256_add(sha, &zero, 1);
	for (index = 0; index < 4; index++)
	{
		length[index] = (byte)(high_bits >> (24 - index * 8));
		length[index + 4] = (byte)(low_bits >> (24 - index * 8));
	}
	sha256_add(sha, length, 8);
	for (index = 0; index < 8; index++)
	{
		digest[index * 4] = (byte)(sha->state[index] >> 24);
		digest[index * 4 + 1] = (byte)(sha->state[index] >> 16);
		digest[index * 4 + 2] = (byte)(sha->state[index] >> 8);
		digest[index * 4 + 3] = (byte)sha->state[index];
	}
}

static void image_report_digest(
	byte const digest[32])
{
	long index;

	for (index = 0; index < 8; index++)
	{
		image_report.digest[index] = (unsigned long)digest[index * 4] << 24 | (unsigned long)digest[index * 4 + 1] << 16 |
			(unsigned long)digest[index * 4 + 2] << 8 | (unsigned long)digest[index * 4 + 3];
	}
}

/* ---------- private code: the layout */

static unsigned long image_layout_digest(
	struct game_state_image_layout const *layout)
{
	unsigned long crc;
	byte bytes[40];
	long index;
	short field;

	crc_new(&crc);
	put_le32(bytes, (image_word)layout->size);
	put_le32(bytes + 4, (image_word)layout->cpu_used);
	put_le32(bytes + 8, (image_word)layout->gpu_used);
	put_le32(bytes + 12, (image_word)layout->allocation_count);
	crc_checksum_buffer(&crc, bytes, 16);
	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];

		put_le32(bytes, (image_word)allocation->kind);
		put_le32(bytes + 4, (image_word)allocation->encoding);
		put_le32(bytes + 8, (image_word)allocation->offset);
		put_le32(bytes + 12, (image_word)allocation->size);
		put_le32(bytes + 16, (image_word)allocation->maximum_count);
		put_le32(bytes + 20, (image_word)allocation->element_size);
		put_le32(bytes + 24, (image_word)allocation->pool_size);
		put_le32(bytes + 28, (image_word)allocation->page_count);
		put_le32(bytes + 32, (image_word)allocation->page_size_bits);
		put_le32(bytes + 36, (image_word)allocation->pointer_field_count);
		crc_checksum_buffer(&crc, bytes, 40);
		for (field = 0; field < allocation->pointer_field_count; field++)
		{
			put_le32(bytes, (image_word)allocation->pointer_fields[field]);
			crc_checksum_buffer(&crc, bytes, 4);
		}
	}

	return crc & 0xFFFFFFFFUL;
}

/* whether the layout is one this file can walk: allocations in ascending
order inside the CPU part, each of its kind's size */
static boolean image_layout_valid(
	struct game_state_image_layout const *layout)
{
	long end = 0;
	long index;
	long other;

	if (!layout || !layout->base || ((unsigned long)layout->base & 3) || layout->size <= 0 || (layout->size & 3) ||
		layout->size > POINTER_OFFSET_MASK || layout->cpu_used < 0 || layout->gpu_used < 0 ||
		(layout->cpu_used & 3) || (layout->gpu_used & 3) || layout->cpu_used > layout->size - layout->gpu_used ||
		layout->allocation_count < 0 || (layout->allocation_count && !layout->allocations) ||
		layout->tags_size < 0 || layout->tags_size > POINTER_OFFSET_MASK || (layout->tags_size && !layout->tags) ||
		layout->pointer_word_count < 0 || (layout->pointer_word_count && !layout->pointer_words))
	{
		return FALSE;
	}
	for (index = 0; index < layout->pointer_word_count; index++)
	{
		struct game_state_image_pointer_word const *declared = &layout->pointer_words[index];

		if ((declared->offset & 3) || declared->offset < 0 || declared->offset > layout->cpu_used - 4 ||
			(index && declared->offset <= layout->pointer_words[index - 1].offset) ||
			(declared->kind != _game_state_image_pointer && declared->kind != _game_state_image_pointer_or_value))
		{
			return FALSE;
		}
		for (other = 0; other < layout->allocation_count; other++)
		{
			if (declared->offset >= layout->allocations[other].offset &&
				declared->offset < layout->allocations[other].offset + layout->allocations[other].size)
			{
				return FALSE;
			}
		}
	}
	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];
		long expected;
		short field;

		switch (allocation->kind)
		{
		case _game_state_image_data:
			expected = data_allocation_size(allocation->maximum_count, allocation->element_size);
			break;
		case _game_state_image_memory_pool:
			expected = memory_pool_allocation_size(allocation->pool_size);
			break;
		case _game_state_image_lruv_cache:
			expected = lruv_allocation_size(allocation->maximum_count);
			break;
		default:
			return FALSE;
		}
		if (allocation->pointer_field_count < 0 || allocation->pointer_field_count > GAME_STATE_IMAGE_MAXIMUM_POINTER_FIELDS ||
			(allocation->pointer_field_count && (allocation->kind != _game_state_image_data ||
				allocation->encoding != _game_state_image_encoding_plain)))
		{
			return FALSE;
		}
		for (field = 0; field < allocation->pointer_field_count; field++)
		{
			if (allocation->pointer_fields[field] < (short)sizeof(short) ||
				allocation->pointer_fields[field] > allocation->element_size - (short)sizeof(void *))
			{
				return FALSE;
			}
		}
		if (allocation->offset < end || (allocation->offset & 3) || allocation->size != expected ||
			allocation->size > layout->cpu_used - allocation->offset || allocation->maximum_count < 0 ||
			allocation->element_size < 0 ||
			(allocation->encoding == _game_state_image_encoding_script_threads &&
				(allocation->kind != _game_state_image_data || allocation->element_size != SCRIPT_THREAD_BYTES)))
		{
			return FALSE;
		}
		end = allocation->offset + allocation->size;
	}

	return TRUE;
}

/* the allocation whose data array elements (or lruv cache's blocks)
[offset, offset+size) lies in */
static boolean image_in_data_array_elements(
	struct game_state_image_layout const *layout,
	long offset,
	long size)
{
	long index;

	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];
		long first;
		long last;

		if (allocation->kind == _game_state_image_data)
			first = allocation->offset + (long)sizeof(struct data_array);
		else if (allocation->kind == _game_state_image_lruv_cache)
			first = allocation->offset + (long)sizeof(struct lruv_cache) + (long)sizeof(struct data_array);
		else
			continue;
		last = first + (long)allocation->maximum_count * allocation->element_size;
		if (allocation->kind == _game_state_image_lruv_cache)
			last = first + (long)allocation->maximum_count * (long)sizeof(struct lruv_cache_block);
		if (offset >= first && offset <= last - size)
			return TRUE;
	}

	return FALSE;
}

/* the largest element of a data array with declared pointer fields (at
least 4 bytes) */
static unsigned long image_largest_pointer_element(
	struct game_state_image_layout const *layout)
{
	unsigned long largest = 4;
	long index;

	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];

		if (allocation->pointer_field_count && (unsigned long)allocation->element_size > largest)
			largest = (unsigned long)allocation->element_size;
	}

	return largest;
}

/* ---------- private code: the header */

static void image_header_encode(
	byte *bytes,
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	struct game_state_image_report const *report,
	byte const digest[32],
	boolean complete)
{
	csmemset(bytes, 0, GAME_STATE_IMAGE_HEADER_BYTES);
	csmemcpy(bytes + HEADER_MAGIC, IMAGE_MAGIC, 4);
	put_le32(bytes + HEADER_VERSION, GAME_STATE_IMAGE_VERSION);
	put_le32(bytes + HEADER_BYTES, GAME_STATE_IMAGE_HEADER_BYTES);
	put_le32(bytes + HEADER_FLAGS, (complete ? IMAGE_FLAG_COMPLETE : 0) | (report->base_bound ? IMAGE_FLAG_BASE_BOUND : 0));
	put_le32(bytes + HEADER_BYTE_ORDER, image_port_big_endian() ? IMAGE_BIG_ENDIAN : IMAGE_LITTLE_ENDIAN);
	put_le32(bytes + HEADER_ENCODING, IMAGE_ENCODING_WORDS);
	put_le32(bytes + HEADER_CHUNK_BYTES, GAME_STATE_IMAGE_CHUNK_BYTES);
	put_le32(bytes + HEADER_CHUNK_COUNT, (image_word)report->chunks);
	put_le32(bytes + HEADER_PAYLOAD_BYTES, (image_word)report->payload_bytes);
	put_le32(bytes + HEADER_STATE_BYTES, (image_word)(layout->cpu_used + layout->gpu_used));
	put_le32(bytes + HEADER_CPU_USED, (image_word)layout->cpu_used);
	put_le32(bytes + HEADER_GPU_USED, (image_word)layout->gpu_used);
	put_le32(bytes + HEADER_STATE_SIZE, (image_word)layout->size);
	put_le32(bytes + HEADER_ALLOCATION_COUNT, (image_word)layout->allocation_count);
	put_le32(bytes + HEADER_LAYOUT_DIGEST, (image_word)image_layout_digest(layout));
	put_le32(bytes + HEADER_ALLOCATION_CHECKSUM, (image_word)header->allocation_size_checksum);
	put_le32(bytes + HEADER_CACHE_FILE_CHECKSUM, (image_word)header->cache_file_checksum);
	put_le16(bytes + HEADER_PLAYER_COUNT, header->player_count);
	put_le16(bytes + HEADER_DIFFICULTY, header->difficulty);
	csmemcpy(bytes + HEADER_MAP_NAME, header->map_name, sizeof(header->map_name));
	bytes[HEADER_MAP_NAME + sizeof(header->map_name) - 1] = 0;
	csmemcpy(bytes + HEADER_BUILD_NUMBER, header->build_number, sizeof(header->build_number));
	bytes[HEADER_BUILD_NUMBER + sizeof(header->build_number) - 1] = 0;
	if (digest)
		csmemcpy(bytes + HEADER_DIGEST, digest, 32);
	put_le32(bytes + HEADER_SAVE_BASE, (image_word)(unsigned long)layout->base);
	put_le32(bytes + HEADER_FOREIGN_POINTERS, (image_word)report->foreign_pointers);
	put_le32(bytes + HEADER_POOL_BLOCKS, (image_word)report->references);
	put_le32(bytes + HEADER_SCRIPT_THREADS, (image_word)report->script_threads);
	put_le32(bytes + HEADER_POINTERS, (image_word)report->pointers);
	put_le32(bytes + HEADER_CHECKSUM, (image_word)image_crc(bytes, HEADER_CHECKSUM));
}

/* the header's own checks: its CRC, magic, version, completeness */
static short image_header_check(
	byte const *bytes)
{
	if (csmemcmp(bytes + HEADER_MAGIC, IMAGE_MAGIC, 4))
		return _game_state_image_refusal_magic;
	if (get_le32(bytes + HEADER_VERSION) != GAME_STATE_IMAGE_VERSION ||
		get_le32(bytes + HEADER_BYTES) != GAME_STATE_IMAGE_HEADER_BYTES)
	{
		return _game_state_image_refusal_version;
	}
	if (get_le32(bytes + HEADER_CHECKSUM) != image_crc(bytes, HEADER_CHECKSUM))
		return _game_state_image_refusal_header_checksum;
	if (!(get_le32(bytes + HEADER_FLAGS) & IMAGE_FLAG_COMPLETE))
		return _game_state_image_refusal_incomplete;

	return _game_state_image_refusal_none;
}

static void image_header_decode(
	byte const *bytes,
	struct game_state_header *header)
{
	csmemset(header, 0, sizeof(*header));
	header->allocation_size_checksum = get_le32(bytes + HEADER_ALLOCATION_CHECKSUM);
	header->cache_file_checksum = get_le32(bytes + HEADER_CACHE_FILE_CHECKSUM);
	header->player_count = get_le16(bytes + HEADER_PLAYER_COUNT);
	header->difficulty = get_le16(bytes + HEADER_DIFFICULTY);
	csmemcpy(header->map_name, bytes + HEADER_MAP_NAME, sizeof(header->map_name));
	header->map_name[sizeof(header->map_name) - 1] = 0;
	csmemcpy(header->build_number, bytes + HEADER_BUILD_NUMBER, sizeof(header->build_number));
	header->build_number[sizeof(header->build_number) - 1] = 0;
}

/* ---------- private code: writing */

static void writer_payload(
	struct image_writer *writer,
	byte const *bytes,
	long size)
{
	sha256_add(&writer->sha, bytes, size);
	writer->payload_bytes += (unsigned long)size;
	if (!writer->file)
		return;
	while (size > 0 && !writer->failed)
	{
		long take = GAME_STATE_IMAGE_CHUNK_BYTES - writer->chunk_used;

		if (take > size)
			take = size;
		csmemcpy(writer->chunk + GAME_STATE_IMAGE_CHUNK_HEADER_BYTES + writer->chunk_used, bytes, (unsigned long)take);
		writer->chunk_used += take;
		bytes += take;
		size -= take;
		if (writer->chunk_used == GAME_STATE_IMAGE_CHUNK_BYTES)
		{
			byte *header = writer->chunk;

			csmemcpy(header, CHUNK_MAGIC, 4);
			put_le32(header + 4, (image_word)writer->chunk_count);
			put_le32(header + 8, (image_word)writer->chunk_used);
			put_le32(header + 12, (image_word)image_crc(header + GAME_STATE_IMAGE_CHUNK_HEADER_BYTES, writer->chunk_used));
			if (!writer->file->write(writer->file->context, writer->chunk,
				GAME_STATE_IMAGE_CHUNK_HEADER_BYTES + writer->chunk_used))
			{
				writer->failed = TRUE;
			}
			writer->report->file_bytes += (unsigned long)(GAME_STATE_IMAGE_CHUNK_HEADER_BYTES + writer->chunk_used);
			writer->chunk_count++;
			writer->chunk_used = 0;
		}
	}
}

static void writer_flush_chunk(
	struct image_writer *writer)
{
	if (writer->file && writer->chunk_used && !writer->failed)
	{
		byte *header = writer->chunk;

		csmemcpy(header, CHUNK_MAGIC, 4);
		put_le32(header + 4, (image_word)writer->chunk_count);
		put_le32(header + 8, (image_word)writer->chunk_used);
		put_le32(header + 12, (image_word)image_crc(header + GAME_STATE_IMAGE_CHUNK_HEADER_BYTES, writer->chunk_used));
		if (!writer->file->write(writer->file->context, writer->chunk,
			GAME_STATE_IMAGE_CHUNK_HEADER_BYTES + writer->chunk_used))
		{
			writer->failed = TRUE;
		}
		writer->report->file_bytes += (unsigned long)(GAME_STATE_IMAGE_CHUNK_HEADER_BYTES + writer->chunk_used);
		writer->chunk_count++;
		writer->chunk_used = 0;
	}
	else if (!writer->file && writer->payload_bytes)
	{
		/* (measured only) */
		writer->chunk_count = (writer->payload_bytes + GAME_STATE_IMAGE_CHUNK_BYTES - 1) / GAME_STATE_IMAGE_CHUNK_BYTES;
	}
}

static void writer_control(
	struct image_writer *writer,
	image_word control)
{
	byte bytes[4];

	put_le32(bytes, control);
	writer_payload(writer, bytes, 4);
}

static void writer_flush_literals(
	struct image_writer *writer)
{
	byte bytes[4 * 64];
	long index;

	if (!writer->literal_count)
		return;
	writer_control(writer, (image_word)writer->literal_count);
	for (index = 0; index < writer->literal_count; index++)
	{
		put_le32(bytes + (index % 64) * 4, writer->literal[index]);
		if (index % 64 == 63 || index == writer->literal_count - 1)
			writer_payload(writer, bytes, (index % 64 + 1) * 4);
	}
	writer->literal_count = 0;
}

static void writer_literal(
	struct image_writer *writer,
	image_word word)
{
	writer->literal[writer->literal_count++] = word;
	if (writer->literal_count == LITERAL_MAXIMUM_WORDS)
		writer_flush_literals(writer);
}

static void writer_flush_zeros(
	struct image_writer *writer)
{
	if (writer->zero_run >= ZERO_RUN_MINIMUM_WORDS)
	{
		writer_flush_literals(writer);
		writer_control(writer, ZERO_RUN_BIT | (image_word)writer->zero_run);
	}
	else
	{
		while (writer->zero_run > 0)
		{
			writer_literal(writer, 0);
			writer->zero_run--;
		}
	}
	writer->zero_run = 0;
}

static void writer_word(
	struct image_writer *writer,
	image_word word)
{
	if (!word)
	{
		if (++writer->zero_run == (long)~(image_word)ZERO_RUN_BIT)
			writer_flush_zeros(writer);
		return;
	}
	if (writer->zero_run)
		writer_flush_zeros(writer);
	writer_literal(writer, word);
}

static void writer_end(
	struct image_writer *writer)
{
	if (writer->zero_run)
		writer_flush_zeros(writer);
	writer_flush_literals(writer);
	writer_flush_chunk(writer);
}

/* the game state's [offset, offset+size) from bytes (the state there, or a
copy with its pointers made places): each word written, a block reference
written as 0 */
static void writer_words(
	struct image_writer *writer,
	byte const *bytes,
	long offset,
	long size)
{
	long at;
	unsigned long low = (unsigned long)writer->layout->base;

	for (at = 0; at < size; at += 4)
	{
		image_word word = get_native_word(bytes + at);

		while (writer->next_reference < writer->reference_count &&
			writer->references[writer->next_reference] < offset + at)
		{
			writer->next_reference++;
		}
		while (writer->next_pointer_word < writer->layout->pointer_word_count &&
			writer->layout->pointer_words[writer->next_pointer_word].offset < offset + at)
		{
			writer->next_pointer_word++;
		}
		if (writer->next_reference < writer->reference_count &&
			writer->references[writer->next_reference] == offset + at)
		{
			word = 0;
			writer->next_reference++;
		}
		else if (writer->next_pointer_word < writer->layout->pointer_word_count &&
			writer->layout->pointer_words[writer->next_pointer_word].offset == offset + at)
		{
			word = writer_declared_word(writer, word,
				writer->layout->pointer_words[writer->next_pointer_word].kind);
			writer->next_pointer_word++;
		}
		else if (word && (unsigned long)word - low < (unsigned long)writer->layout->size)
		{
			/* a word left as it is that reads as an address in the game
			state: in room nothing reads (a moved block's old header, a
			popped frame), or in what is read (a value that happens to be
			one, or a pointer this file does not know: none measured) */
			if (writer->dead)
				writer->report->address_like_dead_words++;
			else if (!writer->report->address_like_words++)
				writer->report->first_address_like_offset = offset + at;
		}
		writer_word(writer, word);
	}
}

static image_word writer_pointer(
	struct image_writer *writer,
	void const *pointer)
{
	unsigned long value = (unsigned long)pointer;
	unsigned long offset = value - (unsigned long)writer->layout->base;

	if (!pointer)
		return 0;
	if (offset < (unsigned long)writer->layout->size)
	{
		writer->report->pointers++;
		return (image_word)(POINTER_TAG | offset);
	}
	offset = value - (unsigned long)writer->layout->tags;
	if (writer->layout->tags && offset < (unsigned long)writer->layout->tags_size)
	{
		writer->report->pointers++;
		writer->report->tag_pointers++;
		return (image_word)(TAG_POINTER_TAG | offset);
	}
	/* outside the game state: kept, and the image bound to this base. One
	that reads as a place in it could not be told from one */
	writer->report->foreign_pointers++;
	writer->report->base_bound = TRUE;
	if (((value & POINTER_TAG_MASK) == POINTER_TAG && (value & POINTER_OFFSET_MASK) < (unsigned long)writer->layout->size) ||
		((value & POINTER_TAG_MASK) == TAG_POINTER_TAG && (value & POINTER_OFFSET_MASK) < (unsigned long)writer->layout->tags_size))
	{
		writer->failed = TRUE;
	}

	return (image_word)value;
}

/* a declared pointer word of a global: a pointer made its place; a union's
value that is no pointer kept (one that would read as a place could not be
told from one) */
static image_word writer_declared_word(
	struct image_writer *writer,
	image_word word,
	short kind)
{
	unsigned long value = word;

	if (kind == _game_state_image_pointer_or_value &&
		value - (unsigned long)writer->layout->base >= (unsigned long)writer->layout->size &&
		(!writer->layout->tags || value - (unsigned long)writer->layout->tags >= (unsigned long)writer->layout->tags_size))
	{
		if ((value & POINTER_TAG_MASK) == POINTER_TAG || (value & POINTER_TAG_MASK) == TAG_POINTER_TAG)
			writer->failed = TRUE;
		return word;
	}

	return writer_pointer(writer, (void const *)value);
}

/* a pointer field (at any byte) of a copy, made its place */
static void writer_pointer_field(
	struct image_writer *writer,
	byte *field)
{
	void *pointer;
	image_word word;

	csmemcpy(&pointer, field, sizeof(pointer));
	word = writer_pointer(writer, pointer);
	csmemcpy(field, &word, sizeof(word));
}

static void writer_data_array(
	struct image_writer *writer,
	struct game_state_image_allocation const *allocation,
	long offset,
	short encoding)
{
	struct game_state_image_layout const *layout = writer->layout;
	struct data_array const *live = (struct data_array const *)(layout->base + offset);
	struct data_array header = *live;
	long elements = offset + (long)sizeof(struct data_array);
	long index;

	writer_pointer_field(writer, (byte *)&header.data);
	writer_words(writer, (byte const *)&header, offset, sizeof(header));
	if (allocation && allocation->pointer_field_count)
	{
		/* each element in use with its declared pointers made places */
		for (index = 0; index < live->maximum_count; index++)
		{
			long element_offset = elements + index * live->size;
			byte const *element = layout->base + element_offset;
			short identifier;
			short field;

			csmemcpy(&identifier, element, sizeof(identifier));
			if (index >= live->count || !identifier)
			{
				writer->dead = TRUE;
				writer_words(writer, element, element_offset, live->size);
				writer->dead = FALSE;
				continue;
			}
			csmemcpy(writer->element, element, (unsigned long)live->size);
			for (field = 0; field < allocation->pointer_field_count; field++)
				writer_pointer_field(writer, writer->element + allocation->pointer_fields[field]);
			writer_words(writer, writer->element, element_offset, live->size);
		}
		return;
	}
	if (encoding != _game_state_image_encoding_script_threads && (live->size & 3))
	{
		writer_words(writer, layout->base + elements, elements, (long)live->maximum_count * live->size);
		return;
	}
	if (encoding != _game_state_image_encoding_script_threads)
	{
		/* (an element not in use is room nothing reads: datum_new clears it
		before it is used again) */
		for (index = 0; index < live->maximum_count; index++)
		{
			long element_offset = elements + index * live->size;
			short identifier;

			csmemcpy(&identifier, layout->base + element_offset, sizeof(identifier));
			writer->dead = index >= live->count || !identifier;
			writer_words(writer, layout->base + element_offset, element_offset, live->size);
		}
		writer->dead = FALSE;
		return;
	}
	for (index = 0; index < live->maximum_count; index++)
	{
		byte thread[SCRIPT_THREAD_BYTES];
		long thread_offset = elements + index * SCRIPT_THREAD_BYTES;
		byte const *thread_live = layout->base + thread_offset;
		short identifier;

		long live_bytes = 0;

		csmemcpy(thread, thread_live, SCRIPT_THREAD_BYTES);
		csmemcpy(&identifier, thread, sizeof(identifier));
		if (index < live->count && identifier)
		{
			byte const *frame;
			long frames = 0;

			live_bytes = SCRIPT_THREAD_BYTES;

			writer->report->script_threads++;
			csmemcpy(&frame, thread_live + SCRIPT_THREAD_STACK, sizeof(frame));
			writer_pointer_field(writer, thread + SCRIPT_THREAD_STACK);
			while (frame && frames++ < SCRIPT_THREAD_MAXIMUM_FRAMES)
			{
				unsigned long at = (unsigned long)(frame - (thread_live + SCRIPT_THREAD_STACK_DATA));
				short size;

				if (at > SCRIPT_THREAD_STACK_BYTES - SCRIPT_FRAME_HEADER_BYTES)
					break;
				csmemcpy(&size, frame + SCRIPT_FRAME_SIZE, sizeof(size));
				if (size < 0 || (unsigned long)size > SCRIPT_THREAD_STACK_BYTES - SCRIPT_FRAME_HEADER_BYTES - at)
					break;
				/* (the top frame's values end what is read) */
				if (frames == 1)
					live_bytes = (SCRIPT_THREAD_STACK_DATA + (long)at + SCRIPT_FRAME_HEADER_BYTES + size + 3) & ~3L;
				writer_pointer_field(writer, thread + SCRIPT_THREAD_STACK_DATA + at + SCRIPT_FRAME_PREVIOUS);
				writer_pointer_field(writer, thread + SCRIPT_THREAD_STACK_DATA + at + SCRIPT_FRAME_RESULT);
				csmemcpy(&frame, frame + SCRIPT_FRAME_PREVIOUS, sizeof(frame));
			}
		}
		writer_words(writer, thread, thread_offset, live_bytes);
		writer->dead = TRUE;
		writer_words(writer, thread + live_bytes, thread_offset + live_bytes, SCRIPT_THREAD_BYTES - live_bytes);
		writer->dead = FALSE;
	}
}

static void writer_memory_pool(
	struct image_writer *writer,
	struct game_state_image_allocation const *allocation)
{
	struct game_state_image_layout const *layout = writer->layout;
	struct memory_pool const *live = (struct memory_pool const *)(layout->base + allocation->offset);
	struct memory_pool header = *live;
	long room = allocation->offset + (long)sizeof(struct memory_pool);
	long position = room;
	long end = room + allocation->pool_size;
	struct memory_pool_block const *block = live->first_block;
	long blocks = 0;

	writer_pointer_field(writer, (byte *)&header.base_address);
	writer_pointer_field(writer, (byte *)&header.first_block);
	writer_pointer_field(writer, (byte *)&header.last_block);
	writer_words(writer, (byte const *)&header, allocation->offset, sizeof(header));
	while (block && blocks++ < MAXIMUM_POOL_BLOCKS)
	{
		long at = (long)((byte const *)block - layout->base);
		struct memory_pool_block copy;

		if (at < position || at > end - (long)sizeof(copy) || (at & 3) || block->size < (long)sizeof(copy) ||
			block->size > end - at)
		{
			break;
		}
		writer->dead = TRUE;
		writer_words(writer, layout->base + position, position, at - position);
		writer->dead = FALSE;
		copy = *block;
		writer_pointer_field(writer, (byte *)&copy.reference);
		writer_pointer_field(writer, (byte *)&copy.next_block);
		writer_pointer_field(writer, (byte *)&copy.previous_block);
		writer_words(writer, (byte const *)&copy, at, sizeof(copy));
		writer_words(writer, layout->base + at + sizeof(copy), at + (long)sizeof(copy), block->size - (long)sizeof(copy));
		position = at + block->size;
		block = block->next_block;
	}
	writer->dead = TRUE;
	writer_words(writer, layout->base + position, position, end - position);
	writer->dead = FALSE;
}

static void writer_lruv_cache(
	struct image_writer *writer,
	struct game_state_image_allocation const *allocation)
{
	struct game_state_image_layout const *layout = writer->layout;
	struct lruv_cache header = *(struct lruv_cache const *)(layout->base + allocation->offset);

	header.delete_block_proc = NULL;
	header.locked_block_proc = NULL;
	writer_pointer_field(writer, (byte *)&header.blocks);
	writer_words(writer, (byte const *)&header, allocation->offset, sizeof(header));
	writer_data_array(writer, NULL, allocation->offset + (long)sizeof(struct lruv_cache), _game_state_image_encoding_plain);
}

static int image_compare_offsets(
	void const *a,
	void const *b)
{
	long left = *(long const *)a;
	long right = *(long const *)b;

	return left < right ? -1 : left > right;
}

/* every memory pool block's reference (its offset), ascending */
static boolean writer_collect_references(
	struct image_writer *writer)
{
	struct game_state_image_layout const *layout = writer->layout;
	long count = 0;
	long pass;

	for (pass = 0; pass < 2; pass++)
	{
		long index;

		if (pass == 1)
		{
			writer->references = image_allocate((unsigned long)(count ? count : 1) * sizeof(long));
			if (!writer->references)
				return FALSE;
			count = 0;
		}
		for (index = 0; index < layout->allocation_count; index++)
		{
			struct game_state_image_allocation const *allocation = &layout->allocations[index];
			struct memory_pool const *pool = (struct memory_pool const *)(layout->base + allocation->offset);
			struct memory_pool_block const *block;
			long blocks = 0;

			if (allocation->kind != _game_state_image_memory_pool)
				continue;
			for (block = pool->first_block; block && blocks++ < MAXIMUM_POOL_BLOCKS; block = block->next_block)
			{
				unsigned long reference = (unsigned long)block->reference - (unsigned long)layout->base;

				if ((unsigned long)((byte const *)block - layout->base) >= (unsigned long)layout->size)
					break;
				if (reference < (unsigned long)layout->size)
				{
					if (pass == 1)
						writer->references[count] = (long)reference;
					count++;
				}
			}
		}
	}
	qsort(writer->references, (size_t)count, sizeof(long), image_compare_offsets);
	writer->reference_count = count;
	writer->report->references = (unsigned long)count;

	return TRUE;
}

static boolean image_write_or_measure(
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	struct game_state_image_file const *file,
	byte digest[32])
{
	struct image_writer writer;
	byte header_bytes[GAME_STATE_IMAGE_HEADER_BYTES];
	long offset = 0;
	long index;
	boolean success;

	csmemset(&image_report, 0, sizeof(image_report));
	image_report.save = TRUE;
	image_report.first_address_like_offset = NONE;
	image_report.save_base = (unsigned long)(layout ? layout->base : NULL);
	image_report.passes = 1;
	csmemset(&writer, 0, sizeof(writer));
	writer.layout = layout;
	writer.file = file;
	writer.report = &image_report;
	sha256_begin(&writer.sha);
	if (!image_layout_valid(layout) || !header)
	{
		image_report.refusal = _game_state_image_refusal_layout;
		return FALSE;
	}
	writer.literal = image_allocate(LITERAL_MAXIMUM_WORDS * sizeof(image_word));
	writer.element = image_allocate(image_largest_pointer_element(layout));
	writer.chunk = file ? image_allocate(GAME_STATE_IMAGE_CHUNK_HEADER_BYTES + GAME_STATE_IMAGE_CHUNK_BYTES) : NULL;
	if (!writer.literal || !writer.element || (file && !writer.chunk) || !writer_collect_references(&writer))
	{
		image_report.refusal = _game_state_image_refusal_memory;
		image_free(writer.element);
		image_free(writer.references);
		image_free(writer.chunk);
		image_free(writer.literal);
		return FALSE;
	}
	if (file)
	{
		/* the header first, not yet complete: a save cut short is refused */
		image_header_encode(header_bytes, layout, header, &image_report, NULL, FALSE);
		if (!file->seek(file->context, 0) || !file->write(file->context, header_bytes, GAME_STATE_IMAGE_HEADER_BYTES))
			writer.failed = TRUE;
		image_report.file_bytes = GAME_STATE_IMAGE_HEADER_BYTES;
	}
	for (index = 0; index < layout->allocation_count && !writer.failed; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];

		writer_words(&writer, layout->base + offset, offset, allocation->offset - offset);
		switch (allocation->kind)
		{
		case _game_state_image_data:
			writer_data_array(&writer, allocation, allocation->offset, allocation->encoding);
			break;
		case _game_state_image_memory_pool:
			writer_memory_pool(&writer, allocation);
			break;
		case _game_state_image_lruv_cache:
			writer_lruv_cache(&writer, allocation);
			break;
		}
		offset = allocation->offset + allocation->size;
	}
	writer_words(&writer, layout->base + offset, offset, layout->cpu_used - offset);
	writer_words(&writer, layout->base + layout->size - layout->gpu_used, layout->size - layout->gpu_used, layout->gpu_used);
	writer_end(&writer);
	sha256_end(&writer.sha, digest);
	image_report.state_bytes = (unsigned long)(layout->cpu_used + layout->gpu_used);
	image_report.payload_bytes = writer.payload_bytes;
	image_report.chunks = writer.chunk_count;
	image_report.chunk_bytes = GAME_STATE_IMAGE_CHUNK_BYTES;
	image_report_digest(digest);
	if (file && !writer.failed)
	{
		/* the header again, complete */
		image_header_encode(header_bytes, layout, header, &image_report, digest, TRUE);
		if (!file->seek(file->context, 0) || !file->write(file->context, header_bytes, GAME_STATE_IMAGE_HEADER_BYTES))
			writer.failed = TRUE;
	}
	success = !writer.failed;
	if (!success)
		image_report.refusal = _game_state_image_refusal_read;
	image_free(writer.element);
	image_free(writer.references);
	image_free(writer.chunk);
	image_free(writer.literal);

	return success;
}

/* ---------- private code: reading */

static boolean reader_refuse(
	struct image_reader *reader,
	short refusal,
	long offset)
{
	if (!reader->refusal)
	{
		reader->refusal = refusal;
		reader->refusal_offset = offset;
	}

	return FALSE;
}

/* the next chunk: its header's magic, index and size, its CRC (and in the
second pass, the first pass's CRC of it) */
static boolean reader_next_chunk(
	struct image_reader *reader)
{
	byte header[GAME_STATE_IMAGE_CHUNK_HEADER_BYTES];
	long size;
	unsigned long checksum;

	if (reader->chunk_index >= reader->chunk_count || !reader->payload_left)
		return reader_refuse(reader, _game_state_image_refusal_length, (long)reader->chunk_index);
	size = reader->payload_left < (unsigned long)reader->chunk_bytes ? (long)reader->payload_left : reader->chunk_bytes;
	if (!reader->file->read(reader->file->context, header, GAME_STATE_IMAGE_CHUNK_HEADER_BYTES) ||
		!reader->file->read(reader->file->context, reader->chunk, size))
	{
		return reader_refuse(reader, _game_state_image_refusal_read, (long)reader->chunk_index);
	}
	if (csmemcmp(header, CHUNK_MAGIC, 4) || get_le32(header + 4) != reader->chunk_index ||
		get_le32(header + 8) != (image_word)size)
	{
		return reader_refuse(reader, _game_state_image_refusal_chunk, (long)reader->chunk_index);
	}
	checksum = image_crc(reader->chunk, size);
	if (get_le32(header + 12) != checksum)
		return reader_refuse(reader, _game_state_image_refusal_chunk_checksum, (long)reader->chunk_index);
	if (reader->taking)
	{
		if (reader->chunk_checksums[reader->chunk_index] != checksum)
			return reader_refuse(reader, _game_state_image_refusal_chunk_checksum, (long)reader->chunk_index);
	}
	else
	{
		reader->chunk_checksums[reader->chunk_index] = checksum;
	}
	sha256_add(&reader->sha, reader->chunk, size);
	reader->chunk_index++;
	reader->chunk_size = size;
	reader->chunk_position = 0;
	reader->payload_left -= (unsigned long)size;

	return TRUE;
}

static boolean reader_payload(
	struct image_reader *reader,
	byte *bytes,
	long size)
{
	while (size > 0)
	{
		long take;

		if (reader->chunk_position == reader->chunk_size && !reader_next_chunk(reader))
			return FALSE;
		take = reader->chunk_size - reader->chunk_position;
		if (take > size)
			take = size;
		csmemcpy(bytes, reader->chunk + reader->chunk_position, (unsigned long)take);
		reader->chunk_position += take;
		bytes += take;
		size -= take;
	}

	return TRUE;
}

static boolean reader_segment(
	struct image_reader *reader,
	long offset)
{
	byte bytes[4];
	image_word control;

	if (!reader_payload(reader, bytes, 4))
		return FALSE;
	control = get_le32(bytes);
	reader->segment_zero = (control & ZERO_RUN_BIT) != 0;
	reader->segment_left = (long)(control & ~(image_word)ZERO_RUN_BIT);
	if (reader->segment_left < 1 || (!reader->segment_zero && reader->segment_left > LITERAL_MAXIMUM_WORDS) ||
		(unsigned long)reader->segment_left > reader->state_words_left)
	{
		return reader_refuse(reader, _game_state_image_refusal_segment, offset);
	}

	return TRUE;
}

/* a declared pointer word of a global (in this port's order): its place
made a pointer; a union's value that is no place kept */
static boolean reader_declared_word(
	struct image_reader *reader,
	byte *field,
	short kind,
	long at)
{
	image_word word;
	long place;
	boolean valid;

	csmemcpy(&word, field, sizeof(word));
	if (kind == _game_state_image_pointer_or_value && (word & POINTER_TAG_MASK) != POINTER_TAG &&
		(word & POINTER_TAG_MASK) != TAG_POINTER_TAG)
	{
		return TRUE;
	}
	reader->allow_tag_pointers = TRUE;
	valid = reader_pointer_field(reader, field, &place, at);
	reader->allow_tag_pointers = FALSE;

	return valid;
}

/* the game state's next size bytes (whole words) at offset: into bytes,
each word in this port's order, or (bytes NULL) only checked */
static boolean reader_words(
	struct image_reader *reader,
	byte *bytes,
	long offset,
	long size)
{
	long done = 0;

	while (done < size)
	{
		long words;

		if (!reader->segment_left && !reader_segment(reader, offset + done))
			return FALSE;
		words = (size - done) / 4;
		if (words > reader->segment_left)
			words = reader->segment_left;
		if (reader->segment_zero)
		{
			if (bytes)
				csmemset(bytes + done, 0, (unsigned long)words * 4);
			/* (a declared pointer word among them is none) */
			while (reader->next_pointer_word < reader->layout->pointer_word_count &&
				reader->layout->pointer_words[reader->next_pointer_word].offset < offset + done + words * 4)
			{
				reader->next_pointer_word++;
			}
		}
		else
		{
			long index;

			for (index = 0; index < words; index++)
			{
				byte word[4];
				byte field[4];

				if (!reader_payload(reader, word, 4))
					return FALSE;
				put_native_word(field, get_le32(word));
				while (reader->next_pointer_word < reader->layout->pointer_word_count &&
					reader->layout->pointer_words[reader->next_pointer_word].offset < offset + done + index * 4)
				{
					reader->next_pointer_word++;
				}
				if (reader->next_pointer_word < reader->layout->pointer_word_count &&
					reader->layout->pointer_words[reader->next_pointer_word].offset == offset + done + index * 4 &&
					!reader_declared_word(reader, field, reader->layout->pointer_words[reader->next_pointer_word].kind,
						offset + done + index * 4))
				{
					return FALSE;
				}
				if (bytes)
					csmemcpy(bytes + done + index * 4, field, 4);
			}
		}
		reader->segment_left -= words;
		reader->state_words_left -= (unsigned long)words;
		done += words * 4;
	}

	return TRUE;
}

/* the game state's [offset, offset+size) as it is in the image: taken (the
second pass) or only read */
static boolean reader_take(
	struct image_reader *reader,
	long offset,
	long size)
{
	return reader_words(reader, reader->taking ? reader->layout->base + offset : NULL, offset, size);
}

/* a pointer field (at any byte) of a copy: its place made a pointer here.
NONE: not one */
static boolean reader_pointer_field(
	struct image_reader *reader,
	byte *field,
	long *offset,
	long at)
{
	image_word word;
	void *pointer;

	csmemcpy(&word, field, sizeof(word));
	if (!word)
	{
		*offset = NONE;
		pointer = NULL;
	}
	else if ((word & POINTER_TAG_MASK) == POINTER_TAG && (word & POINTER_OFFSET_MASK) < (image_word)reader->layout->size)
	{
		*offset = (long)(word & POINTER_OFFSET_MASK);
		pointer = reader->layout->base + *offset;
		reader->report->pointers++;
	}
	else if (reader->allow_tag_pointers && (word & POINTER_TAG_MASK) == TAG_POINTER_TAG &&
		(word & POINTER_OFFSET_MASK) < (image_word)reader->layout->tags_size)
	{
		/* (into the tag cache: only where a unit declared one) */
		*offset = NONE;
		pointer = reader->layout->tags + (word & POINTER_OFFSET_MASK);
		reader->report->pointers++;
		reader->report->tag_pointers++;
	}
	else if (reader->base_bound && reader->save_base == (unsigned long)reader->layout->base)
	{
		/* outside the game state, kept: the image is at its own base */
		*offset = NONE;
		pointer = (void *)(unsigned long)word;
		reader->report->foreign_pointers++;
	}
	else
	{
		return reader_refuse(reader, reader->base_bound ? _game_state_image_refusal_base : _game_state_image_refusal_pointer, at);
	}
	csmemcpy(field, &pointer, sizeof(pointer));

	return TRUE;
}

/* a data array's header: upstream's game_state_image_data_valid, its
elements its own */
static boolean reader_data_array_header(
	struct image_reader *reader,
	struct data_array *data,
	long offset,
	short maximum_count,
	short element_size)
{
	struct data_array const *live = (struct data_array const *)(reader->layout->base + offset);
	long elements;

	if (!reader_pointer_field(reader, (byte *)&data->data, &elements, offset) ||
		elements != offset + (long)sizeof(struct data_array))
	{
		return reader_refuse(reader, _game_state_image_refusal_data_array, offset);
	}
	data->name[NUMBEROF(data->name) - 1] = 0;
	if (data->signature != DATA_ARRAY_SIGNATURE || data->maximum_count != maximum_count || data->size != element_size ||
		data->count < 0 || data->count > data->maximum_count ||
		data->first_free_absolute_index < 0 || data->first_free_absolute_index > data->maximum_count ||
		data->actual_count < 0 || data->actual_count > data->count)
	{
		return reader_refuse(reader, _game_state_image_refusal_data_array, offset);
	}
	data->valid = data->valid != FALSE;
	data->identifier_zero_invalid = data->identifier_zero_invalid != FALSE;
	if ((data->valid && !data->next_identifier) || (!data->valid && live->valid))
		return reader_refuse(reader, _game_state_image_refusal_data_array, offset);

	return TRUE;
}

/* a script thread in use: its top frame and each frame's previous frame
inside its stack, each frame's values inside it, each result a place in
the game state */
static boolean reader_script_thread(
	struct image_reader *reader,
	byte *thread,
	long thread_offset)
{
	long stack_data = thread_offset + SCRIPT_THREAD_STACK_DATA;
	long frame;
	long frames = 0;

	if (!reader_pointer_field(reader, thread + SCRIPT_THREAD_STACK, &frame, thread_offset))
		return FALSE;
	while (frame != NONE)
	{
		long at = frame - stack_data;
		long result;
		short size;

		if (++frames > SCRIPT_THREAD_MAXIMUM_FRAMES || at < 0 || at > SCRIPT_THREAD_STACK_BYTES - SCRIPT_FRAME_HEADER_BYTES)
			return reader_refuse(reader, _game_state_image_refusal_script_thread, thread_offset);
		csmemcpy(&size, thread + SCRIPT_THREAD_STACK_DATA + at + SCRIPT_FRAME_SIZE, sizeof(size));
		if (size < 0 || size > SCRIPT_THREAD_STACK_BYTES - SCRIPT_FRAME_HEADER_BYTES - at)
			return reader_refuse(reader, _game_state_image_refusal_script_thread, thread_offset);
		if (!reader_pointer_field(reader, thread + SCRIPT_THREAD_STACK_DATA + at + SCRIPT_FRAME_PREVIOUS, &frame,
				thread_offset) ||
			!reader_pointer_field(reader, thread + SCRIPT_THREAD_STACK_DATA + at + SCRIPT_FRAME_RESULT, &result,
				thread_offset))
		{
			return FALSE;
		}
		if (result != NONE && result > reader->layout->size - 4)
			return reader_refuse(reader, _game_state_image_refusal_script_thread, thread_offset);
	}
	reader->report->script_threads++;

	return TRUE;
}

static boolean reader_data_array(
	struct image_reader *reader,
	struct game_state_image_allocation const *allocation,
	long offset,
	short maximum_count,
	short element_size,
	short encoding)
{
	struct data_array header;
	long elements = offset + (long)sizeof(struct data_array);
	long index;

	if (!reader_words(reader, (byte *)&header, offset, sizeof(header)) ||
		!reader_data_array_header(reader, &header, offset, maximum_count, element_size))
	{
		return FALSE;
	}
	if (reader->taking)
		csmemcpy(reader->layout->base + offset, &header, sizeof(header));
	if (allocation && allocation->pointer_field_count)
	{
		for (index = 0; index < maximum_count; index++)
		{
			long element_offset = elements + index * element_size;
			short identifier;
			short field;

			if (!reader_words(reader, reader->element, element_offset, element_size))
				return FALSE;
			csmemcpy(&identifier, reader->element, sizeof(identifier));
			for (field = 0; index < header.count && identifier && field < allocation->pointer_field_count; field++)
			{
				long place;
				boolean valid;

				reader->allow_tag_pointers = TRUE;
				valid = reader_pointer_field(reader, reader->element + allocation->pointer_fields[field], &place,
					element_offset);
				reader->allow_tag_pointers = FALSE;
				if (!valid)
					return FALSE;
			}
			if (reader->taking)
				csmemcpy(reader->layout->base + element_offset, reader->element, (unsigned long)element_size);
		}
		return TRUE;
	}
	if (encoding != _game_state_image_encoding_script_threads)
		return reader_take(reader, elements, (long)maximum_count * element_size);
	for (index = 0; index < maximum_count; index++)
	{
		byte thread[SCRIPT_THREAD_BYTES];
		long thread_offset = elements + index * SCRIPT_THREAD_BYTES;
		short identifier;

		if (!reader_words(reader, thread, thread_offset, SCRIPT_THREAD_BYTES))
			return FALSE;
		csmemcpy(&identifier, thread, sizeof(identifier));
		if (index < header.count && identifier && !reader_script_thread(reader, thread, thread_offset))
			return FALSE;
		if (reader->taking)
			csmemcpy(reader->layout->base + thread_offset, thread, SCRIPT_THREAD_BYTES);
	}

	return TRUE;
}

/* a memory pool: upstream's game_state_image_memory_pool_valid, its blocks
read in their order */
static boolean reader_memory_pool(
	struct image_reader *reader,
	struct game_state_image_allocation const *allocation)
{
	struct game_state_image_layout const *layout = reader->layout;
	struct memory_pool header;
	long room = allocation->offset + (long)sizeof(struct memory_pool);
	long end = room + allocation->pool_size;
	long position = room;
	long base_address;
	long first;
	long last;
	long block;
	long previous = NONE;
	long used = 0;
	long blocks = 0;

	if (!reader_words(reader, (byte *)&header, allocation->offset, sizeof(header)) ||
		!reader_pointer_field(reader, (byte *)&header.base_address, &base_address, allocation->offset) ||
		!reader_pointer_field(reader, (byte *)&header.first_block, &first, allocation->offset) ||
		!reader_pointer_field(reader, (byte *)&header.last_block, &last, allocation->offset))
	{
		return FALSE;
	}
	header.name[NUMBEROF(header.name) - 1] = 0;
	if (header.signature != MEMORY_POOL_SIGNATURE || base_address != room || header.size != allocation->pool_size)
		return reader_refuse(reader, _game_state_image_refusal_memory_pool, allocation->offset);
	if (reader->taking)
		csmemcpy(layout->base + allocation->offset, &header, sizeof(header));
	for (block = first; block != NONE; )
	{
		struct memory_pool_block copy;
		long reference;
		long next;
		long previous_block;

		if (block < position || block > end - (long)sizeof(copy) || (block & 3) ||
			++blocks > allocation->pool_size / (long)sizeof(copy))
		{
			return reader_refuse(reader, _game_state_image_refusal_memory_pool, block);
		}
		if (!reader_take(reader, position, block - position) ||
			!reader_words(reader, (byte *)&copy, block, sizeof(copy)) ||
			!reader_pointer_field(reader, (byte *)&copy.reference, &reference, block) ||
			!reader_pointer_field(reader, (byte *)&copy.next_block, &next, block) ||
			!reader_pointer_field(reader, (byte *)&copy.previous_block, &previous_block, block))
		{
			return FALSE;
		}
		if (copy.header_signature != BLOCK_HEADER_SIGNATURE || copy.trailer_signature != BLOCK_TRAILER_SIGNATURE ||
			copy.size < (long)sizeof(copy) || (copy.size & 3) || copy.size > end - block ||
			previous_block != previous || (next != NONE && next < block + copy.size) ||
			reference == NONE || (reference & 3) || !image_in_data_array_elements(layout, reference, sizeof(void *)))
		{
			return reader_refuse(reader, _game_state_image_refusal_memory_pool, block);
		}
		if (reader->taking)
			csmemcpy(layout->base + block, &copy, sizeof(copy));
		if (!reader_take(reader, block + (long)sizeof(copy), copy.size - (long)sizeof(copy)))
			return FALSE;
		used += copy.size;
		previous = block;
		position = block + copy.size;
		block = next;
	}
	if (last != previous || header.free_size != allocation->pool_size - used)
		return reader_refuse(reader, _game_state_image_refusal_memory_pool, allocation->offset);

	return reader_take(reader, position, end - position);
}

static boolean reader_lruv_cache(
	struct image_reader *reader,
	struct game_state_image_allocation const *allocation)
{
	struct lruv_cache header;
	long blocks;

	if (!reader_words(reader, (byte *)&header, allocation->offset, sizeof(header)) ||
		!reader_pointer_field(reader, (byte *)&header.blocks, &blocks, allocation->offset))
	{
		return FALSE;
	}
	header.name[NUMBEROF(header.name) - 1] = 0;
	if (header.signature != LRUV_CACHE_SIGNATURE || blocks != allocation->offset + (long)sizeof(struct lruv_cache) ||
		header.page_count != allocation->page_count || header.page_size_bits != allocation->page_size_bits)
	{
		return reader_refuse(reader, _game_state_image_refusal_lruv_cache, allocation->offset);
	}
	header.delete_block_proc = allocation->delete_block_proc;
	header.locked_block_proc = allocation->locked_block_proc;
	if (reader->taking)
		csmemcpy(reader->layout->base + allocation->offset, &header, sizeof(header));

	return reader_data_array(reader, NULL, blocks, allocation->maximum_count, sizeof(struct lruv_cache_block),
		_game_state_image_encoding_plain);
}

/* one pass over the file: the header, then the game state */
static boolean reader_pass(
	struct image_reader *reader,
	struct game_state_header const *expected)
{
	struct game_state_image_layout const *layout = reader->layout;
	byte header[GAME_STATE_IMAGE_HEADER_BYTES];
	struct game_state_header decoded;
	short refusal;
	long offset = 0;
	long index;
	unsigned long payload_bytes;
	unsigned long state_bytes = (unsigned long)(layout->cpu_used + layout->gpu_used);

	if (!reader->file->seek(reader->file->context, 0) ||
		!reader->file->read(reader->file->context, header, GAME_STATE_IMAGE_HEADER_BYTES))
	{
		return reader_refuse(reader, _game_state_image_refusal_read, 0);
	}
	refusal = image_header_check(header);
	if (refusal)
		return reader_refuse(reader, refusal, 0);
	if (reader->taking ? csmemcmp(header, reader->first_header, sizeof(header)) != 0 :
		(csmemcpy(reader->first_header, header, sizeof(header)), FALSE))
	{
		return reader_refuse(reader, _game_state_image_refusal_read, 0);
	}
	if (get_le32(header + HEADER_BYTE_ORDER) != (image_word)(image_port_big_endian() ? IMAGE_BIG_ENDIAN : IMAGE_LITTLE_ENDIAN))
		return reader_refuse(reader, _game_state_image_refusal_byte_order, 0);
	if (get_le32(header + HEADER_ENCODING) != IMAGE_ENCODING_WORDS)
		return reader_refuse(reader, _game_state_image_refusal_encoding, 0);
	if (get_le32(header + HEADER_STATE_BYTES) != state_bytes ||
		get_le32(header + HEADER_CPU_USED) != (image_word)layout->cpu_used ||
		get_le32(header + HEADER_GPU_USED) != (image_word)layout->gpu_used ||
		get_le32(header + HEADER_STATE_SIZE) != (image_word)layout->size ||
		get_le32(header + HEADER_ALLOCATION_COUNT) != (image_word)layout->allocation_count ||
		get_le32(header + HEADER_LAYOUT_DIGEST) != (image_word)image_layout_digest(layout) ||
		get_le32(header + HEADER_ALLOCATION_CHECKSUM) != (image_word)(layout->allocation_size_checksum & 0xFFFFFFFFUL))
	{
		return reader_refuse(reader, _game_state_image_refusal_layout, 0);
	}
	image_header_decode(header, &decoded);
	if (expected)
	{
		if (csstrcmp(decoded.build_number, expected->build_number))
			return reader_refuse(reader, _game_state_image_refusal_build, 0);
		if (csstrcmp(decoded.map_name, expected->map_name))
			return reader_refuse(reader, _game_state_image_refusal_map, 0);
		if (decoded.player_count != expected->player_count)
			return reader_refuse(reader, _game_state_image_refusal_players, 0);
		if (decoded.cache_file_checksum != (expected->cache_file_checksum & 0xFFFFFFFFUL))
			return reader_refuse(reader, _game_state_image_refusal_map_checksum, 0);
	}
	payload_bytes = get_le32(header + HEADER_PAYLOAD_BYTES);
	reader->chunk_bytes = (long)get_le32(header + HEADER_CHUNK_BYTES);
	reader->chunk_count = get_le32(header + HEADER_CHUNK_COUNT);
	/* (words, zero runs and control words: at most a control word every
	literal maximum, and one for each zero run) */
	if (reader->chunk_bytes != GAME_STATE_IMAGE_CHUNK_BYTES || payload_bytes < 4 || (payload_bytes & 3) ||
		payload_bytes > state_bytes * 2 + 8 ||
		reader->chunk_count != (payload_bytes + GAME_STATE_IMAGE_CHUNK_BYTES - 1) / GAME_STATE_IMAGE_CHUNK_BYTES)
	{
		return reader_refuse(reader, _game_state_image_refusal_length, 0);
	}
	reader->base_bound = (get_le32(header + HEADER_FLAGS) & IMAGE_FLAG_BASE_BOUND) != 0;
	reader->save_base = get_le32(header + HEADER_SAVE_BASE);
	reader->payload_left = payload_bytes;
	reader->chunk_index = 0;
	reader->chunk_size = 0;
	reader->chunk_position = 0;
	reader->segment_left = 0;
	reader->segment_zero = FALSE;
	reader->state_words_left = state_bytes / 4;
	reader->next_pointer_word = 0;
	sha256_begin(&reader->sha);
	if (!reader->chunk_checksums)
	{
		reader->chunk_checksums = image_allocate(reader->chunk_count * sizeof(unsigned long));
		if (!reader->chunk_checksums)
			return reader_refuse(reader, _game_state_image_refusal_memory, 0);
	}
	csmemset(reader->report, 0, offsetof(struct game_state_image_report, peak_bytes));
	reader->report->first_address_like_offset = NONE;

	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];
		boolean valid = FALSE;

		if (!reader_take(reader, offset, allocation->offset - offset))
			return FALSE;
		switch (allocation->kind)
		{
		case _game_state_image_data:
			valid = reader_data_array(reader, allocation, allocation->offset, allocation->maximum_count,
				allocation->element_size, allocation->encoding);
			break;
		case _game_state_image_memory_pool:
			valid = reader_memory_pool(reader, allocation);
			break;
		case _game_state_image_lruv_cache:
			valid = reader_lruv_cache(reader, allocation);
			break;
		}
		if (!valid)
			return FALSE;
		offset = allocation->offset + allocation->size;
	}
	if (!reader_take(reader, offset, layout->cpu_used - offset) ||
		!reader_take(reader, layout->size - layout->gpu_used, layout->gpu_used))
	{
		return FALSE;
	}
	/* nothing after the game state's words */
	if (reader->segment_left || reader->state_words_left || reader->chunk_position != reader->chunk_size ||
		reader->payload_left || reader->chunk_index != reader->chunk_count)
	{
		return reader_refuse(reader, _game_state_image_refusal_length, layout->cpu_used);
	}
	sha256_end(&reader->sha, reader->digest);
	if (csmemcmp(reader->digest, header + HEADER_DIGEST, 32))
		return reader_refuse(reader, _game_state_image_refusal_digest, 0);
	reader->report->base_bound = reader->base_bound;
	reader->report->save_base = reader->save_base;
	reader->report->state_bytes = state_bytes;
	reader->report->payload_bytes = payload_bytes;
	reader->report->chunks = reader->chunk_count;
	reader->report->chunk_bytes = (unsigned long)reader->chunk_bytes;
	reader->report->file_bytes = GAME_STATE_IMAGE_HEADER_BYTES +
		payload_bytes + reader->chunk_count * GAME_STATE_IMAGE_CHUNK_HEADER_BYTES;

	return TRUE;
}

/* after the second pass: each block's reference made again from its block,
each lruv cache's procedures this build's */
static void reader_after_taking(
	struct image_reader *reader)
{
	struct game_state_image_layout const *layout = reader->layout;
	long index;

	reader->report->references = 0;
	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];
		struct memory_pool *pool = (struct memory_pool *)(layout->base + allocation->offset);
		struct memory_pool_block *block;

		if (allocation->kind != _game_state_image_memory_pool)
			continue;
		for (block = pool->first_block; block; block = block->next_block)
		{
			*block->reference = block + 1;
			reader->report->references++;
		}
	}
}

static short image_read(
	struct game_state_image_layout const *layout,
	struct game_state_header const *expected_header,
	struct game_state_image_file const *file,
	boolean take,
	void (*before_taking)(void))
{
	struct image_reader reader;
	struct game_state_header expected;
	short result = _game_state_image_refused;

	csmemset(&image_report, 0, sizeof(image_report));
	image_report.first_address_like_offset = NONE;
	if (expected_header)
		expected = *expected_header; /* (the header is in the game state, which the second pass takes) */
	csmemset(&reader, 0, sizeof(reader));
	reader.layout = layout;
	reader.file = file;
	reader.report = &image_report;
	if (!image_layout_valid(layout) || !file)
	{
		reader_refuse(&reader, _game_state_image_refusal_layout, 0);
	}
	else
	{
		reader.chunk = image_allocate(GAME_STATE_IMAGE_CHUNK_BYTES);
		reader.element = image_allocate(image_largest_pointer_element(layout));
		if (!reader.chunk || !reader.element)
		{
			reader_refuse(&reader, _game_state_image_refusal_memory, 0);
		}
		else if (reader_pass(&reader, expected_header ? &expected : NULL))
		{
			image_report.passes = 1;
			if (!take)
			{
				result = _game_state_image_taken;
			}
			else
			{
				/* checked whole: now taken */
				/* (from here on the game state is changed: the before-load
				procedures, then the second pass) */
				reader.writing = TRUE;
				if (before_taking)
					before_taking();
				reader.taking = TRUE;
				if (reader_pass(&reader, expected_header ? &expected : NULL))
				{
					reader_after_taking(&reader);
					result = _game_state_image_taken;
				}
				else if (reader.writing)
				{
					result = _game_state_image_failed_while_taking;
				}
				image_report.passes = 2;
			}
			image_report_digest(reader.digest);
		}
	}
	image_report.refusal = reader.refusal;
	image_report.refusal_offset = reader.refusal_offset;
	image_free(reader.chunk_checksums);
	image_free(reader.element);
	image_free(reader.chunk);

	return result;
}

/* ---------- public code */

boolean game_state_image_write(
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	struct game_state_image_file const *file)
{
	byte digest[32];
	boolean success;

	if (image_observer)
		image_observer(3, &image_report);
	success = file && image_write_or_measure(layout, header, file, digest);

	if (image_observer)
		image_observer(0, &image_report);

	return success;
}

boolean game_state_image_digest(
	struct game_state_image_layout const *layout,
	struct game_state_header const *header,
	unsigned long digest[8])
{
	byte bytes[32];
	struct game_state_image_report report = image_report;
	boolean success = image_write_or_measure(layout, header, NULL, bytes);

	csmemcpy(digest, image_report.digest, sizeof(image_report.digest));
	image_report = report;

	return success;
}

boolean game_state_image_read_header(
	struct game_state_image_file const *file,
	struct game_state_header *header)
{
	byte bytes[GAME_STATE_IMAGE_HEADER_BYTES];

	if (!file || !file->seek(file->context, 0) || !file->read(file->context, bytes, GAME_STATE_IMAGE_HEADER_BYTES) ||
		image_header_check(bytes))
	{
		return FALSE;
	}
	image_header_decode(bytes, header);

	return TRUE;
}

boolean game_state_image_check(
	struct game_state_image_layout const *layout,
	struct game_state_header const *expected,
	struct game_state_image_file const *file)
{
	boolean valid;

	if (image_observer)
		image_observer(3, &image_report);
	valid = image_read(layout, expected, file, FALSE, NULL) == _game_state_image_taken;

	if (image_observer)
		image_observer(2, &image_report);

	return valid;
}

short game_state_image_load(
	struct game_state_image_layout const *layout,
	struct game_state_header const *expected,
	struct game_state_image_file const *file,
	void (*before_taking)(void))
{
	short result;

	if (image_observer)
		image_observer(3, &image_report);
	result = image_read(layout, expected, file, TRUE, before_taking);

	if (result != _game_state_image_taken)
	{
		error(_error_silent, "the saved game is not taken: %s (at %ld)%s",
			game_state_image_refusal_name(image_report.refusal), image_report.refusal_offset,
			result == _game_state_image_failed_while_taking ? ", after it was checked: the game state is not usable" : "");
	}
	if (image_observer)
		image_observer(1, &image_report);

	return result;
}

struct game_state_image_report const *game_state_image_last_report(
	void)
{
	return &image_report;
}

char const *game_state_image_refusal_name(
	short refusal)
{
	static char const *const names[NUMBER_OF_GAME_STATE_IMAGE_REFUSALS] =
	{
		"none", "read", "magic", "version", "header_checksum", "incomplete", "byte_order", "encoding", "layout",
		"build", "map", "players", "map_checksum", "chunk", "chunk_checksum", "segment", "length", "digest",
		"pointer", "data_array", "memory_pool", "lruv_cache", "script_thread", "base", "memory"
	};

	return refusal >= 0 && refusal < NUMBER_OF_GAME_STATE_IMAGE_REFUSALS ? names[refusal] : "unknown";
}

void game_state_image_set_observer(
	void (*observer)(short event, struct game_state_image_report const *report))
{
	image_observer = observer;
}

/* ---------- port (HWI-079): the image's pointer places

The places (game-state offsets) of the words the image writes as places,
found as the writer finds them (above), so that a harness's digest of the
game state makes exactly these pointers canonical and reads every other word
as the value it is: a datum handle or a bit vector can hold a number that
reads as an address in the game state, and only at some bases. One more
than the image's: a declared pointer field of every element below a data
array's count, in use or not (a deleted element keeps its last pointer,
which a digest of the elements below the count reads). Script threads'
pointers into their own stacks are listed too (at two-byte offsets). */

static boolean places_add(
	long *offsets,
	long capacity,
	long *count,
	long offset)
{
	if (*count >= capacity)
	{
		(*count)++;
		return FALSE;
	}
	offsets[(*count)++] = offset;

	return TRUE;
}

static void places_data_array(
	struct game_state_image_layout const *layout,
	struct game_state_image_allocation const *allocation,
	long offset,
	long *offsets,
	long capacity,
	long *count)
{
	struct data_array const *live = (struct data_array const *)(layout->base + offset);
	long elements = offset + (long)sizeof(struct data_array);
	long index;

	places_add(offsets, capacity, count, offset + (long)offsetof(struct data_array, data));
	for (index = 0; allocation && allocation->pointer_field_count && index < live->count &&
		index < live->maximum_count; index++)
	{
		short field;

		for (field = 0; field < allocation->pointer_field_count; field++)
			places_add(offsets, capacity, count, elements + index * live->size + allocation->pointer_fields[field]);
	}
	if (!allocation || allocation->encoding != _game_state_image_encoding_script_threads)
		return;
	for (index = 0; index < live->count && index < live->maximum_count; index++)
	{
		long thread_offset = elements + index * SCRIPT_THREAD_BYTES;
		byte const *thread = layout->base + thread_offset;
		byte const *frame;
		short identifier;
		long frames = 0;

		csmemcpy(&identifier, thread, sizeof(identifier));
		if (!identifier)
			continue;
		places_add(offsets, capacity, count, thread_offset + SCRIPT_THREAD_STACK);
		csmemcpy(&frame, thread + SCRIPT_THREAD_STACK, sizeof(frame));
		while (frame && frames++ < SCRIPT_THREAD_MAXIMUM_FRAMES)
		{
			unsigned long at = (unsigned long)(frame - (thread + SCRIPT_THREAD_STACK_DATA));
			short size;

			if (at > SCRIPT_THREAD_STACK_BYTES - SCRIPT_FRAME_HEADER_BYTES)
				break;
			csmemcpy(&size, frame + SCRIPT_FRAME_SIZE, sizeof(size));
			if (size < 0 || (unsigned long)size > SCRIPT_THREAD_STACK_BYTES - SCRIPT_FRAME_HEADER_BYTES - at)
				break;
			places_add(offsets, capacity, count, thread_offset + SCRIPT_THREAD_STACK_DATA + (long)at + SCRIPT_FRAME_PREVIOUS);
			places_add(offsets, capacity, count, thread_offset + SCRIPT_THREAD_STACK_DATA + (long)at + SCRIPT_FRAME_RESULT);
			csmemcpy(&frame, frame + SCRIPT_FRAME_PREVIOUS, sizeof(frame));
		}
	}
}

long game_state_image_pointer_places(
	struct game_state_image_layout const *layout,
	long *offsets,
	long capacity)
{
	long count = 0;
	long index;

	if (!image_layout_valid(layout))
		return NONE;
	for (index = 0; index < layout->pointer_word_count; index++)
		places_add(offsets, capacity, &count, layout->pointer_words[index].offset);
	for (index = 0; index < layout->allocation_count; index++)
	{
		struct game_state_image_allocation const *allocation = &layout->allocations[index];

		switch (allocation->kind)
		{
		case _game_state_image_data:
			places_data_array(layout, allocation, allocation->offset, offsets, capacity, &count);
			break;
		case _game_state_image_memory_pool:
		{
			struct memory_pool const *pool = (struct memory_pool const *)(layout->base + allocation->offset);
			long end = allocation->offset + (long)sizeof(struct memory_pool) + allocation->pool_size;
			struct memory_pool_block const *block = pool->first_block;
			long blocks = 0;

			places_add(offsets, capacity, &count, allocation->offset + (long)offsetof(struct memory_pool, base_address));
			places_add(offsets, capacity, &count, allocation->offset + (long)offsetof(struct memory_pool, first_block));
			places_add(offsets, capacity, &count, allocation->offset + (long)offsetof(struct memory_pool, last_block));
			while (block && blocks++ < MAXIMUM_POOL_BLOCKS)
			{
				long at = (long)((byte const *)block - layout->base);
				unsigned long reference = (unsigned long)block->reference - (unsigned long)layout->base;

				if (at < allocation->offset || at > end - (long)sizeof(*block) || (at & 3))
					break;
				places_add(offsets, capacity, &count, at + (long)offsetof(struct memory_pool_block, reference));
				places_add(offsets, capacity, &count, at + (long)offsetof(struct memory_pool_block, next_block));
				places_add(offsets, capacity, &count, at + (long)offsetof(struct memory_pool_block, previous_block));
				/* the element's field the block's reference points to */
				if (reference < (unsigned long)layout->size)
					places_add(offsets, capacity, &count, (long)reference);
				block = block->next_block;
			}
			break;
		}
		case _game_state_image_lruv_cache:
			places_add(offsets, capacity, &count, allocation->offset + (long)offsetof(struct lruv_cache, blocks));
			places_data_array(layout, NULL, allocation->offset + (long)sizeof(struct lruv_cache), offsets, capacity, &count);
			break;
		}
	}
	if (count <= capacity)
		qsort(offsets, (size_t)count, sizeof(long), image_compare_offsets);

	return count;
}
