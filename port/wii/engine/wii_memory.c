/*
WII_MEMORY.C

The engine's memory on the Wii (HWI-015, ADR-015): its MEM2 arena plan, Xbox
contiguous memory (XPhysicalAlloc) and page protection.

On the Xbox the game asks for its game state and tag cache at fixed physical
addresses (source/cache/physical_memory_map.c). ADR-016 measured that those
addresses lie outside the Wii's usable memory, and ADR-015 places both in
MEM2 at bases chosen at run time instead: wii_arena_open plans the game
state, the tag cache, the sound cache and a general region with the shared
arena planner (tools/wii/cache_arena_plan.c) at the arena's actual address,
and lowers the MEM2 arena's high bound below the plan (the plan sits at the
top of MEM2, below IOS), so neither libogc nor the heap, which grows
upwards from the bottom, can allocate there until wii_arena_close raises it
again.
port/wii/engine/physical_memory_map_wii.c hands the regions to the engine.

XPhysicalAlloc serves the general region page by page, top-down first fit as
the Xbox kernel does. A request for a fixed physical address cannot be
honoured (there is no such address on the Wii) and fails.

The Wii build maps no pages through the MMU, so page protection is recorded
(XQueryMemoryProtect reads it back, as the engine's physical_memory_verify
expects) but not enforced; the first protection change says so in the log.

Custom Edition maps are linked to a fixed window (0x40440000) that the Wii
does not have: halo_custom_edition_tag_cache() is NULL, which the engine
treats as "Custom Edition maps cannot run", and the first request reports it
as unsupported. Running them needs the ADR-015 relocation applied to their
tags too (remaining HWI-015 work).
*/

#include "platform.h"
#include "wii_platform.h"
#include "../../../tools/wii/cache_arena_plan.h"
#include "../../linux/include/halo_port_capacity.h"

#include <string.h>

extern void *SYS_GetArena2Lo(void);
extern void *SYS_GetArena2Hi(void);
extern void SYS_SetArena2Hi(void *address);

#define PAGE_SIZE_BYTES 0x1000UL
/* the upstream sizes (cache/physical_memory_map.[ch]) */
#define WII_TAG_CACHE_BYTES 0x1600000UL
#define WII_SOUND_CACHE_BYTES 0x400000UL
/* MEM2 left free for libogc and IOS buffers above the plan */
#define WII_ARENA_RESERVE_BYTES 0x200000UL
/* the general region's page table bounds it */
#define MAXIMUM_GENERAL_PAGES 4096

static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cache_arena_owner arena_owner;
static struct wii_arena_report arena;
static int arena_live;
static DWORD page_protection[MAXIMUM_GENERAL_PAGES];
static unsigned long block_page_count[MAXIMUM_GENERAL_PAGES];
static unsigned long general_pages;
static int protection_reported;

int wii_arena_open(size_t offset, size_t general_bytes, struct wii_arena_report *report)
{
	struct cache_arena_request requests[NUMBER_OF_WII_ARENA_REGIONS];
	struct cache_arena_plan plan;
	struct cache_arena_result result;
	uintptr_t low, high, top;
	unsigned char *span;
	size_t data_bytes = 0;
	int index;

	pthread_mutex_lock(&arena_lock);
	if (arena_live || (offset % PAGE_SIZE_BYTES) || general_bytes % PAGE_SIZE_BYTES ||
		general_bytes / PAGE_SIZE_BYTES > MAXIMUM_GENERAL_PAGES)
	{
		pthread_mutex_unlock(&arena_lock);
		return 0;
	}
	memset(&arena, 0, sizeof(arena));
	low = (uintptr_t)SYS_GetArena2Lo();
	high = (uintptr_t)SYS_GetArena2Hi();
	arena.arena_low_before = low;
	arena.arena_high_before = high;
	requests[_wii_arena_game_state].size = HALO_PORT_GAME_STATE_SIZE;
	requests[_wii_arena_tag_cache].size = WII_TAG_CACHE_BYTES;
	requests[_wii_arena_sound_cache].size = WII_SOUND_CACHE_BYTES;
	requests[_wii_arena_general].size = general_bytes;
	for (index = 0; index < NUMBER_OF_WII_ARENA_REGIONS; index++)
	{
		requests[index].alignment = PAGE_SIZE_BYTES;
		data_bytes += requests[index].size;
	}
	/* the plan sits at the top of the arena, below IOS; the heap and libogc
	keep the bottom, which grows upwards (the heap moves into MEM2 once MEM1
	is full), at least the reserve of it */
	top = (high & ~(uintptr_t)(PAGE_SIZE_BYTES - 1)) - offset;
	span = (unsigned char *)(top - data_bytes);
	if (top <= data_bytes + low || (uintptr_t)span < low + WII_ARENA_RESERVE_BYTES ||
		!cache_arena_plan_build(requests, NUMBER_OF_WII_ARENA_REGIONS, span, data_bytes, 0, &plan, &result) ||
		!cache_arena_owner_bind(&arena_owner, &plan, span, data_bytes, &result))
	{
		arena.failed_allocations++;
		arena.required = data_bytes + WII_ARENA_RESERVE_BYTES;
		pthread_mutex_unlock(&arena_lock);
		if (report)
			*report = arena;
		return 0;
	}
	for (index = 0; index < NUMBER_OF_WII_ARENA_REGIONS; index++)
	{
		arena.region_base[index] = (uintptr_t)span + plan.slots[index].offset;
		arena.region_size[index] = plan.slots[index].size;
	}
	arena.required = plan.required + WII_ARENA_RESERVE_BYTES;
	arena.spare = (size_t)((uintptr_t)span - low) - WII_ARENA_RESERVE_BYTES;
	SYS_SetArena2Hi(span);
	arena.arena_high_during = (uintptr_t)SYS_GetArena2Hi();
	general_pages = general_bytes / PAGE_SIZE_BYTES;
	memset(page_protection, 0, sizeof(page_protection));
	memset(block_page_count, 0, sizeof(block_page_count));
	/* the regions start zeroed, as XPhysicalAlloc'd memory does */
	for (index = 0; index < NUMBER_OF_WII_ARENA_REGIONS; index++)
		memset((void *)arena.region_base[index], 0, arena.region_size[index]);
	arena_live = 1;
	pthread_mutex_unlock(&arena_lock);
	if (report)
		*report = arena;
	return 1;
}

int wii_arena_close(struct wii_arena_report *report)
{
	struct cache_arena_result result;
	unsigned long page;
	int ok = 1;

	pthread_mutex_lock(&arena_lock);
	if (!arena_live)
	{
		pthread_mutex_unlock(&arena_lock);
		return 0;
	}
	for (page = 0; page < general_pages; page++)
	{
		if (page_protection[page])
			ok = 0; /* a block still allocated */
	}
	/* only if nothing else moved the bound meanwhile */
	if ((uintptr_t)SYS_GetArena2Hi() == arena.arena_high_during)
		SYS_SetArena2Hi((void *)arena.arena_high_before);
	else
		ok = 0;
	arena.arena_high_after = (uintptr_t)SYS_GetArena2Hi();
	arena.arena_low_after = (uintptr_t)SYS_GetArena2Lo();
	if (!cache_arena_owner_release(&arena_owner, &result))
		ok = 0;
	arena_live = 0;
	general_pages = 0;
	if (report)
		*report = arena;
	pthread_mutex_unlock(&arena_lock);
	return ok && arena.arena_high_after == arena.arena_high_before;
}

void *wii_arena_region(enum wii_arena_region region, size_t *size)
{
	void *base = NULL;

	pthread_mutex_lock(&arena_lock);
	if (arena_live && region >= 0 && region < NUMBER_OF_WII_ARENA_REGIONS)
	{
		base = (void *)arena.region_base[region];
		if (size)
			*size = arena.region_size[region];
	}
	pthread_mutex_unlock(&arena_lock);
	return base;
}

void wii_arena_report_get(struct wii_arena_report *report)
{
	pthread_mutex_lock(&arena_lock);
	*report = arena;
	pthread_mutex_unlock(&arena_lock);
}

/* ---------- Custom Edition (see above) */

void *halo_custom_edition_tag_cache(void)
{
	wii_unsupported("custom_edition", "tag_cache_window_0x40440000");
	return NULL;
}

/* ---------- contiguous memory: the general region */

static uintptr_t general_base(void)
{
	return arena.region_base[_wii_arena_general];
}

BOOL platform_is_contiguous(const void *address)
{
	uintptr_t value = (uintptr_t)address;

	return arena_live && value >= general_base() && value - general_base() < general_pages * PAGE_SIZE_BYTES;
}

static BOOL pages_free(unsigned long first, unsigned long count)
{
	unsigned long page;

	if (first + count > general_pages)
		return FALSE;
	for (page = first; page < first + count; page++)
	{
		if (page_protection[page])
			return FALSE;
	}
	return TRUE;
}

static void general_usage(void)
{
	unsigned long page;
	size_t used = 0, blocks = 0;

	for (page = 0; page < general_pages; page++)
	{
		used += page_protection[page] ? PAGE_SIZE_BYTES : 0;
		blocks += block_page_count[page] != 0;
	}
	arena.general_in_use = used;
	arena.general_blocks = blocks;
	if (used > arena.general_peak)
		arena.general_peak = used;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long alignment_pages = alignment > PAGE_SIZE_BYTES ? alignment / PAGE_SIZE_BYTES : 1;
	unsigned long first = general_pages;
	unsigned long page;
	void *address;

	if (!count)
		count = 1;
	protect &= ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	if (!protect)
		protect = PAGE_READWRITE;
	pthread_mutex_lock(&arena_lock);
	if (!arena_live || physical_address != PLATFORM_ANY_PHYSICAL_ADDRESS || count > general_pages)
	{
		arena.failed_allocations++;
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}
	{
		unsigned long candidate = general_pages - count;

		for (;;)
		{
			candidate -= candidate % alignment_pages;
			if (pages_free(candidate, count))
			{
				first = candidate;
				break;
			}
			if (candidate == 0)
				break;
			candidate--;
		}
	}
	if (first == general_pages)
	{
		arena.failed_allocations++;
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}
	address = (void *)(general_base() + first * PAGE_SIZE_BYTES);
	memset(address, 0, count * PAGE_SIZE_BYTES);
	for (page = first; page < first + count; page++)
		page_protection[page] = protect;
	block_page_count[first] = count;
	general_usage();
	pthread_mutex_unlock(&arena_lock);
	return address;
}

void platform_contiguous_free(void *address)
{
	unsigned long first, count, page;

	pthread_mutex_lock(&arena_lock);
	if (!platform_is_contiguous(address))
	{
		pthread_mutex_unlock(&arena_lock);
		return;
	}
	first = ((uintptr_t)address - general_base()) / PAGE_SIZE_BYTES;
	count = block_page_count[first];
	for (page = first; page < first + count; page++)
		page_protection[page] = 0;
	block_page_count[first] = 0;
	general_usage();
	pthread_mutex_unlock(&arena_lock);
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	/* the Xbox's "highest acceptable address" form, which places a block at
	a fixed physical address, has no Wii equivalent */
	BOOL fixed = physical_address != (ULONG_PTR)-1 && physical_address < 0x20000000UL;
	void *result = fixed ? NULL :
		platform_contiguous_alloc(size, alignment, PLATFORM_ANY_PHYSICAL_ADDRESS, protect);

	if (!result)
	{
		platform_log("XPhysicalAlloc: cannot allocate %lu bytes (physical address 0x%08lx%s)",
			(unsigned long)size, (unsigned long)physical_address,
			fixed ? ": fixed addresses are not available on the Wii (ADR-015)" : "");
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	platform_contiguous_free(address);
}

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	uintptr_t start = (uintptr_t)address & ~(PAGE_SIZE_BYTES - 1);
	uintptr_t end = ((uintptr_t)address + size + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);

	pthread_mutex_lock(&arena_lock);
	if (old_protect)
		*old_protect = platform_is_contiguous(address) ?
			page_protection[(start - general_base()) / PAGE_SIZE_BYTES] : PAGE_READWRITE;
	if (platform_is_contiguous((void *)start))
	{
		uintptr_t page;

		for (page = (start - general_base()) / PAGE_SIZE_BYTES;
			page < (end - general_base()) / PAGE_SIZE_BYTES && page < general_pages; page++)
		{
			if (page_protection[page])
				page_protection[page] = new_protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
		}
	}
	pthread_mutex_unlock(&arena_lock);
	if (!protection_reported)
	{
		protection_reported = 1;
		wii_log("PARTIAL memory VirtualProtect: protection is recorded, not enforced (no MMU mapping)\n");
	}
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	DWORD protect = PAGE_READWRITE;

	pthread_mutex_lock(&arena_lock);
	if (platform_is_contiguous(address))
	{
		protect = page_protection[((uintptr_t)address - general_base()) / PAGE_SIZE_BYTES];
		if (!protect)
			protect = PAGE_NOACCESS;
	}
	pthread_mutex_unlock(&arena_lock);
	return protect;
}
