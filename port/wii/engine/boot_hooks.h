/*
BOOT_HOOKS.H

HWI-016D: booting the engine's own main through the original menus. In the
boot mode (WII_ENGINE_ROOT/boot.txt, port/wii/engine/boot_main.c) the Wii
driver runs the engine's own main (source/shell/shell_xbox.c): shell
start-up, then main_loop, which loads the UI map (levels\ui\ui), runs the
interface's own widgets, and changes maps as the menus ask.

main_loop draws through the Xbox rasterizer, which needs a Direct3D device
the Wii does not have. The link wraps the rasterizer's backend entry points
the menus and the frame reach (tools/wii/engine_build.py WRAPPED); this unit
(boot_hooks.c, an engine unit compiled into engine.dol and the i686 host
reference alike) defines the wrappers. Each passes the call to the
engine's own backend unless the Wii rasterizer registered its own
(port/wii/render/render_ui.c): the i686 host reference registers none, so
nothing it runs changes.

Only C types here: the engine-typed arguments are passed as pointers to the
engine's own structures, which the registered side reads with the engine's
headers.
*/

#ifndef __HALO_WII_BOOT_HOOKS_H
#define __HALO_WII_BOOT_HOOKS_H

struct wii_boot_render_hooks
{
	/* _rasterizer_initialize / _rasterizer_dispose (rasterizer_xbox.c), after
	the engine's generic rasterizer_initialize has made its game-state
	allocation */
	int (*rasterizer_initialize)(void);
	void (*rasterizer_dispose)(void);
	/* render.c: render_frame (struct render_window *, count, dt),
	render_frame_pregame (struct render_window const *) and
	render_frame_present */
	void (*frame)(void *windows, short window_count, float dt);
	void (*frame_pregame)(const void *window);
	void (*present)(void);
	/* _rasterizer_window_begin (struct rasterizer_window_begin_parameters
	const *) and _rasterizer_window_end */
	void (*window_begin)(const void *parameters);
	void (*window_end)(void);
	/* _rasterizer_psuedo_dynamic_screen_quad_draw (struct
	rasterizer_dynamic_screen_geometry_parameters *, struct
	dynamic_screen_vertex *): the interface's bitmaps */
	void (*screen_quad)(void *parameters, void *vertices);
	/* rasterizer_text_begin (parameters), rasterizer_text_draw_character
	(struct dynamic_screen_vertex const[4]), rasterizer_text_end: the
	engine's own text, from its own glyph cache (rasterizer_text.c) */
	void (*text_begin)(const void *parameters);
	void (*text_character)(const void *vertices);
	void (*text_end)(void);
	/* rasterizer_bitmap_new / _changed / _delete (struct bitmap_data *):
	the glyph cache's bitmap and any other bitmap the engine makes */
	int (*bitmap_new)(void *bitmap);
	void (*bitmap_changed)(void *bitmap);
	void (*bitmap_delete)(void *bitmap);
};

void wii_boot_set_render_hooks(const struct wii_boot_render_hooks *hooks);
/* whether the Wii rasterizer registered (the boot mode) */
int wii_boot_render_hooks_set(void);

/* The map a menu asks for (main.c's main_set_map_name, from the menus' own
level selection), checked before the engine commits to it. The Xbox engine
treats a map that will not load as fatal (game_precache_new_map's assertion,
the damaged-disc error); on the Wii a map is staged on the card and can be
missing or not the one its manifest names. With a check registered (the
boot mode), a map it refuses is not set: the engine's own error dialog shows
the reason, with what to do, and the menus stay where they are, the loaded
data untouched. check returns 0 if the map may load, else writes the reason. */
typedef int (*wii_boot_map_check_proc)(const char *map_name, char *reason, unsigned long reason_size);

void wii_boot_set_map_check(wii_boot_map_check_proc check);
/* refusals so far, and the last one's map name (for the on-screen line) */
long wii_boot_map_refusals(const char **last_map, const char **last_reason);

/* The boot mode's clock. main_update_time (main.c) waits out each frame's
remainder of 33 ms with Sleep and measures the frame by system_milliseconds;
with the frame clock on, the main thread's Sleep advances a clock of its own
by what it was asked to wait (and returns at once: the driver's present
waits for the retrace), and system_milliseconds reads that clock. Every
frame is then one 33 ms step of game time and of the menus' own timers, the
same in every launch and on the i686 host, which replays the launch's pad
input frame by frame. Off (the default), both are the engine's own. */
void wii_boot_set_frame_clock(int on);
unsigned long wii_boot_clock_milliseconds(void);

/* The game state's digests at a boot-mode checkpoint, through the recorded
allocations (real_map_scenario.h): each allocation's live bytes (a data
array's header and its elements below its count; anything else whole) by
real_map_digest_bytes, which does not depend on byte order. Returns the
CRC of all of them; per_allocation (count entries) may be NULL. */
unsigned long wii_boot_state_digest(unsigned long *per_allocation, long count);

#endif
