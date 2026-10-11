/*
BOOT_HOOKS.C

HWI-016D: the wrapped rasterizer backend entry points of the boot mode
(boot_hooks.h). Each calls the Wii rasterizer's hook when one is registered
(port/wii/render/render_ui.c, engine.dol's boot mode), and the engine's own
backend otherwise, so the i686 host reference and the map run are
unchanged. Compiled as an engine unit.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "interface/ui_widget.h"
#include "tag_files/tag_files.h"
#include "memory/crc.h"
#include "memory/data.h"

#include "boot_hooks.h"
#include "wii_platform.h"
#include "real_map_scenario.h"

/* the engine's own (render.c, rasterizer_xbox*.c), reached through the link's
--wrap; engine-typed arguments are passed through as they came */
struct render_window;
struct bitmap_data;

boolean __real__rasterizer_initialize(void);
void __real__rasterizer_dispose(void);
void __real_render_frame(struct render_window *windows, short window_count, void const *screenshot_page_index,
	void const *screenshot_index, struct bitmap_data *screenshot_bitmap, real time_delta_since_tick_sec);
void __real_render_frame_pregame(struct render_window const *window, struct bitmap_data *bitmap);
void __real_render_frame_present(void const *screenshot_index, struct bitmap_data *bitmap);
void __real__rasterizer_window_begin(void const *parameters);
void __real__rasterizer_window_end(void);
void __real__rasterizer_psuedo_dynamic_screen_quad_draw(void *parameters, void *vertices);
void __real_rasterizer_text_begin(void const *parameters);
void __real_rasterizer_text_draw_character(void const *vertices);
void __real_rasterizer_text_end(void);
boolean __real_rasterizer_bitmap_new(struct bitmap_data *bitmap);
void __real_rasterizer_bitmap_changed(struct bitmap_data *bitmap);
void __real_rasterizer_bitmap_delete(struct bitmap_data *bitmap);

static struct wii_boot_render_hooks boot_hooks;
static boolean boot_hooks_registered;

void wii_boot_set_render_hooks(
	const struct wii_boot_render_hooks *hooks)
{
	if (hooks)
	{
		boot_hooks = *hooks;
		boot_hooks_registered = TRUE;
	}
	else
	{
		csmemset(&boot_hooks, 0, sizeof(boot_hooks));
		boot_hooks_registered = FALSE;
	}
}

int wii_boot_render_hooks_set(
	void)
{
	return boot_hooks_registered;
}

boolean __wrap__rasterizer_initialize(
	void)
{
	if (boot_hooks.rasterizer_initialize)
		return boot_hooks.rasterizer_initialize() ? TRUE : FALSE;
	return __real__rasterizer_initialize();
}

void __wrap__rasterizer_dispose(
	void)
{
	if (boot_hooks.rasterizer_dispose)
		boot_hooks.rasterizer_dispose();
	else
		__real__rasterizer_dispose();
}

void __wrap_render_frame(
	struct render_window *windows,
	short window_count,
	void const *screenshot_page_index,
	void const *screenshot_index,
	struct bitmap_data *screenshot_bitmap,
	real time_delta_since_tick_sec)
{
	if (boot_hooks.frame)
		boot_hooks.frame(windows, window_count, time_delta_since_tick_sec);
	else
		__real_render_frame(windows, window_count, screenshot_page_index, screenshot_index, screenshot_bitmap,
			time_delta_since_tick_sec);
}

void __wrap_render_frame_pregame(
	struct render_window const *window,
	struct bitmap_data *bitmap)
{
	if (boot_hooks.frame_pregame)
		boot_hooks.frame_pregame(window);
	else
		__real_render_frame_pregame(window, bitmap);
}

void __wrap_render_frame_present(
	void const *screenshot_index,
	struct bitmap_data *bitmap)
{
	if (boot_hooks.present)
		boot_hooks.present();
	else
		__real_render_frame_present(screenshot_index, bitmap);
}

void __wrap__rasterizer_window_begin(
	void const *parameters)
{
	if (boot_hooks.window_begin)
		boot_hooks.window_begin(parameters);
	else
		__real__rasterizer_window_begin(parameters);
}

void __wrap__rasterizer_window_end(
	void)
{
	if (boot_hooks.window_end)
		boot_hooks.window_end();
	else
		__real__rasterizer_window_end();
}

void __wrap__rasterizer_psuedo_dynamic_screen_quad_draw(
	void *parameters,
	void *vertices)
{
	if (boot_hooks.screen_quad)
		boot_hooks.screen_quad(parameters, vertices);
	else
		__real__rasterizer_psuedo_dynamic_screen_quad_draw(parameters, vertices);
}

void __wrap_rasterizer_text_begin(
	void const *parameters)
{
	if (boot_hooks.text_begin)
		boot_hooks.text_begin(parameters);
	else
		__real_rasterizer_text_begin(parameters);
}

void __wrap_rasterizer_text_draw_character(
	void const *vertices)
{
	if (boot_hooks.text_character)
		boot_hooks.text_character(vertices);
	else
		__real_rasterizer_text_draw_character(vertices);
}

void __wrap_rasterizer_text_end(
	void)
{
	if (boot_hooks.text_end)
		boot_hooks.text_end();
	else
		__real_rasterizer_text_end();
}

boolean __wrap_rasterizer_bitmap_new(
	struct bitmap_data *bitmap)
{
	if (boot_hooks.bitmap_new)
		return boot_hooks.bitmap_new(bitmap) ? TRUE : FALSE;
	return __real_rasterizer_bitmap_new(bitmap);
}

void __wrap_rasterizer_bitmap_changed(
	struct bitmap_data *bitmap)
{
	if (boot_hooks.bitmap_changed)
		boot_hooks.bitmap_changed(bitmap);
	else
		__real_rasterizer_bitmap_changed(bitmap);
}

void __wrap_rasterizer_bitmap_delete(
	struct bitmap_data *bitmap)
{
	if (boot_hooks.bitmap_delete)
		boot_hooks.bitmap_delete(bitmap);
	else
		__real_rasterizer_bitmap_delete(bitmap);
}

/* ---------- the menus' map selection (boot_hooks.h) */

void __real_main_set_map_name(char const *map_name);
void __real_main_menu_switch_to_single_player(void);

static wii_boot_map_check_proc boot_map_check;
static boolean boot_map_refused;
static long boot_map_refusal_count;
static char boot_map_refused_name[64];
static char boot_map_refused_reason[256];
static wchar_t boot_map_refused_text[256];

void wii_boot_set_map_check(
	wii_boot_map_check_proc check)
{
	boot_map_check = check;
}

long wii_boot_map_refusals(
	const char **last_map,
	const char **last_reason)
{
	if (last_map)
		*last_map = boot_map_refused_name;
	if (last_reason)
		*last_reason = boot_map_refused_reason;
	return boot_map_refusal_count;
}

void __wrap_main_set_map_name(
	char const *map_name)
{
	boot_map_refused = FALSE;
	if (boot_map_check && map_name &&
		boot_map_check(map_name, boot_map_refused_reason, sizeof(boot_map_refused_reason)))
	{
		long index;

		boot_map_refused = TRUE;
		boot_map_refusal_count++;
		csstrncpy(boot_map_refused_name, tag_name_strip_path(map_name), sizeof(boot_map_refused_name) - 1);
		boot_map_refused_name[sizeof(boot_map_refused_name) - 1] = 0;
		for (index = 0; boot_map_refused_reason[index] && index < NUMBEROF(boot_map_refused_text) - 1; index++)
			boot_map_refused_text[index] = (wchar_t)(unsigned char)boot_map_refused_reason[index];
		boot_map_refused_text[index] = 0;
		wii_log("BOOT_MAP_REFUSED map=%s count=%ld reason=\"%s\"\n", boot_map_refused_name, boot_map_refusal_count,
			boot_map_refused_reason);
		error(_error_silent, "the map '%s' was not loaded: %s", map_name, boot_map_refused_reason);
		/* the engine's own error dialog, with the port's text */
		display_error_text_deferred(boot_map_refused_text, NONE);
		return;
	}
	__real_main_set_map_name(map_name);
}

void __wrap_main_menu_switch_to_single_player(
	void)
{
	/* (the map it would change to was refused: the menus stay) */
	if (boot_map_refused)
	{
		wii_log("BOOT_MAP_SWITCH_SKIPPED map=%s\n", boot_map_refused_name);
		return;
	}
	__real_main_menu_switch_to_single_player();
}

/* ---------- the boot mode's clock (boot_hooks.h) */

unsigned long __real_system_milliseconds(void);
void __stdcall __real_Sleep(unsigned long milliseconds);

static boolean boot_frame_clock;
static unsigned long boot_clock;
static unsigned long boot_clock_thread;

void wii_boot_set_frame_clock(
	int on)
{
	boot_frame_clock = on ? TRUE : FALSE;
	boot_clock = 0;
	boot_clock_thread = wii_thread_self();
}

unsigned long wii_boot_clock_milliseconds(
	void)
{
	return boot_clock;
}

unsigned long __wrap_system_milliseconds(
	void)
{
	if (boot_frame_clock)
		return boot_clock;
	return __real_system_milliseconds();
}

void __stdcall __wrap_Sleep(
	unsigned long milliseconds)
{
	if (boot_frame_clock && wii_thread_self() == boot_clock_thread)
	{
		boot_clock += milliseconds;
		return;
	}
	__real_Sleep(milliseconds);
}

/* ---------- the game state's digest at a boot-mode checkpoint */


unsigned long wii_boot_state_digest(
	unsigned long *per_allocation,
	long count)
{
	unsigned long crc;
	long index;

	crc_new(&crc);
	for (index = 0; index < real_map_allocation_count(); index++)
	{
		byte const *address = (byte const *)real_map_allocation_address(index);
		unsigned long size = real_map_allocation_bytes(index);
		unsigned long digest;
		byte word[4];

		if (size >= sizeof(struct data_array) &&
			((struct data_array const *)address)->signature == 'd@t@')
		{
			struct data_array const *data = (struct data_array const *)address;
			unsigned long live = sizeof(struct data_array) + (unsigned long)MAX(data->count, 0) * data->size;

			size = MIN(live, size);
		}
		digest = real_map_digest_bytes(address, size);
		if (per_allocation && index < count)
			per_allocation[index] = digest;
		word[0] = (byte)digest;
		word[1] = (byte)(digest >> 8);
		word[2] = (byte)(digest >> 16);
		word[3] = (byte)(digest >> 24);
		crc_checksum_buffer(&crc, word, 4);
	}

	return crc;
}

/* ---------- sound (shell_initialize's sound_initialize)

Without DirectSound, sound_initialize (sound_manager.c) makes the sound
arrays and then fails in its platform's start-up, leaving the looping
sounds' array allocated and never made valid; the first map change then
asserts in data_delete_all. The Wii has no sound yet (HWI-015B never
started it): in the boot mode it is reported unsupported and not started,
so the arrays stay NULL, which every sound path tests. */

void __real_sound_initialize(void);

void __wrap_sound_initialize(
	void)
{
	if (boot_hooks_registered)
	{
		wii_unsupported("dsound", "sound_initialize (no DirectSound: sound not started)");
		return;
	}
	__real_sound_initialize();
}

/* ---------- movies (bink_playback.c)

The first main menu plays the intro movie first (main_screen_shell_load);
Bink and the converted movies are not on the Wii yet (HWI-008D, R13), and
starting one takes the texture cache's memory, which the Wii build does not
have (an assertion). In the boot mode a movie is reported unsupported and not
started: bink_playback_active stays FALSE and the menus load at once, as they
do on the Xbox when the movie cannot be opened. */

void __real_bink_playback_start(char const *full_pathname, unsigned long flags);

void __wrap_bink_playback_start(
	char const *full_pathname,
	unsigned long flags)
{
	if (boot_hooks_registered)
	{
		wii_unsupported("bink", "bink_playback_start (movies: HWI-008D)");
		return;
	}
	__real_bink_playback_start(full_pathname, flags);
}

/* the sound cache's idle (sound_render, every frame): the cache was never
made (sound not started), so in the boot mode there is nothing to idle */
void __real_sound_cache_idle(void);

void __wrap_sound_cache_idle(
	void)
{
	if (boot_hooks_registered)
		return;
	__real_sound_cache_idle();
}
