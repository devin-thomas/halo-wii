/*
CACHE_FILES_WII.C

The Wii build's replacement for source/cache/cache_files_windows.c (HWI-015B,
HWI-016C): direct map loading from the SD card in place of the Xbox's
hard-disk cache.

On the Xbox, a map is precached: copied from the DVD into one of six cache
files on the hard disk (about 0.9 GB, cache_files_open_cache_files), which
the Wii's card cannot hold, and then read from there by a reader thread.
Here a map is staged on the card by the host at import time
(tools/wii/map_stage.py) as "d:\maps\<name>.wmap": its tags and every
structure BSP already in the PowerPC's byte order, every pointer field
holding its target's offset in the tag slot, and the list of those fields,
beside the directory's manifest.txt, which lists each staged map. So:

- a map is precached when its staged file is on the card: nothing is
  copied, and precaching never runs;
- cache_file_open checks the staged map against the manifest and itself
  before anything is read into the tag slot (below), and reads the engine's
  cache header from it;
- cache_file_read serves the tag data and each structure BSP at their map
  file offsets, at once (the caller's completion flag is set before it
  returns): it checks what it read against the segment's CRC and then
  relocates it: each listed field gets the tag slot's run-time base added
  (ADR-015). Nothing reads a byte before that. A campaign scenario's BSPs
  each load alone, over the others' bytes (scenario_switch_structure_bsp);
- the bitmaps' pixels and the sounds' samples are not staged (the Wii
  renderer and sound are unsupported): a read of them fails as the Xbox
  reader's failed reads do (_cache_file_read_failed), and is reported.
  HWI-074: once the Wii's sound has started, a map's sound samples come from
  its sound pack beside it, d:\maps\<name>.wsnd (port/wii/sound/sound_pack.c,
  tools/wii/sound_stage.py), opened with the map: a read the pack serves
  completes at once, checked; one it has not, or that fails its check, fails
  as before.

Load failures (HWI-016C) are recoverable: each is refused with an actionable
error before the engine relies on anything, written to the SD log
("MAP_ERROR") and the engine's error log, and kept for the screen
(wii_cache_last_error). Nothing on the card is changed, and the engine
stays able to load a map. In the order checked:
- the maps directory has no manifest.txt: the data root is wrong;
- the map is not staged: no manifest line, or no file;
- the file is of another staging format version (stale), or the manifest is;
- the file is damaged: not the length the manifest lists (truncated), a
  table or header whose CRC fails, a segment outside the file, or a
  segment's bytes failing their CRC as they are read;
- the file is not the staging the manifest lists (its staging digest).
A BSP refused as it is read leaves the engine to load the one before again
(scenario_switch_structure_bsp).

The vertex and index buffer registration is the upstream unit's (its
Direct3D calls report themselves unsupported). The interface is
cache_files.h's, unchanged. Compiled as an engine unit, from the engine's
headers only, so the same file builds the i686 host reference.
*/

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "cache/physical_memory_map.h"
#include "memory/crc.h"
#include "tag_files/files.h"
#include "tag_files/tag_files.h"
#include "rasterizer/rasterizer.h"

#include <xtl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_files_wii.h"
#include "wii_platform.h"
#include "../sound/wii_sound.h"

void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size);

/* ---------- constants */

enum
{
	CACHE_FILE_SECTOR_SIZE = 512,
	/* the tag data and at most MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO bsps */
	WII_MAP_MAXIMUM_SEGMENTS = 1 + 16,
	WII_MAP_TABLE_BYTES = 0x40,
	WII_MAP_SEGMENT_BYTES = 0x20,
	WII_MAP_ORDER = 0x01020304,
	WII_MAP_ORDER_SWAPPED = 0x04030201,
	WII_MAP_FORMAT_VERSION = 2,
	WII_MAP_SEGMENT_TAGS = 1,
	WII_MAP_SEGMENT_BSP = 2,
	WII_MAP_MANIFEST_BYTES = 0x4000,
	WII_MAP_ERROR_TEXT_BYTES = 384,
	TAG_SLOT_BYTES = 0x01600000,
};

#define WII_MAP_MAGIC_PREFIX "HWIMAP"
#define WII_MAP_MAGIC "HWIMAP02"
#define WII_MAP_MANIFEST_NAME "manifest.txt"
#define WII_MAP_MANIFEST_MAGIC "HWIMAP-MANIFEST"

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

/* a staged map's table (tools/wii/map_stage.py, format 2): native words */
struct wii_map_table
{
	char magic[8];
	unsigned long order;
	unsigned long segment_count;
	unsigned long header_bytes;
	unsigned long header_offset;
	unsigned long source_file_length;
	unsigned long file_bytes;
	unsigned long header_crc;
	unsigned long table_crc;
	unsigned long format_version;
	unsigned long reserved[5];
};

typedef char verify_wii_map_table_size[sizeof(struct wii_map_table) == WII_MAP_TABLE_BYTES ? 1 : -1];

/* a staged map's segment: the native words of its file's table */
struct wii_map_segment
{
	unsigned long kind;
	unsigned long map_offset;
	unsigned long size;
	unsigned long slot_offset;
	unsigned long file_offset;
	unsigned long relocation_count;
	unsigned long relocation_offset;
	unsigned long crc;
};

typedef char verify_wii_map_segment_size[sizeof(struct wii_map_segment) == WII_MAP_SEGMENT_BYTES ? 1 : -1];

/* the table and segments as they lie at the start of the file */
struct wii_map_table_image
{
	struct wii_map_table table;
	struct wii_map_segment segments[WII_MAP_MAXIMUM_SEGMENTS];
};

/* a map's line of the manifest */
struct wii_map_manifest_entry
{
	long format_version;
	unsigned long file_bytes;
	unsigned long staging_digest;
};

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
	/* (a test's maps directory in place of the engine's, wii_cache_files_set_map_directory) */
	char map_directory[128];
	short last_error;
	char last_error_text[WII_MAP_ERROR_TEXT_BYTES];
	unsigned long errors;
	byte manifest[WII_MAP_MANIFEST_BYTES];
};

/* ---------- globals */

static struct wii_cache_globals wii_cache_globals;

static char const *const wii_map_error_names[NUMBER_OF_WII_MAP_ERRORS] =
{
	"none",
	"data_root",
	"not_staged",
	"stale",
	"damaged",
	"manifest_mismatch",
	"read",
};

/* ---------- private code */

static char const *wii_map_directory(
	void)
{
	return wii_cache_globals.map_directory[0] ? wii_cache_globals.map_directory : cache_files_map_directory();
}

/* the maps directory as the card has it, for messages */
static void wii_map_directory_on_card(
	char *path,
	long size)
{
	platform_translate_path(wii_map_directory(), path, size);

	return;
}

static void wii_map_path(
	char const *map_name,
	char *path,
	long size)
{
	snprintf(path, size, "%s%s.wmap", wii_map_directory(), tag_name_strip_path(map_name));

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

/* a load refused: kept for the screen, written to the SD log and the
engine's error log */
static void wii_map_error(
	short kind,
	char const *map_name,
	char const *format,
	...)
{
	va_list arguments;
	char text[WII_MAP_ERROR_TEXT_BYTES - 64];

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	snprintf(wii_cache_globals.last_error_text, sizeof(wii_cache_globals.last_error_text), "map '%s': %s",
		tag_name_strip_path(map_name), text);
	wii_cache_globals.last_error = kind;
	wii_cache_globals.errors++;
	wii_log("MAP_ERROR kind=%s map=%s text=\"%s\"\n", wii_map_error_names[kind], tag_name_strip_path(map_name), text);
	error(_error_silent, "%s", wii_cache_globals.last_error_text);

	return;
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

static unsigned long wii_map_crc(
	void const *data,
	unsigned long size)
{
	unsigned long crc;

	crc_new(&crc);
	crc_checksum_buffer(&crc, data, (long)size);

	return crc;
}

static boolean wii_map_hex_word(
	char const *text,
	unsigned long *value)
{
	unsigned long result = 0;
	short index;

	for (index = 0; index < 8; index++)
	{
		char c = text[index];

		result <<= 4;
		if (c >= '0' && c <= '9')
			result |= (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f')
			result |= (unsigned long)(c - 'a' + 10);
		else
			return FALSE;
	}
	*value = result;

	return text[8] == ' ' || text[8] == 0;
}

/* the directory's manifest, read whole into the globals' buffer (NUL
terminated); FALSE: there is none */
static boolean wii_map_manifest_read(
	void)
{
	char path[256];
	HANDLE file;
	unsigned long read = 0;
	boolean result;

	snprintf(path, sizeof(path), "%s%s", wii_map_directory(), WII_MAP_MANIFEST_NAME);
	file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	result = ReadFile(file, wii_cache_globals.manifest, sizeof(wii_cache_globals.manifest) - 1, &read, NULL);
	CloseHandle(file);
	wii_cache_globals.manifest[result ? read : 0] = 0;

	return TRUE;
}

/* the manifest's line for map_name: 1 found, 0 none, -1 the manifest is of
another format version (its version in *version) */
static long wii_map_manifest_find(
	char const *map_name,
	struct wii_map_manifest_entry *entry,
	long *version)
{
	char const *line = (char const *)wii_cache_globals.manifest;
	unsigned long name_length = csstrlen(map_name);
	long manifest_version = 0;

	if (strncmp(line, WII_MAP_MANIFEST_MAGIC " ", sizeof(WII_MAP_MANIFEST_MAGIC)) != 0)
	{
		*version = 0;
		return -1;
	}
	manifest_version = strtol(line + sizeof(WII_MAP_MANIFEST_MAGIC), NULL, 10);
	*version = manifest_version;
	if (manifest_version != WII_MAP_FORMAT_VERSION)
		return -1;
	while ((line = strchr(line, '\n')) != NULL)
	{
		char *end;

		line++;
		if (strncmp(line, map_name, name_length) != 0 || line[name_length] != ' ')
			continue;
		line += name_length + 1;
		entry->format_version = strtol(line, &end, 10);
		if (end == line || *end != ' ')
			return 0;
		line = end + 1;
		entry->file_bytes = strtoul(line, &end, 10);
		if (end == line || *end != ' ' || !wii_map_hex_word(end + 1, &entry->staging_digest))
			return 0;

		return 1;
	}

	return 0;
}

/* the staged file's table, checked against itself, the file and the
manifest's line, before anything is read into the tag slot */
static boolean wii_map_check(
	char const *map_name,
	struct cache_file_header *header)
{
	struct wii_map_table_image image;
	struct wii_map_manifest_entry entry;
	unsigned long file_bytes = GetFileSize(wii_cache_globals.file, NULL);
	unsigned long table_bytes;
	unsigned long table_crc;
	long manifest_version = 0;
	long found;
	long index;

	csmemset(&image, 0, sizeof(image));
	if (file_bytes < WII_MAP_TABLE_BYTES || !wii_map_read_at(0, &image.table, WII_MAP_TABLE_BYTES))
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap is damaged: %lu bytes, shorter than its table; "
			"copy it again from map_stage.py's output", map_name, file_bytes);
		return FALSE;
	}
	if (csmemcmp(image.table.magic, WII_MAP_MAGIC_PREFIX, 6) != 0)
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap is not a staged map (no HWIMAP table); "
			"stage the map with tools/wii/map_stage.py", map_name);
		return FALSE;
	}
	if (csmemcmp(image.table.magic, WII_MAP_MAGIC, 8) != 0)
	{
		wii_map_error(_wii_map_error_stale, map_name, "%s.wmap was staged in format %.2s, and this build reads "
			"format %02d; stage the map again with this build's tools/wii/map_stage.py and copy the new %s.wmap "
			"and manifest.txt", map_name, image.table.magic + 6, WII_MAP_FORMAT_VERSION, map_name);
		return FALSE;
	}
	if (image.table.order == WII_MAP_ORDER_SWAPPED)
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap is staged for the other byte order (the Wii "
			"reads map_stage.py's big-endian <map>.wmap; <map>.le.wmap is the i686 host reference's)", map_name);
		return FALSE;
	}
	if (image.table.order != WII_MAP_ORDER || image.table.format_version != WII_MAP_FORMAT_VERSION)
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap's table is damaged; copy it again from "
			"map_stage.py's output", map_name);
		return FALSE;
	}

	/* the manifest's line */
	found = wii_map_manifest_find(map_name, &entry, &manifest_version);
	if (found < 0)
	{
		wii_map_error(_wii_map_error_stale, map_name, "manifest.txt is of staging format %ld, and this build "
			"reads format %d; copy the manifest.txt map_stage.py wrote with the maps", manifest_version,
			WII_MAP_FORMAT_VERSION);
		return FALSE;
	}
	if (!found)
	{
		wii_map_error(_wii_map_error_manifest_mismatch, map_name, "%s.wmap is on the card but manifest.txt "
			"has no line for it; copy the manifest.txt of the staging the map came from", map_name);
		return FALSE;
	}
	if (entry.format_version != WII_MAP_FORMAT_VERSION)
	{
		wii_map_error(_wii_map_error_stale, map_name, "manifest.txt lists %s in staging format %ld, and this "
			"build reads format %d; stage the map again", map_name, entry.format_version, WII_MAP_FORMAT_VERSION);
		return FALSE;
	}
	if (file_bytes != entry.file_bytes || image.table.file_bytes != file_bytes)
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap is damaged: %lu bytes, where manifest.txt "
			"lists %lu (truncated or incomplete copy); copy it again from map_stage.py's output", map_name,
			file_bytes, entry.file_bytes);
		return FALSE;
	}

	/* the table's own CRC, the segments inside the file and the tag slot */
	if (image.table.segment_count < 1 || image.table.segment_count > WII_MAP_MAXIMUM_SEGMENTS ||
		image.table.header_bytes != sizeof(struct cache_file_header))
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap's table is damaged (%lu segments); copy it again "
			"from map_stage.py's output", map_name, image.table.segment_count);
		return FALSE;
	}
	table_bytes = WII_MAP_TABLE_BYTES + image.table.segment_count * WII_MAP_SEGMENT_BYTES;
	if (!wii_map_read_at(WII_MAP_TABLE_BYTES, image.segments, image.table.segment_count * WII_MAP_SEGMENT_BYTES))
	{
		wii_map_error(_wii_map_error_read, map_name, "%s.wmap could not be read; check the card", map_name);
		return FALSE;
	}
	table_crc = image.table.table_crc;
	image.table.table_crc = 0;
	if (wii_map_crc(&image, table_bytes) != table_crc)
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap's table fails its CRC; copy it again from "
			"map_stage.py's output", map_name);
		return FALSE;
	}
	if (table_crc != entry.staging_digest)
	{
		wii_map_error(_wii_map_error_manifest_mismatch, map_name, "%s.wmap is staging %08lx, but manifest.txt "
			"lists staging %08lx for it; copy %s.wmap and manifest.txt from one staging", map_name, table_crc,
			entry.staging_digest, map_name);
		return FALSE;
	}
	for (index = 0; index < (long)image.table.segment_count; index++)
	{
		struct wii_map_segment const *segment = &image.segments[index];

		if ((segment->kind != WII_MAP_SEGMENT_TAGS && segment->kind != WII_MAP_SEGMENT_BSP) ||
			segment->size > TAG_SLOT_BYTES ||
			segment->slot_offset > TAG_SLOT_BYTES - segment->size ||
			segment->file_offset > file_bytes || segment->size > file_bytes - segment->file_offset ||
			segment->relocation_count > segment->size / 4 ||
			segment->relocation_offset > file_bytes ||
			segment->relocation_count > (file_bytes - segment->relocation_offset) / 4)
		{
			wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap's segment %ld lies outside the file or the "
				"tag slot; copy it again from map_stage.py's output", map_name, index);
			return FALSE;
		}
	}
	if (image.table.header_offset > file_bytes || sizeof(*header) > file_bytes - image.table.header_offset ||
		!wii_map_read_at(image.table.header_offset, header, sizeof(*header)) ||
		wii_map_crc(header, sizeof(*header)) != image.table.header_crc)
	{
		wii_map_error(_wii_map_error_damaged, map_name, "%s.wmap's cache header fails its CRC; copy it again "
			"from map_stage.py's output", map_name);
		return FALSE;
	}
	wii_cache_globals.segment_count = (long)image.table.segment_count;
	csmemcpy(wii_cache_globals.segments, image.segments, sizeof(image.segments));
	wii_cache_globals.report.staging_digest = table_crc;

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

/* the segment just read into the slot: its bytes and then its relocation
list must give its CRC; then each listed field gets the tag slot's base (it
held its target's offset in the slot) */
static boolean wii_map_relocate(
	struct wii_map_segment const *segment)
{
	byte *slot = physical_memory_get_tag_cache_base_address();
	unsigned long base = (unsigned long)slot;
	unsigned long chunk_words[1024];
	unsigned long crc;
	short pass;

	crc_new(&crc);
	crc_checksum_buffer(&crc, slot + segment->slot_offset, (long)segment->size);
	for (pass = 0; pass < 2; pass++)
	{
		unsigned long remaining = segment->relocation_count;
		unsigned long offset = segment->relocation_offset;

		while (remaining)
		{
			unsigned long count = remaining > NUMBEROF(chunk_words) ? NUMBEROF(chunk_words) : remaining;
			unsigned long index;

			if (!wii_map_read_at(offset, chunk_words, count * 4))
				return FALSE;
			if (pass == 0)
			{
				crc_checksum_buffer(&crc, chunk_words, (long)(count * 4));
			}
			else
			{
				for (index = 0; index < count; index++)
				{
					unsigned long field = chunk_words[index];

					if ((field & 3) || field < segment->slot_offset || field - segment->slot_offset > segment->size - 4)
						return FALSE;
					*(unsigned long *)(slot + field) += base;
				}
				wii_cache_globals.report.relocations += count;
			}
			offset += count * 4;
			remaining -= count;
		}
		if (pass == 0 && crc != segment->crc)
			return FALSE;
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
	/* port (HWI-016C): a map that is not staged is refused by
	cache_file_open as the engine loads it, with an actionable error the
	game recovers from, rather than by precaching, which the engine's
	blocking precache (game_precache_new_map) treats as fatal: there is
	nothing to copy, so precaching always succeeds */
	if (!cache_files_precache_map_loaded(map_name))
		error(_error_silent, "couldn't find map '%s' staged on the card", map_name);

	return TRUE;
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
		/* HWI-074 */
		wii_sound_pack_close();
	}

	return;
}

boolean cache_file_open(
	const char *scenario_name,
	struct cache_file_header *header)
{
	char path[256];
	char const *name = tag_name_strip_path(scenario_name);
	unsigned long started = system_milliseconds();

	match_assert(__FILE__, __LINE__, scenario_name && header && !wii_cache_globals.open);
	wii_cache_globals.last_error = _wii_map_error_none;
	wii_cache_globals.last_error_text[0] = 0;
	csmemset(&wii_cache_globals.report, 0, sizeof(wii_cache_globals.report));
	if (!wii_map_manifest_read())
	{
		char directory[256];

		wii_map_directory_on_card(directory, sizeof(directory));
		wii_map_error(_wii_map_error_data_root, name, "no staged maps at %s (its manifest.txt is missing): the "
			"data root is wrong. Copy map_stage.py's output (the .wmap files and manifest.txt) to the maps "
			"folder of the data root, " WII_ENGINE_DATA_ROOT "/maps/", directory);
		return FALSE;
	}
	wii_map_path(scenario_name, path, sizeof(path));
	wii_cache_globals.file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (wii_cache_globals.file == INVALID_HANDLE_VALUE)
	{
		struct wii_map_manifest_entry entry;
		long version;

		if (wii_map_manifest_find(name, &entry, &version) > 0)
		{
			wii_map_error(_wii_map_error_not_staged, name, "manifest.txt lists %s, but %s.wmap is not on the card; "
				"copy it from map_stage.py's output", name, name);
		}
		else
		{
			wii_map_error(_wii_map_error_not_staged, name, "%s is not staged (manifest.txt has no line for it and "
				"%s.wmap is not on the card); stage it with tools/wii/map_stage.py and copy %s.wmap and manifest.txt",
				name, name, name);
		}
		return FALSE;
	}
	if (!wii_map_check(name, header))
	{
		CloseHandle(wii_cache_globals.file);
		wii_cache_globals.file = INVALID_HANDLE_VALUE;
		return FALSE;
	}
	csstrncpy(wii_cache_globals.name, name, sizeof(wii_cache_globals.name) - 1);
	wii_cache_globals.name[sizeof(wii_cache_globals.name) - 1] = 0;
	wii_cache_globals.open = TRUE;
	wii_cache_globals.report.segments = wii_cache_globals.segment_count;
	wii_cache_globals.report.milliseconds += system_milliseconds() - started;
	/* HWI-074: the map's sound samples, once the Wii's sound has started */
	if (wii_sound_started())
	{
		char pack[256];
		char pack_on_card[256];

		snprintf(pack, sizeof(pack), "%s%s.wsnd", wii_map_directory(), name);
		platform_translate_path(pack, pack_on_card, sizeof(pack_on_card));
		wii_sound_pack_open(pack_on_card, name);
	}

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
	long range;

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
		/* HWI-074: sound samples, from the map's sound pack */
		int served = wii_sound_pack_read(offset, size, buffer);

		if (served > 0)
		{
			*completion_flag_reference = TRUE;
			return 0;
		}
		if (served < 0)
			return NONE;
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
		wii_map_error(_wii_map_error_read, wii_cache_globals.name, "%s.wmap could not be read (%lu bytes at %lu); "
			"check the card", wii_cache_globals.name, segment->size, segment->file_offset);
		return NONE;
	}
	csmemset((byte *)buffer + segment->size, 0, size - segment->size);
	if (!wii_map_relocate(segment))
	{
		wii_map_error(_wii_map_error_damaged, wii_cache_globals.name, "%s.wmap is damaged: its %s at %lu fails "
			"its CRC or its relocations; copy it again from map_stage.py's output", wii_cache_globals.name,
			segment->kind == WII_MAP_SEGMENT_TAGS ? "tag data" : "structure bsp", segment->file_offset);
		return NONE;
	}
	wii_cache_globals.report.bytes += segment->size;
	wii_cache_globals.report.reads++;
	if (segment->kind == WII_MAP_SEGMENT_BSP)
		wii_cache_globals.report.bsp_reads++;
	/* what the slot holds: the tag data, then the bsp loaded last (each
	bsp loads over the others' bytes) */
	range = segment->kind == WII_MAP_SEGMENT_TAGS ? 0 : 1;
	wii_cache_globals.report.range_offset[range] = segment->slot_offset;
	wii_cache_globals.report.range_size[range] = segment->size;
	if (wii_cache_globals.report.range_count < range + 1)
		wii_cache_globals.report.range_count = range + 1;
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

short wii_cache_last_error(
	char *text,
	long size)
{
	if (text && size > 0)
	{
		csstrncpy(text, wii_cache_globals.last_error_text, (unsigned long)size - 1);
		text[size - 1] = 0;
	}

	return wii_cache_globals.last_error;
}

char const *wii_cache_error_name(
	short kind)
{
	return kind >= 0 && kind < NUMBER_OF_WII_MAP_ERRORS ? wii_map_error_names[kind] : "unknown";
}

unsigned long wii_cache_errors(
	void)
{
	return wii_cache_globals.errors;
}

void wii_cache_files_set_map_directory(
	char const *directory)
{
	match_assert(__FILE__, __LINE__, !wii_cache_globals.open);
	wii_cache_globals.map_directory[0] = 0;
	if (directory)
	{
		csstrncpy(wii_cache_globals.map_directory, directory, sizeof(wii_cache_globals.map_directory) - 1);
		wii_cache_globals.map_directory[sizeof(wii_cache_globals.map_directory) - 1] = 0;
	}

	return;
}
