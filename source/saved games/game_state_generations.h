/*
GAME_STATE_GENERATIONS.H

port (HWI-080): a saved game kept in generations, so that a save that does
not finish, or one written wrong, leaves the one before it to load.

A saved game's file (the primary, savegame.bin) has two siblings: the
candidate (savegame.new), which a save writes and checks whole before
anything else is touched, and the previous (savegame.bak), the last primary
that checked whole. A save that stops anywhere leaves a primary, a candidate
or a previous that checks whole, unless none did before it. A load tries
them newest first and takes the first that is not refused.

The files are the caller's (game_state_xbox.c); this is only the order of
the steps. It makes no claim beyond the file system's own: a rename that
power loss interrupts is the medium's to keep or lose (docs/wii).
*/

#ifndef __GAME_STATE_GENERATIONS_H
#define __GAME_STATE_GENERATIONS_H
#pragma once

/* ---------- constants */

enum
{
	GAME_STATE_GENERATION_PATH_BYTES = 256,
};

/* in the order a load tries them */
enum game_state_generation
{
	_game_state_generation_current,   /* savegame.bin */
	_game_state_generation_candidate, /* savegame.new: written, not yet promoted */
	_game_state_generation_previous,  /* savegame.bak */

	NUMBER_OF_GAME_STATE_GENERATIONS
};

/* what an attempt at one generation comes to (game_state_generations_load) */
enum game_state_generation_attempt
{
	_game_state_generation_taken,
	/* not taken, nothing changed: the next generation is tried */
	_game_state_generation_refused,
	/* not taken, and the game state is no longer what it was: no other
	generation is tried (game_state_image.h's failed_while_taking) */
	_game_state_generation_failed,
};

/* ---------- structures */

/* a saved game's files, as the caller reaches them */
struct game_state_generation_files
{
	void *context;
	boolean (*exists)(void *context, char const *path);
	boolean (*remove)(void *context, char const *path);
	/* to does not exist */
	boolean (*rename)(void *context, char const *from, char const *to);
	/* the game state's image, written whole to a new file at path */
	boolean (*write)(void *context, char const *path);
	/* the image at path checked whole, nothing taken; compatible: also as
	one of this game state's map, build and players */
	boolean (*check)(void *context, char const *path, boolean compatible);
};

/* ---------- prototypes */

/* a generation's path: the primary's with its extension (.bin) replaced;
FALSE if it does not fit */
boolean game_state_generation_path(
	char *path,
	long size,
	char const *primary,
	short generation);
/* writes a new generation and promotes it; FALSE leaves the primary or the
previous as it was, and the candidate removed where it can be */
boolean game_state_generations_save(
	struct game_state_generation_files const *files,
	char const *primary);
/* attempt at each generation that exists, newest first, until one is not
refused; its result (refused when none exist), and which in *generation
(NONE when none is taken or failed) */
short game_state_generations_load(
	struct game_state_generation_files const *files,
	char const *primary,
	short (*attempt)(void *context, char const *path),
	short *generation);

#endif // __GAME_STATE_GENERATIONS_H
