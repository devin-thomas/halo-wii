/*
PHYSICAL_MEMORY_MAP_WII.C

The Wii build's replacement for source/cache/physical_memory_map.c
(HWI-015, ADR-015). The upstream unit asks XPhysicalAlloc for the game state
and the tag cache at fixed Xbox addresses and asserts it got them; those
addresses are not usable memory on the Wii (ADR-016). Here the four regions
come from the engine's MEM2 arena plan (port/wii/engine/wii_memory.c), at
bases chosen at run time; everything that uses them asks
physical_memory_get_*_base_address, as upstream. The interface, sizes and
the page check in physical_memory_verify are the upstream unit's.

The texture cache is not placed: the Wii build has no renderer yet, and
neither memory bank holds the upstream texture cache beside the required
reservations (ADR-015 record; HWI-032 owns texture residency). Its base is
NULL and the first request for it is reported as unsupported.
*/

#include "cseries.h"
#include "cseries_windows.h"
#include "cache/physical_memory_map.h"

#include "wii_platform.h"

/* the upstream sizes (cache/physical_memory_map.c) */
#define GAME_STATE_SIZE HALO_PORT_GAME_STATE_SIZE
#define GAME_STATE_VERIFY_SIZE HALO_PORT_GAME_STATE_CPU_SIZE
#define SOUND_CACHE_SIZE 0x400000

struct physical_memory_map_globals
{
	void *game_state_base_address;
	void *tag_cache_base_address;
	void *texture_cache_base_address;
	void *sound_cache_base_address;
};

static struct physical_memory_map_globals physical_memory_map_globals;

void physical_memory_allocate(
	void)
{
	size_t size;

	csmemset(&physical_memory_map_globals, 0, sizeof(physical_memory_map_globals));
	physical_memory_map_globals.game_state_base_address = wii_arena_region(_wii_arena_game_state, &size);
	match_assert(__FILE__, __LINE__, physical_memory_map_globals.game_state_base_address && size >= GAME_STATE_SIZE);
	physical_memory_map_globals.tag_cache_base_address = wii_arena_region(_wii_arena_tag_cache, &size);
	match_assert(__FILE__, __LINE__, physical_memory_map_globals.tag_cache_base_address && size >= TAG_CACHE_SIZE);
	physical_memory_map_globals.sound_cache_base_address = wii_arena_region(_wii_arena_sound_cache, &size);
	match_assert(__FILE__, __LINE__, physical_memory_map_globals.sound_cache_base_address && size >= SOUND_CACHE_SIZE);
	physical_memory_map_globals.texture_cache_base_address = NULL;

	return;
}

void physical_memory_verify(
	void)
{
	byte *address;
	unsigned long page_status;

	for (address = physical_memory_map_globals.tag_cache_base_address;
		address < (byte *)physical_memory_map_globals.tag_cache_base_address + TAG_CACHE_SIZE;
		address += 0x1000)
	{
		page_status = XQueryMemoryProtect(address);
		match_assert(__FILE__, __LINE__, page_status == PAGE_READWRITE);
	}

	for (address = physical_memory_map_globals.game_state_base_address;
		address < (byte *)physical_memory_map_globals.game_state_base_address + GAME_STATE_VERIFY_SIZE;
		address += 0x1000)
	{
		page_status = XQueryMemoryProtect(address);
		match_assert(__FILE__, __LINE__, page_status == PAGE_READWRITE);
	}

	return;
}

void physical_memory_free(
	void)
{
	/* the regions return to the arena when the driver closes it
	(wii_arena_close); nothing here may use them afterwards */
	csmemset(&physical_memory_map_globals, 0, sizeof(physical_memory_map_globals));

	return;
}

void *physical_memory_get_game_state_base_address(
	void)
{
	return physical_memory_map_globals.game_state_base_address;
}

void *physical_memory_get_tag_cache_base_address(
	void)
{
	return physical_memory_map_globals.tag_cache_base_address;
}

void *physical_memory_get_texture_cache_base_address(
	void)
{
	wii_unsupported("render", "texture_cache");
	return physical_memory_map_globals.texture_cache_base_address;
}

void *physical_memory_get_sound_cache_base_address(
	void)
{
	return physical_memory_map_globals.sound_cache_base_address;
}
