/*
CACHE_FILES_WII.H

What the Wii cache layer (cache_files_wii.c, HWI-015B, HWI-016C) loaded of
the open staged map, for the driver's report, and why a map was refused.
Only C types: the drivers include it beside libogc or the host's C library.
*/

#ifndef __HALO_WII_CACHE_FILES_WII_H
#define __HALO_WII_CACHE_FILES_WII_H

struct wii_cache_load_report
{
	long segments;               /* in the staged file's table */
	long reads;                  /* segments read: the tag data, structure BSPs */
	long bsp_reads;              /* of which structure BSPs (the first, then each switch) */
	unsigned long bytes;         /* bytes read into the tag slot */
	unsigned long relocations;   /* pointer fields given the slot's base */
	unsigned long unstaged_reads; /* reads of bitmap pixels or sound samples, which fail */
	unsigned long milliseconds;  /* spent opening, checking and reading */
	unsigned long staging_digest; /* the staged file's (map_stage.py's table CRC) */
	/* what the tag slot holds: the tag data (range 0) and the structure BSP
	read last (range 1: each loads over the others' bytes) */
	long range_count;
	unsigned long range_offset[2];
	unsigned long range_size[2];
};

/* why the last map opened was refused (cache_file_open, cache_file_read) */
enum wii_map_error
{
	_wii_map_error_none,
	_wii_map_error_data_root,         /* the maps directory has no manifest.txt */
	_wii_map_error_not_staged,        /* no manifest line, or no file */
	_wii_map_error_stale,             /* another staging format version */
	_wii_map_error_damaged,           /* truncated, or a CRC fails */
	_wii_map_error_manifest_mismatch, /* not the staging the manifest lists */
	_wii_map_error_read,              /* the card failed a read */
	NUMBER_OF_WII_MAP_ERRORS
};

void wii_cache_load_report_get(struct wii_cache_load_report *report);

/* the last refusal's kind (_wii_map_error_none: the last open succeeded) and
its actionable text, which names the map and what to do */
short wii_cache_last_error(char *text, long size);
const char *wii_cache_error_name(short kind);
/* refusals since start-up */
unsigned long wii_cache_errors(void);

/* a test's maps directory (an Xbox path such as "d:\\other\\maps\\") in
place of the engine's (cache_files_map_directory); NULL restores it. Only
while no map is open. */
void wii_cache_files_set_map_directory(const char *directory);

#endif
