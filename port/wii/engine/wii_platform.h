/*
WII_PLATFORM.H

The Wii platform layer's own interfaces (HWI-015): the SD log, the
unsupported-entry-point register, the MEM2 arena the engine's memory comes
from (ADR-015), and the engine thread runtime. The SDK and C-runtime
interfaces the engine calls are declared by the XDK headers and
port/linux/src/platform.h, which these files implement on the Wii.

No declaration here uses an XDK type, so the driver can include it beside
libogc's headers.
*/

#ifndef __HALO_WII_PLATFORM_H
#define __HALO_WII_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* ---------- SD paths */

#define WII_ENGINE_ROOT "sd:/halo-wii-engine"
/* d:\ (the folder holding maps/) and the save root (z:\, u:\, t:\ ...) */
#define WII_ENGINE_DATA_ROOT WII_ENGINE_ROOT "/data"
#define WII_ENGINE_SAVE_ROOT WII_ENGINE_ROOT "/saves"

/* ---------- log (wii_log.c)

One file on the SD card, written a line at a time and flushed after each,
from any thread. Lines from the platform layer start "platform: ". */

int wii_log_open(const char *path);
void wii_log_close(void);
void wii_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* whether every write so far succeeded */
int wii_log_healthy(void);

/* ---------- unsupported entry points (wii_log.c)

Every SDK or port entry point the Wii build does not implement calls
wii_unsupported(subsystem, name) and then fails the call: it returns its
failure value (NULL, 0, FALSE, E_FAIL, INVALID_HANDLE_VALUE) or does
nothing, exactly as documented beside it. The first call of each name is
logged as "UNSUPPORTED <subsystem> <name>"; every call is counted. Nothing
unsupported reports success. */

void wii_unsupported(const char *subsystem, const char *name);
unsigned long wii_unsupported_calls(void);
unsigned long wii_unsupported_names(void);
/* "<subsystem>:<name>" of the index-th distinct name, in first-call order */
const char *wii_unsupported_name(unsigned long index);
/* forget the counts (a new engine cycle) */
void wii_unsupported_reset(void);

/* ---------- the engine's MEM2 arena (wii_memory.c, ADR-015)

wii_arena_open plans the engine's reservations in the MEM2 arena at its
run-time address: the game state (HALO_PORT_GAME_STATE_SIZE), the tag cache
(22 MiB), the sound cache (4 MiB) and a general region that serves
XPhysicalAlloc (contiguous memory: Direct3D resources, IO buffers), at the
top of the arena, leaving at least a reserve below it for libogc and the
heap. It lowers the arena's high bound below the plan, so nothing else can
allocate there, and wii_arena_close restores it. */

enum wii_arena_region
{
	_wii_arena_game_state,
	_wii_arena_tag_cache,
	_wii_arena_sound_cache,
	_wii_arena_general,
	NUMBER_OF_WII_ARENA_REGIONS
};

struct wii_arena_report
{
	/* the MEM2 arena's bounds: the plan lowers the high bound */
	uintptr_t arena_low_before, arena_low_after;
	uintptr_t arena_high_before, arena_high_during, arena_high_after;
	uintptr_t region_base[NUMBER_OF_WII_ARENA_REGIONS];
	size_t region_size[NUMBER_OF_WII_ARENA_REGIONS];
	size_t required, spare;
	/* XPhysicalAlloc from the general region */
	size_t general_in_use, general_peak, general_blocks;
	int failed_allocations;
};

/* 1 on success; the placement moves down by offset bytes (a multiple of
4 KiB) from the arena's high bound, so cycles can place the engine differently */
int wii_arena_open(size_t offset, size_t general_bytes, struct wii_arena_report *report);
/* 1 if every region was released and the arena bound restored */
int wii_arena_close(struct wii_arena_report *report);
void *wii_arena_region(enum wii_arena_region region, size_t *size);
void wii_arena_report_get(struct wii_arena_report *report);

/* ---------- engine threads (wii_kernel.c)

Every thread CreateThread starts clears FPSCR[NI] before the engine's
routine runs (ADR-018: each libogc thread has its own FPSCR). */

struct wii_thread_report
{
	unsigned long created;
	unsigned long fpscr_cleared;
	uint32_t last_fpscr_before, last_fpscr_after;
};

void wii_thread_report_get(struct wii_thread_report *report);

/* ---------- time (wii_kernel.c) */

/* microseconds since boot, from the time base */
unsigned long long wii_time_microseconds(void);

#endif
