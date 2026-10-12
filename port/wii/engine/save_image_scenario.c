/*
SAVE_IMAGE_SCENARIO.C

(HWI-076) The real-map run's view of saved games (save_image_scenario.h).
Engine side: it reads the canonical image's reports (game_state_image.h) and
reaches files as the engine does (the Xbox file API, xbox_files.c).

- Each save, load and check is reported with its image's sizes, chunks, the
  most the image code took from the heap at once, its pointers and the
  SHA-256 of its payload, and the CRC of the game state's room after it.
  A load is also checked twice: the game state it leaves is the one the
  image was saved from (the CRC that save reported), and writing the game
  state again gives the same image (its SHA-256).
- An image test (image_test=<tick>) saves a core after that tick, checks it,
  and loads damaged copies of it through game_state_read_core, the core
  load's own path: each must be refused for its reason, and the game state's
  room must be as it was, byte for byte. A refused image changes nothing,
  so the run's ticks go on as without the test.
*/

/* ---------- headers */

#include "cseries.h"
#include "memory/crc.h"
#include "saved games/game_state.h"
#include "saved games/game_state_image.h"
#include "cache/physical_memory_map.h"
#include <xtl.h>

#include "save_image_scenario.h"

/* ---------- constants */

enum
{
	SAVED_IMAGES = 32,
	COPY_BUFFER_BYTES = 4096,

	/* game_state_image.c's header and first chunk */
	IMAGE_VERSION_AT = 4,
	IMAGE_FLAGS_AT = 12,
	IMAGE_BYTE_ORDER_AT = 16,
	IMAGE_PAYLOAD_BYTES_AT = 32,
	IMAGE_ALLOCATION_CHECKSUM_AT = 60,
	IMAGE_MAP_NAME_AT = 72,
	IMAGE_BUILD_NUMBER_AT = 328,
	IMAGE_CHECKSUM_AT = 508,
	IMAGE_FIRST_CHUNK_CHECKSUM_AT = GAME_STATE_IMAGE_HEADER_BYTES + 12,
	IMAGE_FIRST_PAYLOAD_AT = GAME_STATE_IMAGE_HEADER_BYTES + GAME_STATE_IMAGE_CHUNK_HEADER_BYTES,
};

#define TEST_IMAGE "hwi076-image.bin"
#define TEST_DAMAGED "hwi076-damaged.bin"
#define TEST_IMAGE_PATH "d:\\core\\" TEST_IMAGE
#define TEST_DAMAGED_PATH "d:\\core\\" TEST_DAMAGED

enum damage
{
	_damage_flip_payload,
	_damage_flip_payload_checksum_fixed,
	_damage_flip_header,
	_damage_truncated_half,
	_damage_truncated_last_byte,
	_damage_empty,
	_damage_wrong_map,
	_damage_wrong_build,
	_damage_wrong_version,
	_damage_incomplete,
	_damage_wrong_byte_order,
	_damage_wrong_layout,

	NUMBER_OF_DAMAGES
};

struct damage_case
{
	char const *name;
	char const *refusal; /* NULL: any refusal of the payload */
};

static struct damage_case const damage_cases[NUMBER_OF_DAMAGES] =
{
	{ "flip_payload", "chunk_checksum" },
	{ "flip_payload_checksum_fixed", NULL },
	{ "flip_header", "header_checksum" },
	{ "truncated_half", "read" },
	{ "truncated_last_byte", "read" },
	{ "empty", "read" },
	{ "wrong_map", "map" },
	{ "wrong_build", "build" },
	{ "wrong_version", "version" },
	{ "incomplete", "incomplete" },
	{ "wrong_byte_order", "byte_order" },
	{ "wrong_layout", "layout" },
};

/* ---------- globals */

static struct
{
	void (*observer)(const struct save_image_event *event);
	long tick;
	long test_tick;
	boolean testing;
	char const *test_case;
	/* the images saved (their SHA-256's first word and the game state's CRC) */
	unsigned long saved_sha[SAVED_IMAGES][8];
	unsigned long saved_state[SAVED_IMAGES];
	long saved_count;
} save_image_globals;

static byte copy_buffer[COPY_BUFFER_BYTES];

/* ---------- private code */

static unsigned long game_state_room_crc(
	void)
{
	unsigned long crc;

	crc_new(&crc);
	crc_checksum_buffer(&crc, physical_memory_get_game_state_base_address(), HALO_PORT_GAME_STATE_SIZE);

	return crc;
}

static void put_le32(
	byte *bytes,
	unsigned long value)
{
	bytes[0] = (byte)value;
	bytes[1] = (byte)(value >> 8);
	bytes[2] = (byte)(value >> 16);
	bytes[3] = (byte)(value >> 24);
}

static unsigned long get_le32(
	byte const *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static void event_from_report(
	struct save_image_event *event,
	int kind,
	struct game_state_image_report const *report)
{
	csmemset(event, 0, sizeof(*event));
	event->kind = kind;
	event->tick = save_image_globals.tick;
	event->test_case = save_image_globals.test_case;
	event->intact = -1;
	event->expected = -1;
	event->same_as_saved = -1;
	event->reencoded_same = -1;
	event->refusal = game_state_image_refusal_name(report->refusal);
	event->refusal_offset = report->refusal_offset;
	event->state_bytes = report->state_bytes;
	event->file_bytes = report->file_bytes;
	event->payload_bytes = report->payload_bytes;
	event->chunks = report->chunks;
	event->chunk_bytes = report->chunk_bytes;
	event->peak_bytes = report->peak_bytes;
	event->pointers = report->pointers;
	event->tag_pointers = report->tag_pointers;
	event->references = report->references;
	event->script_threads = report->script_threads;
	event->foreign_pointers = report->foreign_pointers;
	event->address_like_words = report->address_like_words;
	event->address_like_dead_words = report->address_like_dead_words;
	event->first_address_like_offset = report->first_address_like_offset;
	csmemcpy(event->sha, report->digest, sizeof(event->sha));
	event->passes = (int)report->passes;
}

/* game_state_image.c's observer */
static void save_image_observer(
	short kind,
	struct game_state_image_report const *report)
{
	struct save_image_event event;

	if (!save_image_globals.observer)
		return;
	switch (kind)
	{
	case 0:
		event_from_report(&event, SAVE_IMAGE_EVENT_SAVE, report);
		event.result = report->refusal ? 1 : 0;
		if (!report->refusal)
		{
			long slot = save_image_globals.saved_count++ % SAVED_IMAGES;

			event.state_digest = game_state_room_crc();
			csmemcpy(save_image_globals.saved_sha[slot], report->digest, sizeof(report->digest));
			save_image_globals.saved_state[slot] = event.state_digest;
		}
		break;
	case 1:
	case 2:
		event_from_report(&event, kind == 1 ? SAVE_IMAGE_EVENT_LOAD : SAVE_IMAGE_EVENT_CHECK, report);
		event.result = report->refusal ? 1 : 0;
		if (kind == 1 && report->refusal && report->passes == 2)
			event.result = 2;
		if (kind == 1 && !report->refusal)
		{
			unsigned long again[8];
			long index;

			event.state_digest = game_state_room_crc();
			for (index = 0; index < save_image_globals.saved_count && index < SAVED_IMAGES; index++)
			{
				if (!csmemcmp(save_image_globals.saved_sha[index], report->digest, sizeof(report->digest)))
					event.same_as_saved = save_image_globals.saved_state[index] == event.state_digest;
			}
			event.reencoded_same = game_state_port_image_digest(again) &&
				!csmemcmp(again, event.sha, sizeof(again));
		}
		break;
	default:
		csmemset(&event, 0, sizeof(event));
		event.kind = SAVE_IMAGE_EVENT_BEGIN;
		event.tick = save_image_globals.tick;
		break;
	}
	save_image_globals.observer(&event);
}

static boolean file_read(
	void *context,
	void *buffer,
	long size)
{
	unsigned long transferred;

	return ReadFile(*(HANDLE *)context, buffer, size, &transferred, NULL) && (long)transferred == size;
}

static boolean file_seek(
	void *context,
	long offset)
{
	return SetFilePointer(*(HANDLE *)context, offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
}

/* a damaged copy of the test image */
static boolean make_damaged_copy(
	enum damage damage)
{
	HANDLE from = CreateFileA(TEST_IMAGE_PATH, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	HANDLE to = CreateFileA(TEST_DAMAGED_PATH, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	byte header[GAME_STATE_IMAGE_HEADER_BYTES];
	unsigned long size;
	unsigned long length;
	unsigned long first_chunk;
	unsigned long flip_at;
	unsigned long position;
	unsigned long transferred;
	unsigned long chunk_crc;
	boolean success = FALSE;

	if (from == INVALID_HANDLE_VALUE || to == INVALID_HANDLE_VALUE)
		goto done;
	size = GetFileSize(from, NULL);
	if (size < IMAGE_FIRST_PAYLOAD_AT + 8 ||
		!ReadFile(from, header, sizeof(header), &transferred, NULL) || transferred != sizeof(header))
	{
		goto done;
	}
	first_chunk = get_le32(header + IMAGE_PAYLOAD_BYTES_AT);
	if (first_chunk > GAME_STATE_IMAGE_CHUNK_BYTES)
		first_chunk = GAME_STATE_IMAGE_CHUNK_BYTES;
	flip_at = IMAGE_FIRST_PAYLOAD_AT + first_chunk / 2;
	length = size;
	switch (damage)
	{
	case _damage_truncated_half:
		length = size / 2;
		break;
	case _damage_truncated_last_byte:
		length = size - 1;
		break;
	case _damage_empty:
		length = 0;
		break;
	case _damage_flip_header:
		header[IMAGE_MAP_NAME_AT + 1] ^= 0x01;
		break;
	case _damage_wrong_map:
		csstrcpy((char *)header + IMAGE_MAP_NAME_AT, "levels\\b30\\b30");
		break;
	case _damage_wrong_build:
		header[IMAGE_BUILD_NUMBER_AT] ^= 0x01;
		break;
	case _damage_wrong_version:
		put_le32(header + IMAGE_VERSION_AT, GAME_STATE_IMAGE_VERSION + 1);
		break;
	case _damage_incomplete:
		put_le32(header + IMAGE_FLAGS_AT, get_le32(header + IMAGE_FLAGS_AT) & ~1UL);
		break;
	case _damage_wrong_byte_order:
		put_le32(header + IMAGE_BYTE_ORDER_AT, get_le32(header + IMAGE_BYTE_ORDER_AT) ^ 3UL);
		break;
	case _damage_wrong_layout:
		put_le32(header + IMAGE_ALLOCATION_CHECKSUM_AT, get_le32(header + IMAGE_ALLOCATION_CHECKSUM_AT) ^ 1UL);
		break;
	default:
		break;
	}
	/* (a header edited as a writer would: its CRC made again; the flipped
	byte's is not) */
	if (damage >= _damage_wrong_map)
	{
		unsigned long crc;

		crc_new(&crc);
		crc_checksum_buffer(&crc, header, IMAGE_CHECKSUM_AT);
		put_le32(header + IMAGE_CHECKSUM_AT, crc);
	}
	crc_new(&chunk_crc);
	for (position = 0; position < length; )
	{
		unsigned long take = length - position;
		byte *bytes;

		if (position < sizeof(header))
		{
			bytes = header + position;
			if (take > sizeof(header) - position)
				take = sizeof(header) - position;
		}
		else
		{
			if (take > COPY_BUFFER_BYTES)
				take = COPY_BUFFER_BYTES;
			if (!ReadFile(from, copy_buffer, take, &transferred, NULL) || transferred != take)
				goto done;
			bytes = copy_buffer;
			if ((damage == _damage_flip_payload || damage == _damage_flip_payload_checksum_fixed) &&
				flip_at >= position && flip_at < position + take)
			{
				bytes[flip_at - position] ^= 0x10;
			}
			/* the first chunk's payload, for its CRC made again */
			if (position < IMAGE_FIRST_PAYLOAD_AT + first_chunk && position + take > IMAGE_FIRST_PAYLOAD_AT)
			{
				unsigned long first = position < IMAGE_FIRST_PAYLOAD_AT ? IMAGE_FIRST_PAYLOAD_AT - position : 0;
				unsigned long last = position + take > IMAGE_FIRST_PAYLOAD_AT + first_chunk ?
					IMAGE_FIRST_PAYLOAD_AT + first_chunk - position : take;

				crc_checksum_buffer(&chunk_crc, bytes + first, (long)(last - first));
			}
		}
		if (!WriteFile(to, bytes, take, &transferred, NULL) || transferred != take)
			goto done;
		position += take;
	}
	if (damage == _damage_flip_payload_checksum_fixed)
	{
		byte bytes[4];

		put_le32(bytes, chunk_crc);
		if (SetFilePointer(to, IMAGE_FIRST_CHUNK_CHECKSUM_AT, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
			!WriteFile(to, bytes, 4, &transferred, NULL) || transferred != 4)
		{
			goto done;
		}
	}
	success = TRUE;

done:
	if (from != INVALID_HANDLE_VALUE)
		CloseHandle(from);
	if (to != INVALID_HANDLE_VALUE)
		CloseHandle(to);

	return success;
}

/* the image test after this tick */
static void image_test(
	void)
{
	struct save_image_event summary;
	struct game_state_image_file file;
	HANDLE handle;
	long damage;
	boolean valid = FALSE;

	csmemset(&summary, 0, sizeof(summary));
	save_image_globals.testing = TRUE;
	save_image_globals.test_case = "original";
	game_state_save_core(TEST_IMAGE);
	/* the undamaged image is valid: checked whole, not taken */
	handle = CreateFileA(TEST_IMAGE_PATH, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle != INVALID_HANDLE_VALUE)
	{
		file.context = &handle;
		file.read = file_read;
		file.write = NULL;
		file.seek = file_seek;
		valid = game_state_port_image_check(&file, TRUE);
		CloseHandle(handle);
	}
	for (damage = 0; damage < NUMBER_OF_DAMAGES; damage++)
	{
		struct save_image_event event;
		unsigned long before;
		unsigned long after;
		boolean taken;
		struct game_state_image_report const *report = game_state_image_last_report();

		save_image_globals.test_case = damage_cases[damage].name;
		if (!make_damaged_copy((enum damage)damage))
		{
			csmemset(&event, 0, sizeof(event));
			event.kind = SAVE_IMAGE_EVENT_TEST;
			event.tick = save_image_globals.tick;
			event.test_case = damage_cases[damage].name;
			event.refusal = "copy_failed";
			event.result = 3;
			save_image_globals.observer(&event);
			summary.cases++;
			continue;
		}
		before = game_state_room_crc();
		taken = game_state_read_core(TEST_DAMAGED, NULL, 0);
		after = game_state_room_crc();
		event_from_report(&event, SAVE_IMAGE_EVENT_TEST, report);
		event.result = taken ? 0 : 1;
		event.state_digest = after;
		event.intact = before == after;
		event.expected = !taken && report->refusal &&
			(!damage_cases[damage].refusal || !csstrcmp(event.refusal, damage_cases[damage].refusal));
		save_image_globals.observer(&event);
		summary.cases++;
		summary.cases_expected += event.expected == 1;
		summary.cases_intact += event.intact == 1;
	}
	summary.kind = SAVE_IMAGE_EVENT_SUMMARY;
	summary.tick = save_image_globals.tick;
	summary.test_case = "all";
	summary.result = valid ? 0 : 1;
	summary.refusal = "none";
	save_image_globals.observer(&summary);
	save_image_globals.test_case = NULL;
	save_image_globals.testing = FALSE;
}

/* ---------- public code */

void save_image_scenario_set_observer(
	void (*observer)(const struct save_image_event *event))
{
	save_image_globals.observer = observer;
	game_state_image_set_observer(observer ? save_image_observer : NULL);
}

void save_image_scenario_set_test(
	long tick)
{
	save_image_globals.test_tick = tick;
}

void save_image_scenario_after_tick(
	long tick)
{
	save_image_globals.tick = tick;
	if (save_image_globals.test_tick && tick == save_image_globals.test_tick && save_image_globals.observer)
		image_test();
}
