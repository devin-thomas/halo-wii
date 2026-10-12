/*
BOOT_MAIN.H

HWI-016D: the boot mode's driver half (boot_main.c, libogc): the engine's own
main through the original menus, run by the Wii driver (wii_engine_main.c)
when WII_ENGINE_ROOT/boot.txt is on the card. C types only.
*/

#ifndef __HALO_WII_BOOT_MAIN_H
#define __HALO_WII_BOOT_MAIN_H

#include "../render/wii_render.h"

struct wii_boot_driver
{
	struct wii_render_video video;
	/* the engine's own main (source/shell/shell_xbox.c), __real_main */
	int (*real_main)(void);
};

/* boot.txt is on the card */
int wii_boot_requested(void);
/* runs the boot mode; the launch ends inside it (exit) once the engine
side or a boot error finishes it. Returns 0 only when boot.txt is not a
boot configuration (the driver goes on with the map run). */
int wii_boot_run(const struct wii_boot_driver *driver);

/* (wii_engine_main.c, HWI-033) the main stack's peak below the driver's entry
frame so far; its size and whether the painted window was used up */
unsigned long wii_main_stack_peak(unsigned long *stack_bytes, int *exhausted);

#endif
