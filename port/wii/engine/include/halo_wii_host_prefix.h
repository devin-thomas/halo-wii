/*
HALO_WII_HOST_PREFIX.H

Force-included ahead of every engine and platform unit of the i686 host
reference of the Wii engine build (HWI-015B, tools/wii/run_engine_map_host.py):
the Linux build's prefix, which reproduces the MSVC/XDK environment for x86,
and the one change the Wii prefix makes to what the engine compiles: the
game state's base is chosen at run time from the MEM2 arena plan
(port/wii/engine/physical_memory_map_wii.c, ADR-015), not the native builds'
fixed address.
*/

#ifndef __HALO_WII_HOST_PREFIX_H
#define __HALO_WII_HOST_PREFIX_H

#include "../../../linux/include/halo_linux_prefix.h"

void *physical_memory_get_game_state_base_address(void);
#undef HALO_PORT_GAME_STATE_BASE_ADDRESS
#define HALO_PORT_GAME_STATE_BASE_ADDRESS ((unsigned long)physical_memory_get_game_state_base_address())

#endif /* __HALO_WII_HOST_PREFIX_H */
