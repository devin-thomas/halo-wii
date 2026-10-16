/* HWI-080: fault injection for source/saved games/game_state_generations.c,
compiled for the host against a modelled file system (Win32 rename
semantics: the destination must not exist). Every operation of a save is in
turn interrupted (the process stops there: longjmp) or made to fail, and
what a load then takes is checked. Exit 0 and "ok" when every case holds. */

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define GAME_STATE_GENERATIONS_STANDALONE
#include "saved games/game_state_generations.c"

enum { MAXIMUM_FILES = 8, NAME_BYTES = 64 };

struct model_file
{
	char name[NAME_BYTES];
	boolean present;
	boolean complete; /* an image that checks whole */
	boolean incompatible;
	int id;           /* which save wrote it */
};

static struct
{
	struct model_file files[MAXIMUM_FILES];
	int writing_id;
	int operations;
	int interrupt_at;   /* the operation the process stops at (0: none) */
	int fail_at;        /* the operation that fails (0: none) */
	boolean corrupt_write; /* the write says it succeeded; its image does not check */
	boolean fail_while_taking_current;
	int attempts;
	jmp_buf interrupted;
} model;

static int failures;
static int cases;

#define PRIMARY "z:\\savegame.bin"
#define CANDIDATE "z:\\savegame.new"
#define PREVIOUS "z:\\savegame.bak"

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

static boolean model_write(void *context, char const *path)
{
	struct model_file *file;

	(void)context;
	if (!model_operation())
		return FALSE;
	/* made new: until the write ends it is a part of an image */
	file = model_make(path);
	file->id = model.writing_id;
	if (!model_operation())
		return FALSE;
	file->complete = !model.corrupt_write;
	return TRUE;
}

static boolean model_check(void *context, char const *path, boolean compatible)
{
	struct model_file *file = model_find(path);

	(void)context;
	if (!model_operation())
		return FALSE;
	return file && file->complete && (!compatible || !file->incompatible);
}

static short model_attempt(void *context, char const *path)
{
	struct model_file *file = model_find(path);

	(void)context;
	model.attempts++;
	if (model.fail_while_taking_current && !strcmp(path, PRIMARY))
		return _game_state_generation_failed;
	return file && file->complete && !file->incompatible ? _game_state_generation_taken : _game_state_generation_refused;
}

static struct game_state_generation_files const model_files =
	{ NULL, model_exists, model_remove, model_rename, model_write, model_check };

static void expect(boolean condition, char const *what, char const *scenario, int at)
{
	if (!condition)
	{
		failures++;
		fprintf(stderr, "FAIL %s (%s, operation %d)\n", what, scenario, at);
	}
}

static void model_reset(void)
{
	memset(&model, 0, sizeof(model));
}

static boolean model_save(int id)
{
	model.writing_id = id;
	model.operations = 0;
	return game_state_generations_save(&model_files, PRIMARY);
}

/* the id a load takes (0 none, -1 failed while taking) */
static int model_load(short *generation)
{
	char path[GAME_STATE_GENERATION_PATH_BYTES];
	short result;
	short ignored;

	if (!generation)
		generation = &ignored;
	result = game_state_generations_load(&model_files, PRIMARY, model_attempt, generation);
	if (result == _game_state_generation_failed)
		return -1;
	if (result != _game_state_generation_taken)
		return 0;
	game_state_generation_path(path, sizeof(path), PRIMARY, *generation);
	return model_find(path)->id;
}

/* the files as that many clean saves leave them */
static void model_prepare(int saves)
{
	int id;

	model_reset();
	for (id = 1; id <= saves; id++)
		model_save(id);
}

static void test_paths(void)
{
	char path[GAME_STATE_GENERATION_PATH_BYTES];
	char tiny[16];

	cases++;
	expect(game_state_generation_path(path, sizeof(path), PRIMARY, _game_state_generation_current) &&
		!strcmp(path, PRIMARY), "current path", "paths", 0);
	expect(game_state_generation_path(path, sizeof(path), PRIMARY, _game_state_generation_candidate) &&
		!strcmp(path, CANDIDATE), "candidate path", "paths", 0);
	expect(game_state_generation_path(path, sizeof(path), "e:\\p.d\\savegame.bin", _game_state_generation_previous) &&
		!strcmp(path, "e:\\p.d\\savegame.bak"), "previous path", "paths", 0);
	expect(game_state_generation_path(path, sizeof(path), "e:\\p.d\\savegame", _game_state_generation_previous) &&
		!strcmp(path, "e:\\p.d\\savegame.bak"), "appended extension", "paths", 0);
	expect(!game_state_generation_path(tiny, sizeof(tiny), "e:\\abcdefghijkl.bin", _game_state_generation_previous),
		"overlong path refused", "paths", 0);
	/* 15 characters and the terminator: just fits; 16 does not */
	expect(game_state_generation_path(tiny, sizeof(tiny), "e:\\abcdefgh.bin", _game_state_generation_previous) &&
		!strcmp(tiny, "e:\\abcdefgh.bak"), "path that just fits", "paths", 0);
	expect(!game_state_generation_path(tiny, sizeof(tiny), "e:\\abcdefghi.bin", _game_state_generation_previous),
		"path one past fitting refused", "paths", 0);
	expect(!game_state_generation_path(path, sizeof(path), PRIMARY, NUMBER_OF_GAME_STATE_GENERATIONS),
		"no such generation", "paths", 0);
}

/* a save the process stops in at an operation (the files stay as they are) */
static void model_save_interrupted(int id, int at)
{
	model.interrupt_at = at;
	if (!setjmp(model.interrupted))
		model_save(id);
	model.interrupt_at = 0;
}

/* the operations a clean save makes after that many saves */
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
				struct model_file *previous;

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
				/* the last save that checked whole, or this one */
				loaded = model_load(NULL);
				expect(saves == 0 ? loaded == 0 || loaded == id : loaded == saves || loaded == id,
					mode ? "a failed save leaves a checked one" : "an interrupted save leaves a checked one",
					names[saves], at);
				if (saved)
					expect(loaded == id, "a save that succeeded is the one loaded", names[saves], at);

				/* and the next save (no fault) recovers, keeping a previous */
				expect(model_save(id + 1), "the next save succeeds", names[saves], at);
				expect(model_load(NULL) == id + 1, "the next save is loaded", names[saves], at);
				expect(!model_find(CANDIDATE), "no candidate is left", names[saves], at);
				previous = model_find(PREVIOUS);
				if (saves > 0)
					expect(previous && previous->complete && (previous->id == saves || previous->id == id),
						"the previous is the one before", names[saves], at);
			}
		}
	}
}

static void test_corrupt_write(void)
{
	cases++;
	model_prepare(2);
	model.corrupt_write = TRUE;
	expect(!model_save(3), "a write that does not check is refused", "corrupt write", 0);
	model.corrupt_write = FALSE;
	expect(model_load(NULL) == 2, "the last one is loaded", "corrupt write", 0);
	expect(!model_find(CANDIDATE), "the candidate is removed", "corrupt write", 0);
	expect(model_find(PREVIOUS) && model_find(PREVIOUS)->id == 1, "the previous is untouched", "corrupt write", 0);
}

static void test_newly_corrupt_primary(void)
{
	short generation;

	cases++;
	model_prepare(2);
	model_find(PRIMARY)->complete = FALSE;
	expect(model_load(&generation) == 1 && generation == _game_state_generation_previous,
		"a damaged primary falls back to the previous", "newly corrupt", 0);
	/* a save over a damaged primary does not make it the previous */
	expect(model_save(3), "a save over a damaged primary", "newly corrupt", 0);
	expect(model_find(PREVIOUS)->id == 1 && model_find(PREVIOUS)->complete,
		"the checked previous is kept", "newly corrupt", 0);
	model_find(PRIMARY)->complete = FALSE;
	expect(model_load(NULL) == 1, "and loaded when the new primary is damaged", "newly corrupt", 0);
}

static void test_failed_while_taking(void)
{
	cases++;
	model_prepare(2);
	model.fail_while_taking_current = TRUE;
	model.attempts = 0;
	expect(model_load(NULL) == -1, "a load that failed while taking says so", "failed while taking", 0);
	expect(model.attempts == 1, "and tries no other generation", "failed while taking", 0);
}

static void test_incompatible_primary_keeps_compatible_backup(void)
{
	int at;
	int operations;

	model_prepare(2);
	model_find(PRIMARY)->incompatible = TRUE;
	model_save(3);
	operations = model.operations;
	for (at = 1; at <= operations; at++)
	{
		int mode;

		for (mode = 0; mode < 2; mode++)
		{
			int loaded;

			cases++;
			model_prepare(2);
			model_find(PRIMARY)->incompatible = TRUE;
			if (mode == 0)
				model_save_interrupted(3, at);
			else
			{
				model.fail_at = at;
				model_save(3);
				model.fail_at = 0;
			}
			loaded = model_load(NULL);
			expect(loaded == 1 || loaded == 3,
				"failed/interrupted save preserves a compatible image", "incompatible primary", at);
			expect(model_find(PREVIOUS) && model_find(PREVIOUS)->id == 1,
				"incompatible primary never replaces compatible backup", "incompatible primary", at);
		}
	}
}

static void test_nothing_saved(void)
{
	short generation = 0;

	cases++;
	model_reset();
	expect(model_load(&generation) == 0 && generation == NONE, "nothing to load", "empty", 0);
	/* an empty primary (as a new profile has) is not made a previous */
	model_make(PRIMARY);
	expect(model_save(1) && model_load(NULL) == 1 && !model_find(PREVIOUS), "a save over an empty primary", "empty", 0);
}

int main(void)
{
	test_paths();
	test_interrupted_and_failed();
	test_corrupt_write();
	test_newly_corrupt_primary();
	test_failed_while_taking();
	test_incompatible_primary_keeps_compatible_backup();
	test_nothing_saved();
	if (failures)
	{
		fprintf(stderr, "%d failures in %d cases\n", failures, cases);
		return 1;
	}
	printf("ok %d cases\n", cases);
	return 0;
}
