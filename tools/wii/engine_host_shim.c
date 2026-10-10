/* The few libogc services the Wii platform layer (port/wii/engine/wii_kernel.c,
 * wii_memory.c) calls, on an i686 Linux host, for the host reference of the
 * Wii engine build (HWI-015B, tools/wii/run_engine_map_host.py).
 *
 * MEM2's arena is a mapping at the addresses the Wii run reported (its
 * ARENA line), so the engine's regions, and every pointer into them, have the
 * Wii's addresses: the digests canonicalize such pointers, and the same
 * addresses make sure the host classifies every word as the Wii does. MEM1's
 * arena is reported empty (the host's heap is glibc's). */
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#include "engine_host_shim.h"

static uintptr_t arena2_low, arena2_high;
static char arena1[16];

int host_arena_map(uintptr_t low, uintptr_t high)
{
    void *mapped;
    uintptr_t mapped_high = (high + 0xfff) & ~(uintptr_t)0xfff;
    if (high <= low || (low & 0xfff))
        return 0;
    mapped = mmap((void *)low, mapped_high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                  -1, 0);
    if (mapped != (void *)low) {
        fprintf(stderr, "host arena: cannot map %#lx..%#lx\n", (unsigned long)low, (unsigned long)high);
        return 0;
    }
    arena2_low = low;
    arena2_high = high;
    return 1;
}

void LWP_YieldThread(void)
{
    sched_yield();
}

void *SYS_GetArena1Lo(void)
{
    return arena1;
}

void *SYS_GetArena1Hi(void)
{
    return arena1;
}

void *SYS_GetArena2Lo(void)
{
    return (void *)arena2_low;
}

void *SYS_GetArena2Hi(void)
{
    return (void *)arena2_high;
}

void SYS_SetArena2Hi(void *address)
{
    arena2_high = (uintptr_t)address;
}
