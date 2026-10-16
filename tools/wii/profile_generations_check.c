/* HWI-046: fault injection for a profile's or gametype's signed block kept
in generations (game_state_generation_block_save/load,
source/saved games/game_state_generations.c), compiled for the host against
a modelled file system that holds each file's bytes (Win32 rename
semantics: the destination must not exist). Every operation of a save is in
turn interrupted (the process stops there: longjmp) or made to fail, and
writes are torn or altered; what a load then takes is compared byte for
byte with the block that was saved. Exit 0 and "ok" when every case holds. */

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define GAME_STATE_GENERATIONS_STANDALONE
#include "saved games/game_state_generations.c"

enum
{
	MAXIMUM_FILES = 8,
	NAME_BYTES = 64,
	BLOCK = GAME_STATE_GENERATION_BLOCK_BYTES,
	SIGNATURE = GAME_STATE_GENERATION_SIGNATURE_BYTES,
	/* saved_game_files.c's PLAYER_PROFILE_CHECKSUM_DATA_SIZE and
	PLAYLIST_PROFILE_CHECKSUM_DATA_SIZE */
	PROFILE_SIGNED = 48,
	GAMETYPE_SIGNED = 104,
};

enum
{
	_write_whole,
	_write_torn,     /* says it succeeded; only a part reached the file */
	_write_altered,  /* says it succeeded; a byte after the signature differs */
	_write_short,    /* says it succeeded; the file is shorter than a block */
};

struct model_file
{
	char name[NAME_BYTES];
	boolean present;
	long length;
	unsigned char bytes[BLOCK];
};

static struct
{
	struct model_file files[MAXIMUM_FILES];
	int operations;
	int interrupt_at;   /* the operation the process stops at (0: none) */
	int fail_at;        /* the operation that fails (0: none) */
	int write_mode;
	jmp_buf interrupted;
} model;

static int failures;
static int cases;

#define PRIMARY "u:\\udata\\4d530004\\0123456789ab\\blam.sav"
#define CANDIDATE "u:\\udata\\4d530004\\0123456789ab\\blam.new"
#define PREVIOUS "u:\\udata\\4d530004\\0123456789ab\\blam.bak"
#define GAMETYPE "u:\\udata\\4d530004\\0123456789ab\\blam.lst"

static struct model_file *model_find(char const *path)
{
	int index;

	for (index = 0; index < MAXIMUM_FILES; index++)
		if (model.files[index].present && !strcmp(model.files[index].name, path))
			return &model.files[index];
	return NULL;
}

static struct model_file *model_make(char const *path)
{
	struct model_file *file = model_find(path);
	int index;

	for (index = 0; !file && index < MAXIMUM_FILES; index++)
		if (!model.files[index].present)
			file = &model.files[index];
	memset(file, 0, sizeof(*file));
	snprintf(file->name, sizeof(file->name), "%s", path);
	file->present = TRUE;
	return file;
}

/* an operation: the process may stop here, or the operation fail */
static boolean model_operation(void)
{
	model.operations++;
	if (model.interrupt_at && model.operations == model.interrupt_at)
		longjmp(model.interrupted, 1);
	return !(model.fail_at && model.operations == model.fail_at);
}

static boolean model_exists(void *context, char const *path)
{
	(void)context;
	return model_find(path) != NULL;
}

static boolean model_remove(void *context, char const *path)
{
	struct model_file *file;

	(void)context;
	if (!model_operation() || !(file = model_find(path)))
		return FALSE;
	file->present = FALSE;
	return TRUE;
}

static boolean model_rename(void *context, char const *from, char const *to)
{
	struct model_file *file;

	(void)context;
	if (!model_operation() || !(file = model_find(from)) || model_find(to))
		return FALSE;
	snprintf(file->name, sizeof(file->name), "%s", to);
	return TRUE;
}

static boolean model_write(void *context, char const *path, unsigned char const *block)
{
	struct model_file *file;

	(void)context;
	if (!model_operation())
		return FALSE;
	/* made new and empty: until the write ends it holds a part of a block */
	file = model_make(path);
	if (!model_operation())
		return FALSE;
	switch (model.write_mode)
	{
	case _write_torn:
		memcpy(file->bytes, block, BLOCK / 4);
		file->length = BLOCK;
		break;
	case _write_short:
		memcpy(file->bytes, block, BLOCK / 2);
		file->length = BLOCK / 2;
		break;
	default:
		memcpy(file->bytes, block, BLOCK);
		if (model.write_mode == _write_altered)
			file->bytes[BLOCK - 1] ^= 0x5A;
		file->length = BLOCK;
		break;
	}
	return TRUE;
}

static boolean model_read(void *context, char const *path, unsigned char *block)
{
	struct model_file *file = model_find(path);

	(void)context;
	if (!model_operation() || !file || file->length < BLOCK)
		return FALSE;
	memcpy(block, file->bytes, BLOCK);
	return TRUE;
}

/* a keyed digest standing for XCalculateSignature: any changed byte or
length changes it */
static void model_sign(void *context, unsigned char const *block, long size, unsigned char *signature)
{
	unsigned long state[SIGNATURE / 4];
	long index;
	int lane;

	(void)context;
	for (lane = 0; lane < SIGNATURE / 4; lane++)
		state[lane] = 2166136261UL ^ (unsigned long)(lane * 0x9E3779B1UL) ^ (unsigned long)size;
	for (index = 0; index < size; index++)
	{
		for (lane = 0; lane < SIGNATURE / 4; lane++)
		{
			state[lane] ^= block[index] + (unsigned long)lane;
			state[lane] = (state[lane] * 16777619UL) & 0xFFFFFFFFUL;
		}
	}
	for (index = 0; index < SIGNATURE; index++)
		signature[index] = (unsigned char)(state[index / 4] >> (8 * (index % 4)));
}

static struct game_state_generation_block_files const model_files =
	{ NULL, model_exists, model_remove, model_rename, model_write, model_read, model_sign };

static void expect(boolean condition, char const *what, char const *scenario, int at)
{
	if (!condition)
	{
		failures++;
		fprintf(stderr, "FAIL %s (%s, operation %d)\n", what, scenario, at);
	}
}

/* save id's block: its data signed, and bytes after the signature (as a
gametype's options are) that the signature does not cover */
static void block_make(unsigned char *block, int id, long signed_bytes)
{
	long index;

	for (index = 0; index < BLOCK; index++)
		block[index] = (unsigned char)(id * 37 + index * 11);
	model_sign(NULL, block, signed_bytes, block + signed_bytes);
}

static void model_reset(void)
{
	memset(&model, 0, sizeof(model));
}

static boolean model_save(int id)
{
	unsigned char block[BLOCK];

	block_make(block, id, PROFILE_SIGNED);
	model.operations = 0;
	return game_state_generation_block_save(&model_files, PRIMARY, block, PROFILE_SIGNED);
}

/* the id whose block a load takes (0 none, -1 a block no save wrote) */
static int model_load(short *generation)
{
	unsigned char block[BLOCK];
	unsigned char expected[BLOCK];
	short ignored;
	int id;

	if (!generation)
		generation = &ignored;
	memset(block, 0xEE, sizeof(block));
	if (!game_state_generation_block_load(&model_files, PRIMARY, block, PROFILE_SIGNED, generation))
	{
		for (id = 0; id < BLOCK; id++)
			if (block[id] != 0xEE)
				return -1; /* (a refused load changed the caller's block) */
		return 0;
	}
	for (id = 1; id < 16; id++)
	{
		block_make(expected, id, PROFILE_SIGNED);
		if (!memcmp(block, expected, BLOCK))
			return id;
	}
	return -1;
}

static void model_prepare(int saves)
{
	int id;

	model_reset();
	for (id = 1; id <= saves; id++)
		model_save(id);
}

static void model_save_interrupted(int id, int at)
{
	model.interrupt_at = at;
	if (!setjmp(model.interrupted))
		model_save(id);
	model.interrupt_at = 0;
}

static int model_operations(int saves)
{
	model_prepare(saves);
	model_save(saves + 1);
	return model.operations;
}

static void test_interrupted_and_failed(void)
{
	static char const *const names[] = { "fresh", "one saved", "two saved", "three saved" };
	int saves;

	for (saves = 0; saves < 4; saves++)
	{
		int operations = model_operations(saves);
		int at;

		expect(operations > 0, "a save operates", names[saves], 0);
		for (at = 1; at <= operations; at++)
		{
			int mode;

			for (mode = 0; mode < 2; mode++)
			{
				boolean saved = FALSE;
				int id = saves + 1;
				int loaded;

				cases++;
				model_prepare(saves);
				if (mode == 0)
				{
					model_save_interrupted(id, at);
				}
				else
				{
					model.fail_at = at;
					saved = model_save(id);
					model.fail_at = 0;
				}
				/* the last block saved whole, or this one: never a part */
				loaded = model_load(NULL);
				expect(saves == 0 ? loaded == 0 || loaded == id : loaded == saves || loaded == id,
					mode ? "a failed save leaves a signed block" : "an interrupted save leaves a signed block",
					names[saves], at);
				if (saved)
					expect(loaded == id, "a save that succeeded is the one loaded", names[saves], at);

				expect(model_save(id + 1), "the next save succeeds", names[saves], at);
				expect(model_load(NULL) == id + 1, "the next save is loaded", names[saves], at);
				expect(!model_find(CANDIDATE), "no candidate is left", names[saves], at);
				if (saves > 0)
				{
					struct model_file *previous = model_find(PREVIOUS);
					unsigned char expected[BLOCK];
					unsigned char other[BLOCK];

					block_make(expected, saves, PROFILE_SIGNED);
					block_make(other, id, PROFILE_SIGNED);
					expect(previous && (!memcmp(previous->bytes, expected, BLOCK) || !memcmp(previous->bytes, other, BLOCK)),
						"the previous is the block before", names[saves], at);
				}
			}
		}
	}
}

/* writes that say they succeeded but did not write the block are refused
before anything else is touched */
static void test_bad_writes(void)
{
	static char const *const names[] = { "", "torn write", "altered write", "short write" };
	int mode;

	for (mode = _write_torn; mode <= _write_short; mode++)
	{
		unsigned char expected[BLOCK];

		cases++;
		model_prepare(2);
		model.write_mode = mode;
		expect(!model_save(3), "a write that does not read back is refused", names[mode], 0);
		model.write_mode = _write_whole;
		expect(model_load(NULL) == 2, "the last block is loaded", names[mode], 0);
		expect(!model_find(CANDIDATE), "the candidate is removed", names[mode], 0);
		block_make(expected, 2, PROFILE_SIGNED);
		expect(model_find(PRIMARY) && !memcmp(model_find(PRIMARY)->bytes, expected, BLOCK),
			"the primary is untouched", names[mode], 0);
		block_make(expected, 1, PROFILE_SIGNED);
		expect(model_find(PREVIOUS) && !memcmp(model_find(PREVIOUS)->bytes, expected, BLOCK),
			"the previous is untouched", names[mode], 0);
	}
}

/* a block its caller did not sign is never promoted */
static void test_unsigned_block_refused(void)
{
	unsigned char block[BLOCK];

	cases++;
	model_prepare(1);
	block_make(block, 5, PROFILE_SIGNED);
	block[0] ^= 1;
	model.operations = 0;
	expect(!game_state_generation_block_save(&model_files, PRIMARY, block, PROFILE_SIGNED),
		"an unsigned block is refused", "unsigned block", 0);
	expect(model_load(NULL) == 1 && !model_find(CANDIDATE) && !model_find(PREVIOUS),
		"and the files are as they were", "unsigned block", 0);
}

/* a primary damaged after it was written (or an old-style CRC's) is not
taken, is not made the previous, and a save over it keeps the previous */
static void test_damaged_primary(void)
{
	short generation;
	unsigned char expected[BLOCK];

	cases++;
	model_prepare(2);
	model_find(PRIMARY)->bytes[3] ^= 0x80;
	expect(model_load(&generation) == 1 && generation == _game_state_generation_previous,
		"a damaged primary falls back to the previous", "damaged primary", 0);
	expect(model_save(3), "a save over a damaged primary", "damaged primary", 0);
	block_make(expected, 1, PROFILE_SIGNED);
	expect(model_find(PREVIOUS) && !memcmp(model_find(PREVIOUS)->bytes, expected, BLOCK),
		"the signed previous is kept", "damaged primary", 0);
	model_find(PRIMARY)->length = 10;
	expect(model_load(NULL) == 1, "and loaded when the new primary is short", "damaged primary", 0);
}

/* a save stopped between moving the primary and promoting its candidate:
the candidate is taken, and the next save makes it the primary */
static void test_candidate_alone(void)
{
	short generation;
	int at;

	model_prepare(1);
	model_save(2);
	/* the operation that renames the candidate to the primary is the last */
	at = model.operations;
	cases++;
	model_prepare(1);
	model_save_interrupted(2, at);
	expect(!model_find(PRIMARY) && model_find(CANDIDATE) && model_find(PREVIOUS),
		"stopped before promoting", "candidate alone", at);
	expect(model_load(&generation) == 2 && generation == _game_state_generation_candidate,
		"the checked candidate is taken", "candidate alone", at);
	expect(model_save(3) && model_load(&generation) == 3 && generation == _game_state_generation_current,
		"the next save promotes", "candidate alone", at);
	expect(!model_find(CANDIDATE), "no candidate is left", "candidate alone", at);
}

/* a profile's and a gametype's generations have the same names; each type
takes only a block signed as its own (saved_game_file_type_from_generations) */
static void test_types_apart(void)
{
	unsigned char block[BLOCK];
	unsigned char taken[BLOCK];
	short generation;

	cases++;
	model_reset();
	block_make(block, 7, GAMETYPE_SIGNED);
	model.operations = 0;
	expect(game_state_generation_block_save(&model_files, GAMETYPE, block, GAMETYPE_SIGNED),
		"a gametype saves", "types apart", 0);
	/* stopped as a save leaves it: the candidate alone */
	snprintf(model_find(GAMETYPE)->name, NAME_BYTES, "%s", CANDIDATE);
	expect(!game_state_generation_block_load(&model_files, PRIMARY, taken, PROFILE_SIGNED, &generation) &&
		generation == NONE, "a gametype's block is not a profile's", "types apart", 0);
	expect(game_state_generation_block_load(&model_files, GAMETYPE, taken, GAMETYPE_SIGNED, &generation) &&
		generation == _game_state_generation_candidate && !memcmp(taken, block, BLOCK),
		"and is a gametype's", "types apart", 0);
}

static void test_signed_bounds(void)
{
	unsigned char block[BLOCK];

	cases++;
	memset(block, 0, sizeof(block));
	expect(!game_state_generation_block_signed(&model_files, block, 0), "nothing signed is refused", "bounds", 0);
	expect(!game_state_generation_block_signed(&model_files, block, BLOCK - SIGNATURE + 1),
		"a signature past the block is refused", "bounds", 0);
	model_sign(NULL, block, BLOCK - SIGNATURE, block + BLOCK - SIGNATURE);
	expect(game_state_generation_block_signed(&model_files, block, BLOCK - SIGNATURE),
		"a signature that ends the block", "bounds", 0);
	expect(!game_state_generation_block_signed(NULL, block, PROFILE_SIGNED), "no files", "bounds", 0);
}

static void test_nothing_saved(void)
{
	short generation = 0;

	cases++;
	model_reset();
	expect(model_load(&generation) == 0 && generation == NONE, "nothing to load", "empty", 0);
	/* an empty primary (a profile's directory made and not yet written) */
	model_make(PRIMARY);
	expect(model_load(NULL) == 0, "an empty primary is not taken", "empty", 0);
	expect(model_save(1) && model_load(NULL) == 1 && !model_find(PREVIOUS),
		"a save over an empty primary keeps no previous", "empty", 0);
}

int main(void)
{
	test_interrupted_and_failed();
	test_bad_writes();
	test_unsigned_block_refused();
	test_damaged_primary();
	test_candidate_alone();
	test_types_apart();
	test_signed_bounds();
	test_nothing_saved();
	if (failures)
	{
		fprintf(stderr, "%d failures in %d cases\n", failures, cases);
		return 1;
	}
	printf("ok %d cases\n", cases);
	return 0;
}
