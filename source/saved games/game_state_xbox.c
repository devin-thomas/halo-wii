/*
GAME_STATE_XBOX.C

symbols in this file:
001AFBB0 0160:
	_game_state_allocate_buffer (0000)
001AFD10 0040:
	_game_state_free_buffer (0000)
001AFD50 00d0:
	_game_state_create_or_open_file (0000)
001AFE20 0040:
	_game_state_close_file (0000)
001AFE60 00e0:
	_game_state_write_to_file (0000)
001AFF40 0120:
	_game_state_read_from_file (0000)
001B0060 0090:
	_game_state_write_core (0000)
001B00F0 0080:
	_game_state_read_core_header (0000)
001B0170 00a0:
	_game_state_read_core (0000)
001B0210 0010:
	_game_state_get_persistent_storage_filename (0000)
001B0220 0020:
	_game_state_get_persistent_storage_path (0000)
001B0240 0030:
	_delete_persistent_storage (0000)
001B0270 0190:
	_game_state_open_persistent_storage (0000)
001B0400 01b0:
	_game_state_read_header_from_persistent_storage (0000)
001B05B0 0160:
	_game_state_write_to_persistent_storage (0000)
001B0710 00b0:
	_game_state_read_from_persistent_storage (0000)
001B07C0 0020:
	_game_state_create_persistent_storage (0000)
002A8084 001f:
	??_C@_0BP@IDAHEHI@?$CIunsigned?5long?$CJresult?$DN?$DNaddress?$AA@ (0000)
002A80A4 001e:
	??_C@_0BO@BBKFFHIP@?$CB?$CIgpu_size?$CG?$CICPU_PAGE_SIZE?91?$CJ?$CJ?$AA@ (0000)
002A80C4 001e:
	??_C@_0BO@BHHCKDPL@?$CB?$CIcpu_size?$CG?$CICPU_PAGE_SIZE?91?$CJ?$CJ?$AA@ (0000)
002A80E4 000b:
	??_C@_0L@JBLOLKNC@gpu_size?$DO0?$AA@ (0000)
002A80F0 000b:
	??_C@_0L@CBGHMJFA@cpu_size?$DO0?$AA@ (0000)
002A80FC 002a:
	??_C@_0CK@FIOMLFFF@?$CBxbox_game_state_globals?4buffer_@ (0000)
002A8128 002d:
	??_C@_0CN@KDCMADJG@c?3?2halo?2SOURCE?2saved?5games?2game_@ (0000)
002A8158 0029:
	??_C@_0CJ@FFFLELDG@xbox_game_state_globals?4buffer_a@ (0000)
002A8184 002e:
	??_C@_0CO@BEKHBMJJ@couldn?8t?5open?5or?5create?5saved?5ga@ (0000)
002A81B4 0010:
	??_C@_0BA@MOOKLLKO@z?3?2savegame?4bin?$AA@ (0000)
002A81C4 0023:
	??_C@_0CD@DDMIEKAL@?$CBxbox_game_state_globals?4file_op@ (0000)
002A81E8 0022:
	??_C@_0CC@CLCLOOEI@xbox_game_state_globals?4file_ope@ (0000)
002A820C 0025:
	??_C@_0CF@BLHGLGEN@couldn?8t?5write?5saved?5game?5file?5?$CI@ (0000)
002A8234 0024:
	??_C@_0CE@CPCCDNFP@couldn?8t?5read?5saved?5game?5file?5?$CI?$CD@ (0000)
002A8258 0048:
	??_C@_0EI@JODPNGEB@xbox_game_state_globals?4file_val@ (0000)
002A82A0 000b:
	??_C@_0L@BACPNKLN@d?3?2core?2?$CFs?$AA@ (0000)
002A82AC 0008:
	??_C@_07ELALHFFD@d?3?2core?$AA@ (0000)
002A82B4 0036:
	??_C@_0DG@PGBLNDAA@game?5state?5has?5been?5corrupted?5?$CIt@ (0000)
002A82EC 000d:
	??_C@_0N@KAKGDJDL@savegame?4bin?$AA@ (0000)
002A82FC 0030:
	??_C@_0DA@GAKEGFCK@couldn?8t?5open?5or?5create?5persiste@ (0000)
002A832C 0028:
	??_C@_0CI@LDKKAEDE@couldn?8t?5resize?5persistent?5stora@ (0000)
002A8354 0033:
	??_C@_0DD@EGEEEGAD@couldn?8t?5read?5header?5from?5persis@ (0000)
002A8388 0026:
	??_C@_0CG@JDCOBIBA@checksum?5failed?5on?5persistent?5st@ (0000)
002A83B0 002c:
	??_C@_0CM@LDKGDBEH@failed?5to?5write?5to?5persistent?5st@ (0000)
002A83DC 0021:
	??_C@_0CB@GFGKONGF@header_size?$DMsizeof?$CIsaved_header?$CJ@ (0000)
002A8400 002d:
	??_C@_0CN@DOEIAHAD@failed?5to?5read?5from?5persistent?5s@ (0000)
004D27D0 0014:
	_xbox_game_state_globals (0000)
*/

/* ---------- headers */

#include "cache/physical_memory_map.h"
#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "interface/player_ui.h"
#include "main/main.h"
#include "memory/crc.h"
#include "saved games/game_state.h"
#include "saved games/game_state_generations.h"
#include "saved games/game_state_image.h"
#include "sound/sound_manager.h"
#include <xtl.h>

/* ---------- constants */

enum
{
	CPU_PAGE_SIZE = 0x1000,
	/* the native builds' larger game state (halo_port_capacity.h) */
	GAME_STATE_SIZE = HALO_PORT_GAME_STATE_SIZE
};

/* port (HWI-080): the checkpoint's primary (game_state_generations.h) */
#define GAME_STATE_CHECKPOINT_PATH "z:\\savegame.bin"
#define GAME_STATE_NO_FILE_ATTRIBUTES ((DWORD)-1)

/* ---------- structures */

struct xbox_game_state_globals_prefix
{
	boolean buffer_allocated;
	byte reserved001[3];
	void *buffer;
	long buffer_size;
	boolean file_open;
	boolean file_valid_for_read;
	byte reserved00E[2];
	HANDLE handle;
};

typedef char verify_xbox_game_state_buffer_offset[
	offsetof(struct xbox_game_state_globals_prefix, buffer) == 0x4 ? 1 : -1];
typedef char verify_xbox_game_state_file_open_offset[
	offsetof(struct xbox_game_state_globals_prefix, file_open) == 0xC ? 1 : -1];
typedef char verify_xbox_game_state_handle_offset[
	offsetof(struct xbox_game_state_globals_prefix, handle) == 0x10 ? 1 : -1];
typedef char verify_xbox_game_state_globals_prefix_size[
	sizeof(struct xbox_game_state_globals_prefix) == 0x14 ? 1 : -1];

/* port (HWI-080): a saved game's generations as files
(game_state_generation_files_new) */
struct game_state_generation_io
{
	DWORD flags; /* CreateFileA's */
	char const *what;
	/* game_state_generation_attempt_header's */
	void *header;
	boolean damaged;
	boolean check_difficulty;
};

/* ---------- prototypes */

static void game_state_header_terminate(
	void *header,
	long header_size);
static HANDLE game_state_open_persistent_storage(
	const char *directory);
static void game_state_image_file_new(
	struct game_state_image_file *file,
	HANDLE *handle);
static boolean game_state_image_file_end(
	HANDLE handle);
static boolean game_state_image_file_take(
	HANDLE handle,
	boolean check_header,
	char const *what);
static short game_state_image_file_take_result(
	HANDLE handle,
	boolean check_header,
	char const *what);
static void game_state_generation_files_new(
	struct game_state_generation_files *files,
	struct game_state_generation_io *io);
static short game_state_generation_attempt_header(
	void *context,
	char const *path);
static boolean game_state_generations_take(
	char const *primary,
	DWORD flags,
	char const *what,
	boolean check_difficulty);

/* ---------- globals */

static struct xbox_game_state_globals_prefix xbox_game_state_globals = { 0 };

/* ---------- public code */

void *game_state_allocate_buffer(
	unsigned long address,
	unsigned long cpu_size,
	unsigned long gpu_size)
{
	void *result;

	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		46,
		!xbox_game_state_globals.buffer_allocated);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		48,
		address);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		49,
		cpu_size>0);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		50,
		gpu_size>0);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		53,
		!(cpu_size&(CPU_PAGE_SIZE-1)));
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		54,
		!(gpu_size&(CPU_PAGE_SIZE-1)));

	result = physical_memory_get_game_state_base_address();
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		58,
		result);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		59,
		(unsigned long)result==address);

	XPhysicalProtect(
		(void *)(address+cpu_size),
		gpu_size,
		PAGE_READWRITE|PAGE_WRITECOMBINE);

	xbox_game_state_globals.buffer_allocated = TRUE;
	xbox_game_state_globals.buffer = (void *)address;
	xbox_game_state_globals.buffer_size = cpu_size+gpu_size;

	return result;
}

void game_state_free_buffer(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		75,
		xbox_game_state_globals.buffer_allocated);
	XPhysicalFree(xbox_game_state_globals.buffer);
	xbox_game_state_globals.buffer_allocated = FALSE;

	return;
}

void game_state_create_or_open_file(
	void)
{
	char candidate[GAME_STATE_GENERATION_PATH_BYTES];

	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		86,
		xbox_game_state_globals.buffer_allocated);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		87,
		!xbox_game_state_globals.file_open);

	/* port (HWI-076): the canonical image is as long as it is written, not
	the game state's size (game_state_image.c). port (HWI-080): the
	checkpoint is kept in generations (game_state_generations.h), each
	opened as it is written or read; the primary is only made sure of here,
	and not held open (it is renamed), nor made over a candidate a save
	stopped before promoting (the next save promotes it) */
	if (game_state_generation_path(candidate, sizeof(candidate), GAME_STATE_CHECKPOINT_PATH,
			_game_state_generation_candidate) &&
		GetFileAttributesA(GAME_STATE_CHECKPOINT_PATH) == GAME_STATE_NO_FILE_ATTRIBUTES &&
		GetFileAttributesA(candidate) != GAME_STATE_NO_FILE_ATTRIBUTES)
	{
		xbox_game_state_globals.handle = INVALID_HANDLE_VALUE;
		xbox_game_state_globals.file_open = TRUE;

		return;
	}
	xbox_game_state_globals.handle = CreateFileA(GAME_STATE_CHECKPOINT_PATH,
		GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
		FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (xbox_game_state_globals.handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(xbox_game_state_globals.handle);
		xbox_game_state_globals.handle = INVALID_HANDLE_VALUE;
		xbox_game_state_globals.file_open = TRUE;
	}
	else
	{
		match_vassert(
			"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
			97,
			FALSE,
			csprintf(temporary, "couldn't open or create saved game file (#%d)",
				GetLastError()));
	}

	return;
}

void game_state_close_file(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		106,
		xbox_game_state_globals.file_open);
	/* port (HWI-080): no handle is held (game_state_create_or_open_file) */
	if (xbox_game_state_globals.handle != INVALID_HANDLE_VALUE)
		CloseHandle(xbox_game_state_globals.handle);
	xbox_game_state_globals.handle = INVALID_HANDLE_VALUE;
	xbox_game_state_globals.file_open = FALSE;

	return;
}

boolean game_state_write_to_file(
	void)
{
	boolean result = FALSE;
	struct game_state_generation_io io;
	struct game_state_generation_files files;

	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		120,
		xbox_game_state_globals.buffer_allocated);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		121,
		xbox_game_state_globals.file_open);

	/* port (HWI-076): the canonical image, in chunks. port (HWI-080): as a
	new generation, checked whole before it replaces the last checkpoint;
	one not written leaves the last as it was (game_state_generations.h),
	and game_state_save keeps the previous checkpoint available */
	csmemset(&io, 0, sizeof(io));
	io.flags = FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN;
	game_state_generation_files_new(&files, &io);
	if (game_state_generations_save(&files, GAME_STATE_CHECKPOINT_PATH))
	{
		xbox_game_state_globals.file_valid_for_read = TRUE;
		result = TRUE;
	}
	else
	{
		match_vassert(
			"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
			132,
			FALSE,
			csprintf(temporary, "couldn't write saved game file (#%d)", GetLastError()));
	}

	return result;
}

boolean game_state_read_from_file(
	void)
{
	boolean result;

	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		144,
		xbox_game_state_globals.buffer_allocated);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		145,
		xbox_game_state_globals.file_open);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		146,
		xbox_game_state_globals.file_valid_for_read || recover_saved_games_hack);

	/* port (HWI-076): the canonical image, checked whole in a first pass
	over the file and only then taken in a second; one that is not taken
	leaves the game state as it is (game_state_revert starts the map over).
	port (HWI-080): the newest generation that is taken; one that failed
	while being taken ends the attempt (game_state_generations_take) */
	result = game_state_generations_take(GAME_STATE_CHECKPOINT_PATH,
		FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN, "saved game file", FALSE);

	return result;
}

boolean game_state_write_core(
	const char *name,
	void *buffer,
	long buffer_size)
{
	char path[1024];
	HANDLE file;
	boolean result = FALSE;
	struct game_state_image_file image;

	/* port (HWI-076): the canonical image of the game state (buffer) */
	(void)buffer;
	(void)buffer_size;
	CreateDirectoryA("d:\\core", NULL);
	sprintf(path, "d:\\core\\%s", name);
	file = CreateFileA(path, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		game_state_image_file_new(&image, &file);
		if (game_state_port_image_write(&image))
		{
			result = TRUE;
		}
	}
	CloseHandle(file);

	return result;
}

boolean game_state_read_core_header(
	const char *name,
	void *header,
	long header_size)
{
	char path[1024];
	HANDLE file;
	boolean result = FALSE;
	struct game_state_image_file image;

	sprintf(path, "d:\\core\\%s", name);
	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
		NULL);
	/* port (HWI-076): the canonical image's header, as the game state's */
	if (file != INVALID_HANDLE_VALUE && header_size == sizeof(struct game_state_header))
	{
		game_state_image_file_new(&image, &file);
		if (game_state_image_read_header(&image, header))
		{
			game_state_header_terminate(header, header_size);
			result = TRUE;
		}
	}
	CloseHandle(file);

	return result;
}

boolean game_state_read_core(
	const char *name,
	void *buffer,
	long buffer_size)
{
	char path[1024];
	HANDLE file;
	boolean result = FALSE;

	sprintf(path, "d:\\core\\%s", name);
	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
		NULL);
	/* port (HWI-076): the canonical image, checked whole and only then
	taken into the game state (buffer); one that is not taken leaves it */
	(void)buffer;
	(void)buffer_size;
	if (file != INVALID_HANDLE_VALUE)
		result = game_state_image_file_take(file, TRUE, "core");
	CloseHandle(file);

	return result;
}

const char *game_state_get_persistent_storage_filename(
	void)
{
	return "savegame.bin";
}

static __inline boolean game_state_get_persistent_storage_path(
	char *path)
{
	boolean result;

	result = player_ui_get_path_to_local_player_profile_directory(0, path) != FALSE;

	return result;
}

/* port (HWI-080): the persistent storage's primary (game_state_generations.h).
Upstream deleted a saved game it could not write or read; its generations
are kept instead, the previous one the one a load falls back to */
static boolean game_state_get_persistent_storage_primary(
	char *path)
{
	char directory[256];
	boolean result = FALSE;

	if (game_state_get_persistent_storage_path(directory) &&
		strlen(directory) + strlen("savegame.bin") < GAME_STATE_GENERATION_PATH_BYTES)
	{
		strcpy(path, directory);
		strcat(path, "savegame.bin");
		result = TRUE;
	}

	return result;
}

static HANDLE game_state_open_persistent_storage(
	const char *directory)
{
	char path[256];
	HANDLE file;
	boolean success = FALSE;

	if (directory || game_state_get_persistent_storage_path(path))
	{
		if (directory)
		{
			strcpy(path, directory);
		}
		else
		{
			game_state_get_persistent_storage_path(path);
		}
		strcat(path, "savegame.bin");

		file = CreateFileA(path, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0,
			NULL);
		/* port (HWI-076): the canonical image is as long as it is written;
		the file is no longer made the game state's size first */
		if (file != INVALID_HANDLE_VALUE)
		{
			success = TRUE;
		}
		else
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
				498,
				FALSE,
				csprintf(temporary, "couldn't open or create persistent storage \"%s\"",
					path));
		}
	}

	return success ? file : INVALID_HANDLE_VALUE;
}

/* port: the strings of a header read from a file end in it (the file is
anyone's) */
static void game_state_header_terminate(
	void *header,
	long header_size)
{
	if (header_size == sizeof(struct game_state_header))
	{
		struct game_state_header *game_state_header = header;

		game_state_header->map_name[NUMBEROF(game_state_header->map_name) - 1] = 0;
		game_state_header->build_number[NUMBEROF(game_state_header->build_number) - 1] = 0;
	}
}

boolean game_state_read_header_from_persistent_storage(
	void *header,
	unsigned long *header_checksum,
	long header_size,
	long buffer_size,
	boolean *corrupted)
{
	char primary[GAME_STATE_GENERATION_PATH_BYTES];
	struct game_state_generation_io io;
	struct game_state_generation_files files;
	short generation;
	boolean result = FALSE;

	/* port (HWI-076): the canonical image's header, and the whole image
	checked (game_state_image_check: its chunks, its digest, its layout)
	where the upstream summed the file's CRC. port (HWI-080): of the newest
	generation that checks whole, the one game_state_read_from_persistent_storage
	then takes; corrupted only when none does and one held something */
	(void)header_checksum;
	(void)buffer_size;
	if (corrupted)
		*corrupted = FALSE;

	if (header_size == sizeof(struct game_state_header) && game_state_get_persistent_storage_primary(primary))
	{
		csmemset(&io, 0, sizeof(io));
		io.header = header;
		game_state_generation_files_new(&files, &io);
		if (game_state_generations_load(&files, primary, game_state_generation_attempt_header, &generation) ==
			_game_state_generation_taken)
		{
			if (generation != _game_state_generation_current)
				error(_error_silent, "persistent storage: the newest saved game that checks whole is generation %d",
					generation);
			result = TRUE;
		}
		else
		{
			if (io.damaged && corrupted)
				*corrupted = TRUE;
			error(_error_silent, io.damaged ? "checksum failed on persistent storage" :
				"no saved game in persistent storage");
		}
	}

	return result;
}

void game_state_write_to_persistent_storage(
	void *buffer,
	unsigned long *header_checksum,
	long header_size,
	long buffer_size)
{
	char primary[GAME_STATE_GENERATION_PATH_BYTES];
	struct game_state_generation_io io;
	struct game_state_generation_files files;

	/* port (HWI-076): the canonical image of the game state (buffer); its
	chunks' CRCs and its digest stand for the upstream header checksum.
	port (HWI-080): as a new generation, checked whole before it replaces
	the saved game; one not written leaves that as it was, where upstream
	deleted it */
	(void)buffer;
	(void)header_checksum;
	(void)header_size;
	(void)buffer_size;
	if (game_state_get_persistent_storage_primary(primary))
	{
		csmemset(&io, 0, sizeof(io));
		game_state_generation_files_new(&files, &io);
		if (!game_state_generations_save(&files, primary))
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
				351,
				FALSE,
				csprintf(temporary, "failed to write to persistent storage (#%d)",
					GetLastError()));
		}
	}

	return;
}

boolean game_state_read_from_persistent_storage(
	void *buffer,
	long buffer_size)
{
	char primary[GAME_STATE_GENERATION_PATH_BYTES];
	boolean result = FALSE;

	/* port (HWI-076): the canonical image, checked whole and only then
	taken into the game state (buffer). port (HWI-080): the newest
	generation that is taken; none is deleted (upstream deleted the one it
	could not read), and one that failed while being taken ends the attempt
	(game_state_generations_take) */
	(void)buffer;
	(void)buffer_size;
	if (game_state_get_persistent_storage_primary(primary))
		result = game_state_generations_take(primary, 0, "persistent storage", TRUE);

	return result;
}

void game_state_create_persistent_storage(
	const char *path)
{
	HANDLE handle;

	handle = game_state_open_persistent_storage(path);
	if (handle != INVALID_HANDLE_VALUE)
		CloseHandle(handle);

	return;
}

/* ---------- private code */

/* port (HWI-076): a file handle as game_state_image.c reads and writes it */
static boolean game_state_image_file_read(
	void *context,
	void *buffer,
	long size)
{
	unsigned long bytes_read;

	return ReadFile(*(HANDLE *)context, buffer, size, &bytes_read, NULL) && (long)bytes_read == size;
}

static boolean game_state_image_file_write(
	void *context,
	void const *buffer,
	long size)
{
	unsigned long bytes_written;

	return WriteFile(*(HANDLE *)context, buffer, size, &bytes_written, NULL) && (long)bytes_written == size;
}

static boolean game_state_image_file_seek(
	void *context,
	long offset)
{
	return SetFilePointer(*(HANDLE *)context, offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
}

static void game_state_image_file_new(
	struct game_state_image_file *file,
	HANDLE *handle)
{
	file->context = handle;
	file->read = game_state_image_file_read;
	file->write = game_state_image_file_write;
	file->seek = game_state_image_file_seek;
}

/* the file ends where the image just written does */
static boolean game_state_image_file_end(
	HANDLE handle)
{
	return SetFilePointer(handle, (long)game_state_image_last_report()->file_bytes, NULL, FILE_BEGIN) !=
		INVALID_SET_FILE_POINTER && SetEndOfFile(handle);
}

/* an image checked whole and then taken: TRUE if taken. One refused leaves
the game state as it is; one that read differently the second time has
changed it, and the game halts (as upstream did for a game state it could
not read) */
static boolean game_state_image_file_take(
	HANDLE handle,
	boolean check_header,
	char const *what)
{
	return game_state_image_file_take_result(handle, check_header, what) == _game_state_image_taken;
}

/* (game_state_image.h's result) */
static short game_state_image_file_take_result(
	HANDLE handle,
	boolean check_header,
	char const *what)
{
	struct game_state_image_file file;
	short result;

	game_state_image_file_new(&file, &handle);
	result = game_state_port_image_load(&file, check_header);
	if (result == _game_state_image_refused)
	{
		error(_error_silent, "couldn't read %s: %s", what,
			game_state_image_refusal_name(game_state_image_last_report()->refusal));
	}
	/* Assertions only log in release builds. Once taking has changed the
	state, an ordinary refusal/reset or another generation is unsafe. */
	if (result == _game_state_image_failed_while_taking)
		vhalt("game state has been corrupted while loading a saved image");

	return result;
}

/* port (HWI-080): a saved game's generations (game_state_generations.h) as
files. None is held open: each is opened as it is written, checked or read,
and closed before it is renamed or removed */
static boolean game_state_generation_exists(
	void *context,
	char const *path)
{
	(void)context;

	return GetFileAttributesA(path) != GAME_STATE_NO_FILE_ATTRIBUTES;
}

static boolean game_state_generation_remove(
	void *context,
	char const *path)
{
	(void)context;

	return DeleteFileA(path) != FALSE;
}

static boolean game_state_generation_rename(
	void *context,
	char const *from,
	char const *to)
{
	(void)context;

	return MoveFileA(from, to) != FALSE;
}

/* the image's end is where the file's is, and the file is closed (no flush
is asked of the medium: there is none in this API, docs/wii) */
static boolean game_state_generation_write(
	void *context,
	char const *path)
{
	struct game_state_generation_io *io = context;
	struct game_state_image_file image;
	HANDLE file;
	boolean result = FALSE;

	file = CreateFileA(path, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, io->flags, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		game_state_image_file_new(&image, &file);
		result = game_state_port_image_write(&image) && game_state_image_file_end(file);
		if (!CloseHandle(file))
			result = FALSE;
	}

	return result;
}

/* checked whole in bounded memory (game_state_image_check); nothing taken */
static boolean game_state_generation_check(
	void *context,
	char const *path,
	boolean compatible)
{
	struct game_state_generation_io *io = context;
	struct game_state_image_file image;
	HANDLE file;
	boolean result = FALSE;

	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, io->flags, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		game_state_image_file_new(&image, &file);
		result = game_state_port_image_check(&image, compatible);
		CloseHandle(file);
	}

	return result;
}

static void game_state_generation_files_new(
	struct game_state_generation_files *files,
	struct game_state_generation_io *io)
{
	files->context = io;
	files->exists = game_state_generation_exists;
	files->remove = game_state_generation_remove;
	files->rename = game_state_generation_rename;
	files->write = game_state_generation_write;
	files->check = game_state_generation_check;
}

/* a generation's header, of an image that checks whole (not against this
game state: the caller sees to that) */
static short game_state_generation_attempt_header(
	void *context,
	char const *path)
{
	struct game_state_generation_io *io = context;
	struct game_state_image_file image;
	HANDLE file;
	short result = _game_state_generation_refused;

	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, io->flags, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		game_state_image_file_new(&image, &file);
		if (!game_state_image_read_header(&image, io->header))
		{
			/* (an empty file holds no saved game; anything else is damaged) */
			if (GetFileSize(file, NULL))
				io->damaged = TRUE;
		}
		else if (game_state_port_image_check(&image, FALSE))
		{
			game_state_header_terminate(io->header, sizeof(struct game_state_header));
			result = _game_state_generation_taken;
		}
		else
		{
			io->damaged = TRUE;
		}
		CloseHandle(file);
	}

	return result;
}

/* a generation checked whole and taken (game_state_image_file_take) */
static short game_state_generation_attempt_take(
	void *context,
	char const *path)
{
	struct game_state_generation_io *io = context;
	HANDLE file;
	short result = _game_state_generation_refused;

	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, io->flags, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		/* The menu checked one generation's difficulty, but fallback may
		select another. Check the actual candidate before any state is taken. */
		if (io->check_difficulty)
		{
			struct game_state_image_file image;
			struct game_state_header header;

			game_state_image_file_new(&image, &file);
			if (!game_state_image_read_header(&image, &header) || header.difficulty != main_get_difficulty())
			{
				CloseHandle(file);
				return _game_state_generation_refused;
			}
		}
		switch (game_state_image_file_take_result(file, TRUE, io->what))
		{
		case _game_state_image_taken:
			result = _game_state_generation_taken;
			break;
		case _game_state_image_failed_while_taking:
			result = _game_state_generation_failed;
			break;
		}
		CloseHandle(file);
	}

	return result;
}

/* the newest generation taken: TRUE if one was. One that failed while it
was taken has changed the game state, and no other is tried: that has
halted as game_state_image_file_take does, and the game state is not the
one before it (it is not pretended to be) */
static boolean game_state_generations_take(
	char const *primary,
	DWORD flags,
	char const *what,
	boolean check_difficulty)
{
	struct game_state_generation_io io;
	struct game_state_generation_files files;
	short generation;
	short result;

	csmemset(&io, 0, sizeof(io));
	io.flags = flags;
	io.what = what;
	io.check_difficulty = check_difficulty;
	game_state_generation_files_new(&files, &io);
	result = game_state_generations_load(&files, primary, game_state_generation_attempt_take, &generation);
	if (result == _game_state_generation_taken && generation != _game_state_generation_current)
		error(_error_silent, "%s: taken from generation %d (the newer ones were refused)", what, generation);

	return result == _game_state_generation_taken;
}
