/*
CACHE_FILES_WII.C

The Wii build's replacement for source/cache/cache_files_windows.c (HWI-015B):
direct map loading from the SD card in place of the Xbox's hard-disk cache.

On the Xbox, a map is precached: copied from the DVD into one of six cache
files on the hard disk (about 0.9 GB, cache_files_open_cache_files), which
the Wii's card cannot hold, and then read from there by a reader thread.
Here a map is staged on the card by the host at import time
(tools/wii/map_stage.py) as "d:\maps\<name>.wmap": its tags and structure
BSPs already in the PowerPC's byte order, every pointer field holding its
target's offset in the tag slot, and the list of those fields. So:

- a map is precached when its staged file is on the card: nothing is
  copied, and precaching never runs;
- cache_file_open reads the staged file's table and the engine's cache
  header from it;
- cache_file_read serves the tag data and a BSP at their map file offsets,
  at once (the caller's completion flag is set before it returns), and then
  relocates what it read: each listed field gets the tag slot's run-time
  base added (ADR-015). Nothing reads a byte before that.
- the bitmaps' pixels and the sounds' samples are not staged (the Wii
  renderer and sound are unsupported): a read of them fails as the Xbox
  reader's failed reads do (_cache_file_read_failed), and is reported.

The vertex and index buffer registration is the upstream unit's (its
Direct3D calls report themselves unsupported). The interface is
cache_files.h's, unchanged. Compiled as an engine unit, from the engine's
headers only, so the same file builds the i686 host reference.
*/

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "cache/physical_memory_map.h"
#include "tag_files/files.h"
#include "tag_files/tag_files.h"
#include "rasterizer/rasterizer.h"

#include <xtl.h>

#include "cache_files_wii.h"
#include "wii_platform.h"

/* ---------- constants */

enum
{
	CACHE_FILE_SECTOR_SIZE = 512,
	WII_MAP_MAXIMUM_SEGMENTS = 8,
	WII_MAP_TABLE_BYTES = 0x20,
	WII_MAP_SEGMENT_BYTES = 0x20,
	WII_MAP_ORDER = 0x01020304,
	WII_MAP_SEGMENT_TAGS = 1,
	WII_MAP_SEGMENT_BSP = 2,
	TAG_SLOT_BYTES = 0x01600000,
};

/* ---------- structures */

struct cache_file_tag_header
{
	struct cache_file_tag_instance *tag_instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	D3DVertexBuffer *vertex_buffers;
	long index_buffer_count;
	D3DIndexBuffer *index_buffers;
	unsigned long signature;
};

struct cache_file_structure_bsp_header
{
	void *base_address;
	long vertex_buffer_count;
	D3DVertexBuffer *vertex_buffers;
	long lightmap_vertex_buffer_count;
	D3DVertexBuffer *lightmap_vertex_buffers;
	unsigned long signature;
};

/* (cache_files.c's) */
struct cache_file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	byte reservedC[4];
	long tag_data_offset;
	long tag_data_size;
	byte reserved18[8];
	char name[0x20];
	char build[0x20];
	short scenario_type;
	short pad62;
	unsigned long checksum;
	byte reserved68[0x794];
	unsigned long footer_signature;
};

typedef char verify_cache_file_header_size[sizeof(struct cache_file_header) == 0x800 ? 1 : -1];

/* a staged map's segment (tools/wii/map_stage.py): the native words of its
file's table */
struct wii_map_segment
{
	unsigned long kind;
	unsigned long map_offset;
	unsigned long size;
	unsigned long slot_offset;
	unsigned long file_offset;
	unsigned long relocation_count;
	unsigned long relocation_offset;
	unsigned long reserved;
};

typedef char verify_wii_map_segment_size[sizeof(struct wii_map_segment) == WII_MAP_SEGMENT_BYTES ? 1 : -1];

struct wii_cache_globals
{
	boolean initialized;
	boolean open;
	HANDLE file;
	char name[32];
	long segment_count;
	struct wii_map_segment segments[WII_MAP_MAXIMUM_SEGMENTS];
	struct wii_cache_load_report report;
	boolean unstaged_read_reported;
};

/* ---------- globals */

static struct wii_cache_globals wii_cache_globals;

/* ---------- private code */

static void wii_map_path(
	char const *map_name,
	char *path,
	long size)
{
	snprintf(path, size, "%s%s.wmap", cache_files_map_directory(), tag_name_strip_path(map_name));

	return;
}

static boolean wii_map_file_exists(
	char const *map_name)
{
	char path[256];
	HANDLE file;

	wii_map_path(map_name, path, sizeof(path));
	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	CloseHandle(file);

	return TRUE;
}

static boolean wii_map_read_at(
	unsigned long offset,
	void *buffer,
	unsigned long size)
{
	unsigned long read = 0;

	if (SetFilePointer(wii_cache_globals.file, (long)offset, NULL, FILE_BEGIN) != offset)
		return FALSE;
	while (size)
	{
		unsigned long chunk = size > 0x40000 ? 0x40000 : size;

		if (!ReadFile(wii_cache_globals.file, buffer, chunk, &read, NULL) || read != chunk)
			return FALSE;
		buffer = (byte *)buffer + chunk;
		size -= chunk;
	}

	return TRUE;
}

/* the staged file's table: its byte order the target's, its segments
inside the tag slot and not overlapping the table */
static boolean wii_map_read_table(
	struct cache_file_header *header)
{
	unsigned long table[WII_MAP_TABLE_BYTES / 4];
	long index;

	if (!wii_map_read_at(0, table, sizeof(table)) ||
		csmemcmp(table, "HWIMAP01", 8) != 0 ||
		table[2] != WII_MAP_ORDER ||
		table[3] < 1 || table[3] > WII_MAP_MAXIMUM_SEGMENTS ||
		table[4] != sizeof(struct cache_file_header))
	{
		return FALSE;
	}
	wii_cache_globals.segment_count = (long)table[3];
	if (!wii_map_read_at(WII_MAP_TABLE_BYTES, wii_cache_globals.segments,
			wii_cache_globals.segment_count * sizeof(struct wii_map_segment)) ||
		!wii_map_read_at(table[5], header, sizeof(*header)))
	{
		return FALSE;
	}
	for (index = 0; index < wii_cache_globals.segment_count; index++)
	{
		struct wii_map_segment const *segment = &wii_cache_globals.segments[index];

		if ((segment->kind != WII_MAP_SEGMENT_TAGS && segment->kind != WII_MAP_SEGMENT_BSP) ||
			segment->size > TAG_SLOT_BYTES ||
			segment->slot_offset > TAG_SLOT_BYTES - segment->size ||
			segment->relocation_count > segment->size / 4)
		{
			return FALSE;
		}
	}

	return TRUE;
}

static struct wii_map_segment const *wii_map_segment_at(
	long offset)
{
	long index;

	for (index = 0; index < wii_cache_globals.segment_count; index++)
	{
		if (wii_cache_globals.segments[index].map_offset == (unsigned long)offset)
			return &wii_cache_globals.segments[index];
	}

	return NULL;
}

/* each listed field of the segment just read gets the tag slot's base:
it held its target's offset in the slot */
static boolean wii_map_relocate(
	struct wii_map_segment const *segment)
{
	byte *slot = physical_memory_get_tag_cache_base_address();
	unsigned long base = (unsigned long)slot;
	unsigned long remaining = segment->relocation_count;
	unsigned long offset = segment->relocation_offset;
	unsigned long chunk_words[1024];

	while (remaining)
	{
		unsigned long count = remaining > NUMBEROF(chunk_words) ? NUMBEROF(chunk_words) : remaining;
		unsigned long index;

		if (!wii_map_read_at(offset, chunk_words, count * 4))
			return FALSE;
		for (index = 0; index < count; index++)
		{
			unsigned long field = chunk_words[index];

			if ((field & 3) || field < segment->slot_offset || field - segment->slot_offset > segment->size - 4)
				return FALSE;
			*(unsigned long *)(slot + field) += base;
		}
		wii_cache_globals.report.relocations += count;
		offset += count * 4;
		remaining -= count;
	}

	return TRUE;
}

/* ---------- public code */

void tags_header_register_vertex_and_index_buffers(
	struct cache_file_tag_header *header)
{
	long index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}
	for (index = 0; index < header->index_buffer_count; index++)
	{
		D3DIndexBuffer *index_buffer = &header->index_buffers[index];

		index_buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | 1;
	}

	return;
}

void tags_header_deregister_vertex_and_index_buffers(
	struct cache_file_tag_header *header)
{
	long index;

	for (index = 0; index < header->vertex_buffer_count; index++)
		IDirect3DVertexBuffer8_BlockUntilNotBusy(&header->vertex_buffers[index]);
	for (index = 0; index < header->index_buffer_count; index++)
		IDirect3DIndexBuffer8_BlockUntilNotBusy(&header->index_buffers[index]);

	return;
}

void structure_bsp_header_register_vertex_buffers(
	struct cache_file_structure_bsp_header *header)
{
	long index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}
	for (index = 0; index < header->lightmap_vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->lightmap_vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	return;
}

void structure_bsp_header_deregister_vertex_buffers(
	struct cache_file_structure_bsp_header *header)
{
	long index;

	rasterizer_globals.current_lock_operation = _rasterizer_lock_bsp_switch;
	for (index = 0; index < header->vertex_buffer_count; index++)
		IDirect3DVertexBuffer8_BlockUntilNotBusy(&header->vertex_buffers[index]);
	for (index = 0; index < header->lightmap_vertex_buffer_count; index++)
		IDirect3DVertexBuffer8_BlockUntilNotBusy(&header->lightmap_vertex_buffers[index]);
	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;

	return;
}

void cache_files_initialize(
	void)
{
	csmemset(&wii_cache_globals, 0, sizeof(wii_cache_globals));
	wii_cache_globals.file = INVALID_HANDLE_VALUE;
	wii_cache_globals.initialized = TRUE;

	return;
}

void cache_files_dispose(
	void)
{
	match_assert(__FILE__, __LINE__, !wii_cache_globals.open);
	wii_cache_globals.initialized = FALSE;

	return;
}

/* precaching: a staged map is on the card already */

void cache_files_precache_set_priority(
	boolean blocking)
{
	return;
}

boolean cache_files_precache_in_progress(
	void)
{
	return FALSE;
}

boolean cache_files_precache_is_copying_map(
	const char *map_name)
{
	return FALSE;
}

boolean cache_files_precache_map_loaded(
	const char *map_name)
{
	return map_name && map_name[0] && wii_map_file_exists(map_name);
}

boolean cache_files_precache_map_begin(
	const char *map_name,
	boolean copy_map)
{
	if (cache_files_precache_map_loaded(map_name))
		return TRUE;
	error(_error_silent, "couldn't find map '%s' staged on the card", map_name);
	if (copy_map)
		display_error_damaged_media();

	return FALSE;
}

void cache_files_precache_map_end(
	void)
{
	return;
}

void cache_files_precache_map_queue_end(
	void)
{
	return;
}

short cache_files_precache_map_status(
	real *progress)
{
	if (progress)
		*progress = 1.0f;

	return _cached_map_file_success;
}

void cache_file_promote_read(
	short request_index)
{
	return;
}

void cache_file_block_until_not_busy(
	void)
{
	return;
}

void cache_file_close(
	void)
{
	if (wii_cache_globals.open)
	{
		CloseHandle(wii_cache_globals.file);
		wii_cache_globals.file = INVALID_HANDLE_VALUE;
		wii_cache_globals.open = FALSE;
	}

	return;
}

boolean cache_file_open(
	const char *scenario_name,
	struct cache_file_header *header)
{
	char path[256];
	unsigned long started = system_milliseconds();

	match_assert(__FILE__, __LINE__, scenario_name && header && !wii_cache_globals.open);
	wii_map_path(scenario_name, path, sizeof(path));
	wii_cache_globals.file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (wii_cache_globals.file == INVALID_HANDLE_VALUE)
	{
		error(_error_silent, "the map '%s' is not staged on the card", scenario_name);
		return FALSE;
	}
	if (!wii_map_read_table(header))
	{
		error(_error_silent, "the staged map '%s' is not one this build reads (its table or byte order)", scenario_name);
		CloseHandle(wii_cache_globals.file);
		wii_cache_globals.file = INVALID_HANDLE_VALUE;
		return FALSE;
	}
	csmemset(&wii_cache_globals.report, 0, sizeof(wii_cache_globals.report));
	csstrncpy(wii_cache_globals.name, tag_name_strip_path(scenario_name), sizeof(wii_cache_globals.name) - 1);
	wii_cache_globals.name[sizeof(wii_cache_globals.name) - 1] = 0;
	wii_cache_globals.open = TRUE;
	wii_cache_globals.report.segments = wii_cache_globals.segment_count;
	wii_cache_globals.report.milliseconds += system_milliseconds() - started;

	return TRUE;
}

short cache_file_read(
	long tag_index,
	long offset,
	long size,
	void *buffer,
	boolean *completion_flag_reference,
	boolean blocking)
{
	struct wii_map_segment const *segment;
	unsigned long started = system_milliseconds();
	byte *slot = physical_memory_get_tag_cache_base_address();

	match_assert(__FILE__, __LINE__, buffer && completion_flag_reference);
	*completion_flag_reference = _cache_file_read_failed;
	if (!wii_cache_globals.open || offset < 0 || size < 0)
	{
		error(_error_silent, "cache file read of %08x bytes at %08x refused", size, offset);
		return NONE;
	}
	segment = wii_map_segment_at(offset);
	if (!segment)
	{
		/* bitmap pixels and sound samples: not staged */
		wii_cache_globals.report.unstaged_reads++;
		if (!wii_cache_globals.unstaged_read_reported)
		{
			wii_cache_globals.unstaged_read_reported = TRUE;
			wii_unsupported("content", "cache_file_read of bitmap pixels or sound samples (not staged)");
		}
		return NONE;
	}
	/* the read is the whole segment, rounded up to whole sectors as the
	Xbox reader's (what follows it in the slot is zero), to where it
	loads in the tag slot */
	if ((unsigned long)size < segment->size ||
		(unsigned long)size > ((segment->size + CACHE_FILE_SECTOR_SIZE - 1) & ~(unsigned long)(CACHE_FILE_SECTOR_SIZE - 1)) ||
		(byte *)buffer != slot + segment->slot_offset ||
		segment->slot_offset + (unsigned long)size > TAG_SLOT_BYTES)
	{
		error(_error_silent, "cache file read of %08x bytes at %08x does not match the staged map", size, offset);
		return NONE;
	}
	if (!wii_map_read_at(segment->file_offset, buffer, segment->size))
	{
		error(_error_silent, "the staged map '%s' could not be read", wii_cache_globals.name);
		return NONE;
	}
	csmemset((byte *)buffer + segment->size, 0, size - segment->size);
	if (!wii_map_relocate(segment))
	{
		error(_error_silent, "the staged map '%s' has a relocation outside what it loads", wii_cache_globals.name);
		return NONE;
	}
	wii_cache_globals.report.bytes += segment->size;
	wii_cache_globals.report.reads++;
	if (wii_cache_globals.report.range_count < (long)NUMBEROF(wii_cache_globals.report.range_offset))
	{
		wii_cache_globals.report.range_offset[wii_cache_globals.report.range_count] = segment->slot_offset;
		wii_cache_globals.report.range_size[wii_cache_globals.report.range_count] = segment->size;
		wii_cache_globals.report.range_count++;
	}
	wii_cache_globals.report.milliseconds += system_milliseconds() - started;
	*completion_flag_reference = TRUE;

	return 0;
}

void wii_cache_load_report_get(
	struct wii_cache_load_report *report)
{
	*report = wii_cache_globals.report;

	return;
}
