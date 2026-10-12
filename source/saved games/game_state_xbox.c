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
#include "memory/crc.h"
#include "saved games/game_state.h"
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
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		86,
		xbox_game_state_globals.buffer_allocated);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		87,
		!xbox_game_state_globals.file_open);

	/* port (HWI-076): the canonical image is as long as it is written, not
	the game state's size (game_state_image.c) */
	xbox_game_state_globals.handle = CreateFileA("z:\\savegame.bin",
		GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
		FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (xbox_game_state_globals.handle != INVALID_HANDLE_VALUE)
	{
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
	CloseHandle(xbox_game_state_globals.handle);
	xbox_game_state_globals.file_open = FALSE;

	return;
}

boolean game_state_write_to_file(
	void)
{
	boolean result = FALSE;
	struct game_state_image_file file;

	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		120,
		xbox_game_state_globals.buffer_allocated);
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		121,
		xbox_game_state_globals.file_open);

	/* port (HWI-076): the canonical image, in chunks */
	game_state_image_file_new(&file, &xbox_game_state_globals.handle);
	if (game_state_port_image_write(&file) &&
		game_state_image_file_end(xbox_game_state_globals.handle))
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
	leaves the game state as it is (game_state_revert starts the map over) */
	result = game_state_image_file_take(xbox_game_state_globals.handle, TRUE, "saved game file");

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

static void delete_persistent_storage(
	void)
{
	char path[256];

	if (game_state_get_persistent_storage_path(path))
		DeleteFileA(path);

	return;
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
	HANDLE file;
	boolean result;
	struct game_state_image_file image;

	/* port (HWI-076): the canonical image's header, and the whole image
	checked (game_state_image_check: its chunks, its digest, its layout)
	where the upstream summed the file's CRC */
	(void)header_checksum;
	(void)buffer_size;
	file = game_state_open_persistent_storage(NULL);
	result = FALSE;
	if (corrupted)
		*corrupted = FALSE;

	if (file != INVALID_HANDLE_VALUE)
	{
		game_state_image_file_new(&image, &file);
		if (header_size != sizeof(struct game_state_header) ||
			!game_state_image_read_header(&image, header))
		{
			/* (an empty file holds no saved game; anything else is damaged) */
			if (GetFileSize(file, NULL) && corrupted)
				*corrupted = TRUE;
			error(_error_silent, "no saved game in persistent storage");
		}
		else
		{
			game_state_header_terminate(header, header_size);
			if (game_state_port_image_check(&image, FALSE))
			{
				result = TRUE;
			}
			else
			{
				if (corrupted)
					*corrupted = TRUE;
				error(_error_silent, "checksum failed on persistent storage");
			}
		}
		CloseHandle(file);
	}

	return result;
}

void game_state_write_to_persistent_storage(
	void *buffer,
	unsigned long *header_checksum,
	long header_size,
	long buffer_size)
{
	HANDLE file;
	struct game_state_image_file image;

	/* port (HWI-076): the canonical image of the game state (buffer); its
	chunks' CRCs and its digest stand for the upstream header checksum */
	(void)buffer;
	(void)header_checksum;
	(void)header_size;
	(void)buffer_size;
	file = game_state_open_persistent_storage(NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		game_state_image_file_new(&image, &file);
		if (!game_state_port_image_write(&image) || !game_state_image_file_end(file))
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
				351,
				FALSE,
				csprintf(temporary, "failed to write to persistent storage (#%d)",
					GetLastError()));
			delete_persistent_storage();
		}
		CloseHandle(file);
	}

	return;
}

boolean game_state_read_from_persistent_storage(
	void *buffer,
	long buffer_size)
{
	HANDLE file;
	boolean result = FALSE;

	/* port (HWI-076): the canonical image, checked whole and only then
	taken into the game state (buffer); one that is not taken is deleted,
	as upstream deleted one it could not read */
	(void)buffer;
	(void)buffer_size;
	file = game_state_open_persistent_storage(NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		result = game_state_image_file_take(file, TRUE, "persistent storage");
		CloseHandle(file);
		if (!result)
			delete_persistent_storage();
	}

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
	struct game_state_image_file file;
	short result;

	game_state_image_file_new(&file, &handle);
	result = game_state_port_image_load(&file, check_header);
	if (result == _game_state_image_refused)
	{
		error(_error_silent, "couldn't read %s: %s", what,
			game_state_image_refusal_name(game_state_image_last_report()->refusal));
	}
	match_vassert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		156,
		result != _game_state_image_failed_while_taking,
		"game state has been corrupted (thank you, come again)");

	return result == _game_state_image_taken;
}
