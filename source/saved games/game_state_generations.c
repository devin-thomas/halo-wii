/*
GAME_STATE_GENERATIONS.C

port (HWI-080): the steps of a saved game's generations
(game_state_generations.h). Each step either leaves the files as a load
finds a checked image in, or stops there:

	save                          after it, a load takes
	-----------------------------------------------------------------
	(primary gone, candidate)     candidate promoted first
	remove a stale candidate      primary, or previous
	write the candidate           primary, or previous
	check it whole                primary, or previous
	primary checks whole:
	  remove previous             primary
	  primary -> previous         candidate (checked), or previous
	primary does not:
	  remove primary              candidate (checked), or previous
	candidate -> primary          primary (checked), or previous

A primary that does not check whole or is incompatible with this game is
not made the previous: the compatible previous is kept.
*/

/* ---------- headers */

#ifndef GAME_STATE_GENERATIONS_STANDALONE
#include "cseries/cseries.h"
#endif
#include "saved games/game_state_generations.h"

/* ---------- structures */

/* port (HWI-046): a signed block's generations as game_state_generation_files */
struct game_state_generation_block_io
{
	struct game_state_generation_block_files const *files;
	long signed_bytes;
	/* a save's: the block it writes, and the candidate that must read back
	as it */
	unsigned char const *block;
	char const *candidate;
	/* a load's: where the block taken goes */
	unsigned char *taken;
	unsigned char read[GAME_STATE_GENERATION_BLOCK_BYTES];
};

/* ---------- prototypes */

static boolean game_state_generations_paths(
	char paths[NUMBER_OF_GAME_STATE_GENERATIONS][GAME_STATE_GENERATION_PATH_BYTES],
	char const *primary);
static boolean game_state_generations_abandon(
	struct game_state_generation_files const *files,
	char const *candidate);
static void game_state_generation_block_files_new(
	struct game_state_generation_files *files,
	struct game_state_generation_block_io *io);
static short game_state_generation_block_attempt(
	void *context,
	char const *path);

/* ---------- public code */

boolean game_state_generation_path(
	char *path,
	long size,
	char const *primary,
	short generation)
{
	static char const *const extensions[NUMBER_OF_GAME_STATE_GENERATIONS] = { NULL, ".new", ".bak" };
	long length = 0;
	long stem;
	long index;

	if (!path || size <= 0 || !primary || generation < 0 || generation >= NUMBER_OF_GAME_STATE_GENERATIONS)
		return FALSE;
	while (primary[length])
		length++;
	/* the extension is the last '.' of the name (after the last '\\') */
	stem = length;
	for (index = length - 1; index >= 0 && primary[index] != '\\' && primary[index] != '/'; index--)
	{
		if (primary[index] == '.')
		{
			stem = index;
			break;
		}
	}
	if (generation == _game_state_generation_current)
		stem = length;
	if (stem >= size)
		return FALSE;
	for (index = 0; index < stem; index++)
		path[index] = primary[index];
	if (extensions[generation])
	{
		char const *extension = extensions[generation];

		for (; *extension; extension++, index++)
		{
			if (index >= size - 1)
				return FALSE;
			path[index] = *extension;
		}
	}
	if (index >= size)
		return FALSE;
	path[index] = 0;

	return TRUE;
}

boolean game_state_generations_save(
	struct game_state_generation_files const *files,
	char const *primary)
{
	char paths[NUMBER_OF_GAME_STATE_GENERATIONS][GAME_STATE_GENERATION_PATH_BYTES];
	char const *current = paths[_game_state_generation_current];
	char const *candidate = paths[_game_state_generation_candidate];
	char const *previous = paths[_game_state_generation_previous];

	if (!files || !game_state_generations_paths(paths, primary))
		return FALSE;

	/* a save stopped between moving the primary and promoting its checked
	candidate: the candidate is the primary (checked again below before it
	is made the previous) */
	if (!files->exists(files->context, current) && files->exists(files->context, candidate))
		files->rename(files->context, candidate, current);
	if (files->exists(files->context, candidate) && !files->remove(files->context, candidate))
		return FALSE;

	/* the candidate, checked whole: until then nothing else is touched */
	if (!files->write(files->context, candidate) || !files->check(files->context, candidate, TRUE))
		return game_state_generations_abandon(files, candidate);

	if (files->exists(files->context, current))
	{
		if (files->check(files->context, current, TRUE))
		{
			if (files->exists(files->context, previous) && !files->remove(files->context, previous))
				return game_state_generations_abandon(files, candidate);
			if (!files->rename(files->context, current, previous))
				return game_state_generations_abandon(files, candidate);
		}
		else if (!files->remove(files->context, current))
		{
			return game_state_generations_abandon(files, candidate);
		}
	}

	/* (not promoted: the checked candidate is still the newest a load finds) */
	return files->rename(files->context, candidate, current);
}

short game_state_generations_load(
	struct game_state_generation_files const *files,
	char const *primary,
	short (*attempt)(void *context, char const *path),
	short *generation)
{
	char paths[NUMBER_OF_GAME_STATE_GENERATIONS][GAME_STATE_GENERATION_PATH_BYTES];
	short result = _game_state_generation_refused;
	short index;

	if (generation)
		*generation = NONE;
	if (!files || !attempt || !game_state_generations_paths(paths, primary))
		return result;
	for (index = 0; index < NUMBER_OF_GAME_STATE_GENERATIONS; index++)
	{
		if (!files->exists(files->context, paths[index]))
			continue;
		result = attempt(files->context, paths[index]);
		if (result != _game_state_generation_refused)
		{
			if (generation)
				*generation = index;
			break;
		}
	}

	return result;
}

boolean game_state_generation_block_save(
	struct game_state_generation_block_files const *files,
	char const *primary,
	unsigned char const *block,
	long signed_bytes)
{
	char candidate[GAME_STATE_GENERATION_PATH_BYTES];
	struct game_state_generation_block_io io;
	struct game_state_generation_files generation_files;

	if (!files || !block ||
		!game_state_generation_path(candidate, sizeof(candidate), primary, _game_state_generation_candidate))
	{
		return FALSE;
	}
	io.files = files;
	io.signed_bytes = signed_bytes;
	io.block = block;
	io.candidate = candidate;
	io.taken = NULL;
	game_state_generation_block_files_new(&generation_files, &io);

	return game_state_generations_save(&generation_files, primary);
}

boolean game_state_generation_block_load(
	struct game_state_generation_block_files const *files,
	char const *primary,
	unsigned char *block,
	long signed_bytes,
	short *generation)
{
	struct game_state_generation_block_io io;
	struct game_state_generation_files generation_files;
	short taken = NONE;
	boolean result = FALSE;

	if (files && block)
	{
		io.files = files;
		io.signed_bytes = signed_bytes;
		io.block = NULL;
		io.candidate = NULL;
		io.taken = block;
		game_state_generation_block_files_new(&generation_files, &io);
		result = game_state_generations_load(&generation_files, primary, game_state_generation_block_attempt, &taken) ==
			_game_state_generation_taken;
	}
	if (generation)
		*generation = result ? taken : NONE;

	return result;
}

boolean game_state_generation_block_signed(
	struct game_state_generation_block_files const *files,
	unsigned char const *block,
	long signed_bytes)
{
	unsigned char signature[GAME_STATE_GENERATION_SIGNATURE_BYTES];
	long index;

	if (!files || !block || signed_bytes <= 0 ||
		signed_bytes > GAME_STATE_GENERATION_BLOCK_BYTES - GAME_STATE_GENERATION_SIGNATURE_BYTES)
	{
		return FALSE;
	}
	files->sign(files->context, block, signed_bytes, signature);
	for (index = 0; index < GAME_STATE_GENERATION_SIGNATURE_BYTES; index++)
	{
		if (block[signed_bytes + index] != signature[index])
			return FALSE;
	}

	return TRUE;
}

/* ---------- private code */

static boolean game_state_generations_paths(
	char paths[NUMBER_OF_GAME_STATE_GENERATIONS][GAME_STATE_GENERATION_PATH_BYTES],
	char const *primary)
{
	short index;

	for (index = 0; index < NUMBER_OF_GAME_STATE_GENERATIONS; index++)
	{
		if (!game_state_generation_path(paths[index], GAME_STATE_GENERATION_PATH_BYTES, primary, index))
			return FALSE;
	}

	return TRUE;
}

/* a candidate not promoted: removed (the files are as before the save) */
static boolean game_state_generations_abandon(
	struct game_state_generation_files const *files,
	char const *candidate)
{
	if (files->exists(files->context, candidate))
		files->remove(files->context, candidate);

	return FALSE;
}

/* port (HWI-046): a signed block's generations. A block holds no map or
build, so it is compatible whenever it is signed; the candidate is also
read back as the block the save meant */
static boolean game_state_generation_block_exists(
	void *context,
	char const *path)
{
	struct game_state_generation_block_io *io = context;

	return io->files->exists(io->files->context, path);
}

static boolean game_state_generation_block_remove(
	void *context,
	char const *path)
{
	struct game_state_generation_block_io *io = context;

	return io->files->remove(io->files->context, path);
}

static boolean game_state_generation_block_rename(
	void *context,
	char const *from,
	char const *to)
{
	struct game_state_generation_block_io *io = context;

	return io->files->rename(io->files->context, from, to);
}

static boolean game_state_generation_block_write(
	void *context,
	char const *path)
{
	struct game_state_generation_block_io *io = context;

	return io->block && io->files->write(io->files->context, path, io->block);
}

static boolean game_state_generation_block_check(
	void *context,
	char const *path,
	boolean compatible)
{
	struct game_state_generation_block_io *io = context;
	long index;

	(void)compatible;
	if (!io->files->read(io->files->context, path, io->read) ||
		!game_state_generation_block_signed(io->files, io->read, io->signed_bytes))
	{
		return FALSE;
	}
	if (io->block && io->candidate)
	{
		for (index = 0; io->candidate[index] == path[index]; index++)
		{
			if (!path[index])
				break;
		}
		if (io->candidate[index] == path[index])
		{
			for (index = 0; index < GAME_STATE_GENERATION_BLOCK_BYTES; index++)
			{
				if (io->read[index] != io->block[index])
					return FALSE;
			}
		}
	}

	return TRUE;
}

static void game_state_generation_block_files_new(
	struct game_state_generation_files *files,
	struct game_state_generation_block_io *io)
{
	files->context = io;
	files->exists = game_state_generation_block_exists;
	files->remove = game_state_generation_block_remove;
	files->rename = game_state_generation_block_rename;
	files->write = game_state_generation_block_write;
	files->check = game_state_generation_block_check;
}

/* a generation's block, taken when it is signed */
static short game_state_generation_block_attempt(
	void *context,
	char const *path)
{
	struct game_state_generation_block_io *io = context;
	long index;

	if (!game_state_generation_block_check(io, path, TRUE))
		return _game_state_generation_refused;
	for (index = 0; index < GAME_STATE_GENERATION_BLOCK_BYTES; index++)
		io->taken[index] = io->read[index];

	return _game_state_generation_taken;
}
