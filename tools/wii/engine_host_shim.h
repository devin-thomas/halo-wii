#ifndef WII_ENGINE_HOST_SHIM_H
#define WII_ENGINE_HOST_SHIM_H
#include <stdint.h>

/* MEM2's arena on the host: [low, high), mapped at those addresses (the Wii
 * run's); 1 on success (tools/wii/engine_host_shim.c) */
int host_arena_map(uintptr_t low, uintptr_t high);
#endif
