/*
CACHE_FILES_WII.H

What the Wii cache layer (cache_files_wii.c, HWI-015B) loaded of the open
staged map, for the driver's report. Only C types: the drivers include it
beside libogc or the host's C library.
*/

#ifndef __HALO_WII_CACHE_FILES_WII_H
#define __HALO_WII_CACHE_FILES_WII_H

struct wii_cache_load_report
{
	long segments;               /* in the staged file's table */
	long reads;                  /* segments read: the tag data, a structure BSP */
	unsigned long bytes;         /* bytes read into the tag slot */
	unsigned long relocations;   /* pointer fields given the slot's base */
	unsigned long unstaged_reads; /* reads of bitmap pixels or sound samples, which fail */
	unsigned long milliseconds;  /* spent opening and reading */
	/* where in the tag slot the segments read lie (the tag data, a BSP) */
	long range_count;
	unsigned long range_offset[8];
	unsigned long range_size[8];
};

void wii_cache_load_report_get(struct wii_cache_load_report *report);

#endif
