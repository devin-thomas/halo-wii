/*
RENDER_UI.C

HWI-016D: the engine-side half of the boot mode's rasterizer
(wii_render_ui.h). Compiled as an engine unit (the engine's headers and
flags), in engine.dol only.

In the boot mode the engine's own main runs (source/shell/shell_xbox.c):
shell_initialize, then main_loop, which loads the UI map, runs the
interface's own widgets (process_ui_widgets) and the map changes the menus
ask for. main_loop draws through the rasterizer; the boot hooks
(port/wii/engine/boot_hooks.h) route its backend here:

- start-up (_rasterizer_initialize): the screen and title-safe frame bounds
  of the Xbox rasterizer (640x480, frame 48,36..592,444), the clip
  distances, the engine's own glyph cache (rasterizer_text_cache_initialize)
  and the environment path's boot mode (render_engine.c);
- each frame (render.c's render_frame, render_frame_pregame): for each
  player window, the loaded map's environment from the window's own camera
  (main_loop's director and observer), then the engine's own
  render_ui_widgets over it in the window's viewport; the progress bar as
  render_frame draws it. The console window draws nothing here (no console
  on the Wii);
- the interface's draws (_rasterizer_psuedo_dynamic_screen_quad_draw): each
  bitmap is found by its bitmap tag and its index in the tag's bitmaps
  (bitmap_data.tag_index; a lookup through the bitmap tags when it is not
  set), and drawn from the converted HWT1 with the vertex colour times the
  map's tint and fade, in the engine's framebuffer blend. The menus' plasma
  effect (two animated plasma maps blended under the widget's bitmap) is
  not composited: the widget's own bitmap is drawn, a labelled
  approximation, counted;
- text (rasterizer_text_begin/draw_character/end): the engine lays the
  string out (draw_string.c) and fills its own glyph cache
  (rasterizer_text.c, its fonts' 8-bit coverage from the map's font tags);
  the cache's bitmap is copied into a GX texture when the engine changes it,
  and each character is a quad from it;
- the frame's end (render_frame_present): the diagnostic line (the
  manifests, the map's label), the screen checks, the copy to the screen.

Every screen the interface shows is logged (BOOT_SCREEN): its widget tag,
when it changed, and, once it has stood for the configured frames, a CRC of
the EFB on a 32x24 grid and the game state's digest.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "cache/cache_files.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_text.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "interface/ui_widget.h"
#include "interface/progress_bar.h"
#include "bink/bink_playback.h"
#include "game/game.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "memory/crc.h"

#include "wii_render.h"
#include "wii_render_ui.h"
#include "../engine/boot_hooks.h"
#include "../engine/engine_hooks.h"
#include "../engine/wii_platform.h"
#include "../engine/real_map_scenario.h"

/* the engine's (port hooks, ui_widget.c's port accessor, main.c) */
void halo_screen_ui_offset(unsigned char centered);
void ui_widget_port_active_screen(short local_player_index, long *screen_tag_index, long *focused_tag_index);
extern boolean global_frame_rate_throttle;

/* ---------- constants */

enum
{
	BOOT_UI_SCREEN_WIDTH = 640,
	BOOT_UI_SCREEN_HEIGHT = 480,
	/* the Xbox rasterizer's title-safe frame (rasterizer_xbox.c) */
	BOOT_UI_FRAME_X0 = 48,
	BOOT_UI_FRAME_Y0 = 36,
	BOOT_UI_FRAME_X1 = 592,
	BOOT_UI_FRAME_Y1 = 444,
	BOOT_UI_MAXIMUM_DYNAMIC_BITMAPS = 4,
	BOOT_UI_MAXIMUM_TEXT_QUADS = 512,
	BOOT_UI_MAXIMUM_BITMAP_LOOKUPS = 256,
	BOOT_UI_MAXIMUM_STATE_ALLOCATIONS = REAL_MAP_MAXIMUM_ALLOCATIONS,
	/* screens remembered for the totals */
	BOOT_UI_MAXIMUM_SCREENS = 64,
};

/* the engine's framebuffer blend functions: the GX side takes them as they are */
typedef char boot_ui_blend_alpha_is_zero[WII_UI_BLEND_ALPHA == 0 ? 1 : -1];

/* ---------- structures */

struct boot_ui_dynamic_bitmap
{
	struct bitmap_data *bitmap;
	long handle;
	boolean dirty;
	unsigned long updates;
};

struct boot_ui_bitmap_lookup
{
	struct bitmap_data const *bitmap;
	long tag;
	long index;
};

struct boot_ui_screen
{
	long screen_tag;
	long focused_tag;
	long first_frame;
	long frames;
	boolean sampled;
};

struct boot_ui_globals
{
	struct wii_boot_ui_config config;
	boolean initialized;
	long frame;
	long environment_frames;
	boolean environment_drawn;
	char map_name[64];
	long scenario_index;
	long map_changes;
	/* HWI-033: the loaded map is a campaign map (its scenario's type is solo),
	whose environment drawing is pending (HWI-030): not drawn, labelled */
	boolean campaign;
	boolean environment_pending;    /* this frame */
	boolean pending_logged;         /* this map */
	/* the target map's ticks (the tick observer) and those digested */
	long target_ticks;
	long tick_digests;
	/* the glyph cache and any other bitmap the engine makes */
	struct boot_ui_dynamic_bitmap dynamic[BOOT_UI_MAXIMUM_DYNAMIC_BITMAPS];
	/* text between rasterizer_text_begin and _end */
	struct wii_ui_draw text_draw;
	struct bitmap_data *text_bitmap;
	real text_scale_u, text_scale_v;
	struct wii_ui_vertex text_quads[BOOT_UI_MAXIMUM_TEXT_QUADS * 4];
	long text_quad_count;
	/* bitmaps whose tag_index is not set, found through the bitmap tags */
	struct boot_ui_bitmap_lookup lookups[BOOT_UI_MAXIMUM_BITMAP_LOOKUPS];
	long lookup_count;
	boolean tag_index_reported;
	/* counts */
	unsigned long screen_quads, text_characters, plasma_approximated, meter_approximated, untextured_quads;
	unsigned long plasma_composited, plasma_missing;
	unsigned long missing_bitmaps, console_windows, nonplayer_windows, pregame_frames, player_windows;
	/* the screens */
	struct boot_ui_screen current;
	struct boot_ui_screen screens[BOOT_UI_MAXIMUM_SCREENS];
	long screen_count;
	unsigned long state_digests[BOOT_UI_MAXIMUM_STATE_ALLOCATIONS];
	boolean finished;
	long screen_samples;
};

/* ---------- globals */

static struct boot_ui_globals boot_ui;

/* ---------- private code */

static void boot_ui_finish(
	int result,
	char const *reason)
{
	if (boot_ui.finished)
		return;
	boot_ui.finished = TRUE;
	wii_render_ui_report();
	if (boot_ui.config.finish)
		boot_ui.config.finish(result, reason);
}

/* the loaded scenario's map name (levels\...\<name>), "" before one loads */
static void boot_ui_map_current(
	void)
{
	char const *name = global_scenario_index != NONE ? tag_name_strip_path(tag_get_name(global_scenario_index)) : "";

	/* (every map's scenario can be its first tag: the same datum; the name tells) */
	if (global_scenario_index == boot_ui.scenario_index && !csstrcmp(name, boot_ui.map_name))
		return;
	if (boot_ui.scenario_index != NONE || global_scenario_index != NONE)
	{
		/* the interface's textures are the old map's */
		wii_ui_gx_release_map_textures();
		boot_ui.lookup_count = 0;
		boot_ui.map_changes++;
	}
	boot_ui.scenario_index = global_scenario_index;
	boot_ui.map_name[0] = 0;
	boot_ui.campaign = FALSE;
	boot_ui.pending_logged = FALSE;
	boot_ui.target_ticks = 0;
	if (global_scenario_index != NONE)
	{
		csstrncpy(boot_ui.map_name, tag_name_strip_path(tag_get_name(global_scenario_index)),
			sizeof(boot_ui.map_name) - 1);
		boot_ui.map_name[sizeof(boot_ui.map_name) - 1] = 0;
		boot_ui.campaign = global_scenario_get()->type == _scenario_type_solo;
	}
	wii_log("BOOT_MAP frame=%ld map=%s scenario_tag=%ld kind=%s changes=%ld game_time=%ld\n", boot_ui.frame,
		boot_ui.map_name[0] ? boot_ui.map_name : "(none)",
		global_scenario_index == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(global_scenario_index),
		global_scenario_index == NONE ? "none" : boot_ui.campaign ? "campaign" :
		!csstrcmp(boot_ui.map_name, "ui") ? "menus" : "multiplayer",
		boot_ui.map_changes, game_in_progress() ? game_time_get() : -1L);
}

static boolean boot_ui_in_menu_map(
	void)
{
	return !csstrcmp(boot_ui.map_name, "ui");
}

static struct boot_ui_dynamic_bitmap *boot_ui_dynamic_find(
	struct bitmap_data const *bitmap)
{
	short index;

	for (index = 0; index < BOOT_UI_MAXIMUM_DYNAMIC_BITMAPS; index++)
	{
		if (boot_ui.dynamic[index].bitmap == bitmap)
			return &boot_ui.dynamic[index];
	}

	return NULL;
}

/* a tag's bitmap: its bitmap tag and its index among the tag's bitmaps */
static boolean boot_ui_bitmap_identify(
	struct bitmap_data const *bitmap,
	long *tag,
	long *index)
{
	struct tag_iterator iterator;
	long tag_index;
	long lookup;

	if (bitmap->tag_index != NONE && bitmap->tag_index != 0)
	{
		struct bitmap_group *group = (struct bitmap_group *)tag_get(BITMAP_GROUP_TAG, bitmap->tag_index);
		struct bitmap_data const *first = (struct bitmap_data const *)group->bitmaps.address;

		if (bitmap >= first && bitmap < first + group->bitmaps.count)
		{
			*tag = DATUM_INDEX_TO_ABSOLUTE_INDEX(bitmap->tag_index);
			*index = (long)(bitmap - first);
			return TRUE;
		}
	}
	for (lookup = 0; lookup < boot_ui.lookup_count; lookup++)
	{
		if (boot_ui.lookups[lookup].bitmap == bitmap)
		{
			*tag = boot_ui.lookups[lookup].tag;
			*index = boot_ui.lookups[lookup].index;
			return *tag != NONE;
		}
	}
	if (!boot_ui.tag_index_reported)
	{
		boot_ui.tag_index_reported = TRUE;
		wii_log("BOOT_UI_NOTE bitmap_data.tag_index not set (%08lx): bitmaps are found through the bitmap tags\n",
			(unsigned long)bitmap->tag_index);
	}
	*tag = NONE;
	*index = NONE;
	tag_iterator_new(&iterator, BITMAP_GROUP_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct bitmap_group *group = (struct bitmap_group *)tag_get(BITMAP_GROUP_TAG, tag_index);
		struct bitmap_data const *first = (struct bitmap_data const *)group->bitmaps.address;

		if (first && bitmap >= first && bitmap < first + group->bitmaps.count)
		{
			*tag = DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index);
			*index = (long)(bitmap - first);
			break;
		}
	}
	if (boot_ui.lookup_count < BOOT_UI_MAXIMUM_BITMAP_LOOKUPS)
	{
		boot_ui.lookups[boot_ui.lookup_count].bitmap = bitmap;
		boot_ui.lookups[boot_ui.lookup_count].tag = *tag;
		boot_ui.lookups[boot_ui.lookup_count].index = *index;
		boot_ui.lookup_count++;
	}

	return *tag != NONE;
}

/* the GX texture of a bitmap the engine draws: its glyph cache's, or a tag's */
static long boot_ui_texture(
	struct bitmap_data *bitmap)
{
	struct boot_ui_dynamic_bitmap *dynamic;
	long tag, index;

	if (!bitmap)
		return -1;
	dynamic = boot_ui_dynamic_find(bitmap);
	if (dynamic)
	{
		if (dynamic->dirty && dynamic->handle >= 0 && bitmap->base_address)
		{
			wii_ui_gx_texture_dynamic_update(dynamic->handle, (uint16_t const *)bitmap->base_address);
			dynamic->dirty = FALSE;
			dynamic->updates++;
		}
		return dynamic->handle;
	}
	if (!boot_ui.map_name[0] || !boot_ui_bitmap_identify(bitmap, &tag, &index))
	{
		boot_ui.missing_bitmaps++;
		return -1;
	}

	return wii_ui_gx_texture_tag(boot_ui.map_name, tag, index, bitmap->width, bitmap->height);
}

static uint32_t boot_ui_modulate(
	pixel32 argb,
	real_rgb_color const *tint,
	real const *fade)
{
	real a = (real)((argb >> 24) & 0xFF);
	real r = (real)((argb >> 16) & 0xFF);
	real g = (real)((argb >> 8) & 0xFF);
	real b = (real)(argb & 0xFF);

	if (tint)
	{
		r *= PIN(tint->red, 0.0f, 1.0f);
		g *= PIN(tint->green, 0.0f, 1.0f);
		b *= PIN(tint->blue, 0.0f, 1.0f);
	}
	if (fade)
		a *= PIN(*fade, 0.0f, 1.0f);

	return ((uint32_t)(a + 0.5f) << 24) | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) |
		(uint32_t)(b + 0.5f);
}

/* the texture cache's question for a bitmap (engine_hooks.h): the menus'
button icons, drawn inline in their text, are answered while the menus' map
is loaded and the icon's texture is on the card; nothing else (a game
map's decals keep the answer the map run has: none) */
static void *boot_ui_bitmap_hardware_format(
	struct bitmap_data *bitmap)
{
	if (!bitmap || !boot_ui.initialized)
		return NULL;
	/* HWI-033: the map loaded now, not the one the last frame drew. The
	engine asks while a new map loads, before its first frame: the menus'
	map name was kept, and a bitmap of the new map (Prisoner's lightmap page,
	tag 1642 bitmap 1) was looked for among the menus' textures and reported
	missing */
	boot_ui_map_current();
	if (!boot_ui_in_menu_map())
		return NULL;
	return boot_ui_texture(bitmap) >= 0 ? (void *)bitmap : NULL;
}

/* each recorded allocation's digest (the last wii_boot_state_digest), for
comparing a checkpoint with the host's allocation by allocation */
static void boot_ui_log_allocations(
	char const *checkpoint)
{
	long index;

	for (index = 0; index < real_map_allocation_count() && index < BOOT_UI_MAXIMUM_STATE_ALLOCATIONS; index++)
	{
		wii_log("BOOT_STATE_ALLOCATION at=%s frame=%ld index=%ld digest=%08lx name=%s\n", checkpoint, boot_ui.frame,
			index, boot_ui.state_digests[index], real_map_allocation_name(index));
	}
}

/* the last digests without the script runtime's allocations ("hs ..."):
HWI-016D's comparison, kept beside the whole state's (which reads the
script threads by their fields since HWI-033) */
static unsigned long boot_ui_state_without_scripts(
	void)
{
	unsigned long crc;
	long index;

	crc_new(&crc);
	for (index = 0; index < real_map_allocation_count() && index < BOOT_UI_MAXIMUM_STATE_ALLOCATIONS; index++)
	{
		unsigned long digest = boot_ui.state_digests[index];
		byte word[4];

		if (!csstrncmp(real_map_allocation_name(index), "hs ", 3))
			continue;
		word[0] = (byte)digest;
		word[1] = (byte)(digest >> 8);
		word[2] = (byte)(digest >> 16);
		word[3] = (byte)(digest >> 24);
		crc_checksum_buffer(&crc, word, 4);
	}

	return crc;
}

/* ---------- the screens */

static void boot_ui_screen_check(
	void)
{
	long screen_tag, focused_tag;
	struct boot_ui_screen *current = &boot_ui.current;

	ui_widget_port_active_screen(0, &screen_tag, &focused_tag);
	if (screen_tag != current->screen_tag || focused_tag != current->focused_tag)
	{
		if (current->frames > 0 && boot_ui.screen_count < BOOT_UI_MAXIMUM_SCREENS)
			boot_ui.screens[boot_ui.screen_count++] = *current;
		csmemset(current, 0, sizeof(*current));
		current->screen_tag = screen_tag;
		current->focused_tag = focused_tag;
		current->first_frame = boot_ui.frame;
		wii_log("BOOT_SCREEN_CHANGE frame=%ld clock_ms=%lu map=%s screen_tag=%ld screen=%s focused_tag=%ld "
			"focused=%s\n", boot_ui.frame, wii_boot_clock_milliseconds(), boot_ui.map_name,
			screen_tag == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(screen_tag),
			screen_tag == NONE ? "(none)" : tag_get_name(screen_tag),
			focused_tag == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(focused_tag),
			focused_tag == NONE ? "(none)" : tag_get_name(focused_tag));
	}
	current->frames++;
	if (!current->sampled && current->frames == boot_ui.config.screen_settle_frames)
	{
		int covered = 0;
		unsigned long crc = wii_ui_gx_sample_crc(&covered);
		unsigned long state = wii_boot_state_digest(boot_ui.state_digests, BOOT_UI_MAXIMUM_STATE_ALLOCATIONS);

		current->sampled = TRUE;
		wii_log("BOOT_SCREEN frame=%ld clock_ms=%lu map=%s screen_tag=%ld focused_tag=%ld efb_crc=%08lx covered=%d "
			"state=%08lx state_without_scripts=%08lx game_time=%ld\n", boot_ui.frame, wii_boot_clock_milliseconds(),
			boot_ui.map_name, screen_tag == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(screen_tag),
			focused_tag == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(focused_tag), crc, covered, state,
			boot_ui_state_without_scripts(), game_in_progress() ? game_time_get() : -1L);
		if (boot_ui.screen_samples++ == 0)
			boot_ui_log_allocations("first_screen");
		/* (diagnosis) the allocation's bytes at every sampled screen */
		if (boot_ui.config.dump_allocation >= 0 &&
			boot_ui.config.dump_allocation < real_map_allocation_count())
		{
			char path[64];
			FILE *dump;

			_snprintf(path, sizeof(path) - 1, "d:\\boot_dump_%05ld.bin", boot_ui.frame);
			path[sizeof(path) - 1] = 0;
			dump = fopen(path, "wb");
			if (dump)
			{
				fwrite(real_map_allocation_address(boot_ui.config.dump_allocation), 1,
					real_map_allocation_bytes(boot_ui.config.dump_allocation), dump);
				fclose(dump);
			}
		}
	}
}

/* ---------- the diagnostic line */

static void boot_ui_overlay(
	void)
{
	char line[160] = { 0 };
	char const *refused_map = NULL;
	char const *refused_reason = NULL;
	long refusals = wii_boot_map_refusals(&refused_map, &refused_reason);

	wii_ui_gx_window(0, 0, BOOT_UI_SCREEN_WIDTH, BOOT_UI_SCREEN_HEIGHT);
	wii_ui_gx_fill(0, 462, BOOT_UI_SCREEN_WIDTH, 480, 0xA0000000u);
	if (boot_ui.config.overlay)
		wii_ui_gx_text(4, 465, 1, 0xFFC8C8C8u, boot_ui.config.overlay);
	if (boot_ui.map_name[0] && !boot_ui_in_menu_map() && boot_ui.campaign)
	{
		/* HWI-033: the campaign runs; its environment is not drawn (HWI-030:
		a BSP's geometry does not fit the memory left), and says so */
		_snprintf(line, sizeof(line) - 1, "MAP %s: CAMPAIGN - ENVIRONMENT DRAWING PENDING (HWI-030)", boot_ui.map_name);
		wii_ui_gx_fill(0, 0, BOOT_UI_SCREEN_WIDTH, 14, 0xA0000000u);
		wii_ui_gx_text(4, 3, 1, 0xFFFFD040u, line);
		wii_ui_gx_fill(120, 200, 520, 268, 0xC0202020u);
		wii_ui_gx_text(136, 210, 2, 0xFFFFFFFFu, "CAMPAIGN ENVIRONMENT");
		wii_ui_gx_text(136, 228, 2, 0xFFFFFFFFu, "DRAWING PENDING (HWI-030)");
		_snprintf(line, sizeof(line) - 1, "THE GAME RUNS: TICK %ld, NOT DRAWN", boot_ui.target_ticks);
		wii_ui_gx_text(136, 250, 1, 0xFFFFE0A0u, line);
	}
	else if (boot_ui.map_name[0] && !boot_ui_in_menu_map())
	{
		/* a multiplayer map is never the campaign */
		_snprintf(line, sizeof(line) - 1, "MAP %s: MULTIPLAYER ENVIRONMENT (DIAGNOSTIC) - NOT THE CAMPAIGN",
			boot_ui.map_name);
		wii_ui_gx_fill(0, 0, BOOT_UI_SCREEN_WIDTH, 14, 0xA0000000u);
		wii_ui_gx_text(4, 3, 1, 0xFFFFD040u, line);
	}
	if (refusals > 0)
	{
		_snprintf(line, sizeof(line) - 1, "MAP %s NOT LOADED (%ld): SEE THE MESSAGE; STAGE IT WITH MAP_STAGE.PY",
			refused_map, refusals);
		wii_ui_gx_fill(0, 448, BOOT_UI_SCREEN_WIDTH, 462, 0xC0400000u);
		wii_ui_gx_text(4, 451, 1, 0xFFFFFFFFu, line);
	}
}

/* ---------- the hooks */

static int boot_ui_rasterizer_initialize(
	void)
{
	boolean text;

	wii_render_environment_boot();
	rasterizer_globals.reserved04.screen_bounds.x0 = 0;
	rasterizer_globals.reserved04.screen_bounds.y0 = 0;
	rasterizer_globals.reserved04.screen_bounds.x1 = BOOT_UI_SCREEN_WIDTH;
	rasterizer_globals.reserved04.screen_bounds.y1 = BOOT_UI_SCREEN_HEIGHT;
	rasterizer_globals.reserved04.frame_bounds.x0 = BOOT_UI_FRAME_X0;
	rasterizer_globals.reserved04.frame_bounds.y0 = BOOT_UI_FRAME_Y0;
	rasterizer_globals.reserved04.frame_bounds.x1 = BOOT_UI_FRAME_X1;
	rasterizer_globals.reserved04.frame_bounds.y1 = BOOT_UI_FRAME_Y1;
	rasterizer_globals.fps_accumulation_frame_index = 1;
	/* the frame is paced by the engine's own wait (main_update_time) and
	the copy's retrace, not by a vertical blank counter */
	rasterizer_globals.framerate_throttle = FALSE;
	text = rasterizer_text_cache_initialize();
	rasterizer_globals.initialized = TRUE;
	boot_ui.initialized = TRUE;
	wii_log("BOOT_UI_INIT rasterizer=wii_gx_boot screen=%d,%d frame=%d,%d,%d,%d text_cache=%d "
		"framerate_throttle=%d frame_rate_throttle=%d\n", BOOT_UI_SCREEN_WIDTH, BOOT_UI_SCREEN_HEIGHT,
		BOOT_UI_FRAME_X0, BOOT_UI_FRAME_Y0, BOOT_UI_FRAME_X1, BOOT_UI_FRAME_Y1, text,
		rasterizer_globals.framerate_throttle, global_frame_rate_throttle);
	wii_unsupported("render", "console window, HUD and game engine overlays (boot mode)");

	return text;
}

static void boot_ui_rasterizer_dispose(
	void)
{
	rasterizer_text_cache_dispose();
	boot_ui.initialized = FALSE;
}

static void boot_ui_window_begin(
	void const *parameters)
{
	struct rasterizer_window_begin_parameters const *window = parameters;
	rectangle2d const *bounds = &window->camera.viewport_bounds;

	global_window_parameters = *window;
	wii_ui_gx_window(bounds->x0, bounds->y0, bounds->x1, bounds->y1);
}

static void boot_ui_window_end(
	void)
{
	return;
}

static void boot_ui_interface(
	struct render_window const *window,
	short local_player_index,
	short window_index)
{
	struct rasterizer_window_begin_parameters parameters;

	csmemset(&parameters, 0, sizeof(parameters));
	render.camera = window->render_camera;
	render_camera_build_frustum(&render.camera, NULL, &render.frustum, TRUE);
	parameters.camera = window->rasterizer_camera;
	render_camera_build_frustum(&parameters.camera, NULL, &parameters.frustum, TRUE);
	parameters.rasterizer_target = 0;
	parameters.window_index = window_index;
	rasterizer_window_begin(&parameters);
	halo_screen_ui_offset(TRUE);
	render_ui_widgets(local_player_index, &window->rasterizer_camera.viewport_bounds);
	halo_screen_ui_offset(FALSE);
	bink_playback_render();
	rasterizer_window_end();
}

static void boot_ui_frame(
	void *windows,
	short window_count,
	float dt)
{
	struct render_window *windoze = windows;
	short window_index;

	render.frame_index++;
	render.time_delta_since_tick_sec = dt;
	boot_ui_map_current();
	boot_ui.environment_drawn = FALSE;
	boot_ui.environment_pending = FALSE;
	for (window_index = 0; window_index < window_count; window_index++)
	{
		struct render_window *window = &windoze[window_index];
		rectangle2d const *bounds = &window->rasterizer_camera.viewport_bounds;

		render.window_index = window_index;
		if (window->console_window)
		{
			boot_ui.console_windows++;
			continue;
		}
		if (window->local_player_index == NONE)
		{
			boot_ui.nonplayer_windows++;
			continue;
		}
		boot_ui.player_windows++;
		/* the environment from the window's own camera, then the interface */
		wii_ui_gx_window(bounds->x0, bounds->y0, bounds->x1, bounds->y1);
		if (boot_ui.campaign)
		{
			/* HWI-033: a campaign map's environment is not drawn until its
			BSPs fit (HWI-030); it is not loaded either, so nothing is taken
			from the memory the game runs in. The window stays clear and the
			overlay says why */
			if (!boot_ui.pending_logged)
			{
				wii_log("BOOT_ENVIRONMENT_PENDING map=%s frame=%ld reason=campaign_bsp_geometry_does_not_fit "
					"ticket=HWI-030\n", boot_ui.map_name, boot_ui.frame);
			}
			boot_ui.pending_logged = TRUE;
			boot_ui.environment_pending = TRUE;
		}
		else if (wii_render_environment_draw(&window->render_camera))
			boot_ui.environment_drawn = TRUE;
		render.local_player_index = window->local_player_index;
		boot_ui_interface(window, window->local_player_index, window_index);
	}
	halo_screen_ui_offset(TRUE);
	progress_bar_eachframe();
	halo_screen_ui_offset(FALSE);
}

static void boot_ui_frame_pregame(
	void const *window)
{
	struct render_window const *pregame = window;
	real progress;

	render.frame_index++;
	boot_ui.pregame_frames++;
	boot_ui_map_current();
	boot_ui_interface(pregame, 0, 0);
	if (game_map_loading_in_progress(&progress))
		progress_bar_display(progress);
}

static void boot_ui_present(
	void)
{
	boot_ui_screen_check();
	boot_ui_overlay();
	/* (HWI-033: a campaign map's frames count too, its drawing pending) */
	if ((boot_ui.environment_drawn || boot_ui.environment_pending) && boot_ui.map_name[0] && !boot_ui_in_menu_map())
	{
		boot_ui.environment_frames++;
		if (boot_ui.environment_frames == 1)
		{
			int covered = 0;
			unsigned long crc = wii_ui_gx_sample_crc(&covered);
			unsigned long state = wii_boot_state_digest(boot_ui.state_digests, BOOT_UI_MAXIMUM_STATE_ALLOCATIONS);

			wii_log("BOOT_ENVIRONMENT first frame=%ld clock_ms=%lu map=%s efb_crc=%08lx covered=%d state=%08lx "
				"game_time=%ld drawing=%s\n", boot_ui.frame, wii_boot_clock_milliseconds(), boot_ui.map_name, crc,
				covered, state, game_in_progress() ? game_time_get() : -1L,
				boot_ui.environment_pending ? "pending_hwi030" : "drawn");
			boot_ui_log_allocations("first_environment");
		}
	}
	wii_ui_gx_present();
	boot_ui.frame++;
	if (boot_ui.config.environment_frames > 0 && boot_ui.environment_frames >= boot_ui.config.environment_frames)
	{
		int covered = 0;
		unsigned long state = wii_boot_state_digest(boot_ui.state_digests, BOOT_UI_MAXIMUM_STATE_ALLOCATIONS);

		wii_log("BOOT_ENVIRONMENT end frames=%ld map=%s state=%08lx game_time=%ld covered_last=%d ticks=%ld "
			"tick_digests=%ld drawing=%s\n", boot_ui.environment_frames, boot_ui.map_name, state,
			game_in_progress() ? game_time_get() : -1L, covered, boot_ui.target_ticks, boot_ui.tick_digests,
			boot_ui.campaign ? "pending_hwi030" : "drawn");
		boot_ui_finish(1, boot_ui.campaign ? "campaign_reached_drawing_pending" : "environment_reached");
	}
	else if (boot_ui.config.max_frames > 0 && boot_ui.frame >= boot_ui.config.max_frames)
	{
		if (boot_ui.config.expect_refusal)
		{
			/* the load-failure test: a map was refused and the menus are back */
			boolean recovered = wii_boot_map_refusals(NULL, NULL) > 0 && boot_ui_in_menu_map();

			wii_log("BOOT_LOAD_FAILURE refusals=%ld map=%s recovered=%d\n", wii_boot_map_refusals(NULL, NULL),
				boot_ui.map_name, recovered);
			boot_ui_finish(recovered, recovered ? "load_failure_recovered" : "load_failure_not_recovered");
		}
		else
		{
			boot_ui_finish(boot_ui.config.environment_frames > 0 ? 0 : 1,
				boot_ui.config.environment_frames > 0 ? "max_frames_before_environment" : "max_frames");
		}
	}
}

/* HWI-033: a map's texture coordinates as the Xbox's screen-geometry vertex
shader makes them (vertex_shader_table[3], translated by the Linux port's
nv2a_vsh.c): the vertex's position in the window when the map is anchored to
the screen (without parameters->offset), its texture coordinates otherwise;
times the map's scale, plus its offset, times its texture scale */
static void boot_ui_map_coordinates(
	struct rasterizer_dynamic_screen_geometry_parameters const *parameters,
	struct dynamic_screen_vertex const *vertex,
	short map_index,
	float *s,
	float *t)
{
	real base_x = parameters->map_anchor_screen[map_index] ? vertex->position.x : vertex->texture_coordinates.x;
	real base_y = parameters->map_anchor_screen[map_index] ? vertex->position.y : vertex->texture_coordinates.y;
	real offset_x = parameters->map_offset[map_index] ? parameters->map_offset[map_index]->x : 0.0f;
	real offset_y = parameters->map_offset[map_index] ? parameters->map_offset[map_index]->y : 0.0f;

	*s = (base_x * parameters->map_scale[map_index].i + offset_x) * parameters->map_texture_scale[map_index].i;
	*t = (base_y * parameters->map_scale[map_index].j + offset_y) * parameters->map_texture_scale[map_index].j;
}

/* HWI-033: the plasma effect (wii_render_ui.h wii_ui_gx_plasma): the three
maps' textures and coordinates, their colours as the rasterizer's
constants hold them (real_argb_color_to_pixel32: tint, else white; fade,
else 1) and the plasma fade */
static void boot_ui_plasma_quad(
	struct rasterizer_dynamic_screen_geometry_parameters const *parameters,
	struct dynamic_screen_vertex const *vertices)
{
	struct wii_ui_plasma plasma;
	struct wii_ui_plasma_vertex quad[4];
	real offset_x = parameters->offset ? parameters->offset->i : 0.0f;
	real offset_y = parameters->offset ? parameters->offset->j : 0.0f;
	short map_index;
	short vertex_index;

	csmemset(&plasma, 0, sizeof(plasma));
	for (map_index = 0; map_index < 3; map_index++)
	{
		real_rgb_color const *tint = parameters->map_tint[map_index] ? parameters->map_tint[map_index] :
			global_real_rgb_white;
		real_argb_color colour;
		pixel32 packed;

		plasma.textures[map_index] = boot_ui_texture(parameters->map[map_index]);
		if (plasma.textures[map_index] < 0)
		{
			/* a bitmap not on the card is not drawn (reported by the GX side) */
			boot_ui.plasma_missing++;
			return;
		}
		plasma.wrap[map_index] = parameters->map_wrapped[map_index];
		colour.rgb = *tint;
		colour.alpha = parameters->map_fade[map_index] ? *parameters->map_fade[map_index] : 1.0f;
		packed = real_argb_color_to_pixel32(&colour);
		if (map_index < 2)
		{
			plasma.map_fade[map_index] = (uint8_t)(packed >> 24);
			plasma.map_tint_blue[map_index] = (uint8_t)packed;
		}
		else
		{
			plasma.map2_argb = (uint32_t)packed;
		}
	}
	plasma.plasma_fade = (uint32_t)real_argb_color_to_pixel32(&parameters->plasma_fade) & 0x00FFFFFFu;
	plasma.point_sampled = parameters->point_sampled;
	plasma.blend = PIN(parameters->framebuffer_blend_function, 0, WII_UI_BLEND_COUNT - 1);
	for (vertex_index = 0; vertex_index < 4; vertex_index++)
	{
		quad[vertex_index].x = vertices[vertex_index].position.x + offset_x;
		quad[vertex_index].y = vertices[vertex_index].position.y + offset_y;
		for (map_index = 0; map_index < 3; map_index++)
		{
			boot_ui_map_coordinates(parameters, &vertices[vertex_index], map_index,
				&quad[vertex_index].st[map_index][0], &quad[vertex_index].st[map_index][1]);
		}
		quad[vertex_index].argb = (uint32_t)vertices[vertex_index].color;
	}
	wii_ui_gx_plasma(&plasma, quad, 1);
	boot_ui.plasma_composited++;
	boot_ui.screen_quads++;
}

static void boot_ui_screen_quad(
	void *parameters_pointer,
	void *vertices_pointer)
{
	struct rasterizer_dynamic_screen_geometry_parameters *parameters = parameters_pointer;
	struct dynamic_screen_vertex *vertices = vertices_pointer;
	struct wii_ui_vertex quad[4];
	struct wii_ui_draw draw;
	short map_index = 0;
	real offset_x, offset_y;
	short vertex_index;

	if (!rasterizer_debug_options.draw_dynamic_screen_geometry ||
		global_window_parameters.rasterizer_target != 0)
	{
		return;
	}
	if (parameters->doing_plasma_effect && parameters->map[0] && parameters->map[1] && parameters->map[2] &&
		!parameters->meter_parameters && parameters->map0_to_1_blend_function == WII_UI_BLEND_MIN &&
		parameters->map1_to_2_blend_function == WII_UI_BLEND_ALPHA)
	{
		/* HWI-033: the plasma composited as the Xbox's combiners do */
		boot_ui_plasma_quad(parameters, vertices);
		return;
	}
	if (parameters->map[1])
	{
		/* another combination of maps (none in the menus): the first map's
		layer is drawn, counted */
		map_index = parameters->doing_plasma_effect && parameters->map[2] ? 2 : 0;
		boot_ui.plasma_approximated++;
	}
	if (parameters->meter_parameters)
		boot_ui.meter_approximated++;
	csmemset(&draw, 0, sizeof(draw));
	draw.texture = boot_ui_texture(parameters->map[map_index]);
	if (parameters->map[map_index] && draw.texture < 0)
	{
		/* a bitmap not on the card is not drawn (reported by the GX side) */
		return;
	}
	if (draw.texture < 0)
		boot_ui.untextured_quads++;
	draw.blend = PIN(parameters->framebuffer_blend_function, 0, WII_UI_BLEND_COUNT - 1);
	draw.wrap = parameters->map_wrapped[map_index];
	draw.point_sampled = parameters->point_sampled;
	draw.alpha_weighted = parameters->alpha_weighted;
	offset_x = parameters->offset ? parameters->offset->i : 0.0f;
	offset_y = parameters->offset ? parameters->offset->j : 0.0f;
	for (vertex_index = 0; vertex_index < 4; vertex_index++)
	{
		quad[vertex_index].x = vertices[vertex_index].position.x + offset_x;
		quad[vertex_index].y = vertices[vertex_index].position.y + offset_y;
		boot_ui_map_coordinates(parameters, &vertices[vertex_index], map_index, &quad[vertex_index].u,
			&quad[vertex_index].v);
		quad[vertex_index].argb = boot_ui_modulate(vertices[vertex_index].color, parameters->map_tint[map_index],
			parameters->map_fade[map_index]);
	}
	wii_ui_gx_quads(&draw, quad, 1);
	boot_ui.screen_quads++;
}

static void boot_ui_text_flush(
	void)
{
	if (boot_ui.text_quad_count)
		wii_ui_gx_quads(&boot_ui.text_draw, boot_ui.text_quads, boot_ui.text_quad_count);
	boot_ui.text_quad_count = 0;
}

static void boot_ui_text_begin(
	void const *parameters_pointer)
{
	struct rasterizer_dynamic_screen_geometry_parameters const *parameters = parameters_pointer;

	boot_ui.text_quad_count = 0;
	csmemset(&boot_ui.text_draw, 0, sizeof(boot_ui.text_draw));
	boot_ui.text_bitmap = parameters->map[0];
	boot_ui.text_draw.texture = boot_ui_texture(parameters->map[0]);
	boot_ui.text_draw.blend = PIN(parameters->framebuffer_blend_function, 0, WII_UI_BLEND_COUNT - 1);
	boot_ui.text_draw.point_sampled = parameters->point_sampled;
	boot_ui.text_draw.alpha_weighted = parameters->alpha_weighted;
	boot_ui.text_scale_u = parameters->map_texture_scale[0].i;
	boot_ui.text_scale_v = parameters->map_texture_scale[0].j;
}

static void boot_ui_text_character(
	void const *vertices_pointer)
{
	struct dynamic_screen_vertex const *vertices = vertices_pointer;
	struct wii_ui_vertex *quad;
	short vertex_index;

	if (boot_ui.text_draw.texture < 0)
		return;
	if (boot_ui.text_quad_count == BOOT_UI_MAXIMUM_TEXT_QUADS)
		boot_ui_text_flush();
	/* (this character was just cached: the texture follows the bitmap) */
	if (boot_ui.text_bitmap)
	{
		struct boot_ui_dynamic_bitmap *slot = boot_ui_dynamic_find(boot_ui.text_bitmap);

		if (slot && slot->dirty)
		{
			boot_ui_text_flush();
			boot_ui_texture(boot_ui.text_bitmap);
		}
	}
	quad = &boot_ui.text_quads[boot_ui.text_quad_count * 4];
	for (vertex_index = 0; vertex_index < 4; vertex_index++)
	{
		quad[vertex_index].x = vertices[vertex_index].position.x;
		quad[vertex_index].y = vertices[vertex_index].position.y;
		quad[vertex_index].u = vertices[vertex_index].texture_coordinates.x * boot_ui.text_scale_u;
		quad[vertex_index].v = vertices[vertex_index].texture_coordinates.y * boot_ui.text_scale_v;
		quad[vertex_index].argb = (uint32_t)vertices[vertex_index].color;
	}
	boot_ui.text_quad_count++;
	boot_ui.text_characters++;
}

static void boot_ui_text_end(
	void)
{
	boot_ui_text_flush();
}

static int boot_ui_bitmap_new(
	void *bitmap_pointer)
{
	struct bitmap_data *bitmap = bitmap_pointer;
	struct boot_ui_dynamic_bitmap *slot = boot_ui_dynamic_find(NULL);

	/* A4R4G4B4 (format 9), 2D, as the glyph cache and the high-res text's
	atlas are made (rasterizer_text.c); no mipmaps */
	bitmap->mipmap_count = 0;
	if (!slot || bitmap->type != 0 || bitmap->format != 9 || !bitmap->base_address)
	{
		wii_log("BOOT_UI_BITMAP_NEW refused type=%d format=%d %dx%d\n", bitmap->type, bitmap->format, bitmap->width,
			bitmap->height);
		return FALSE;
	}
	/* (bitmap_2d_new leaves the pixels as the heap had them: the cache's
	unused cells are made clear, so every launch samples the same texels) */
	csmemset(bitmap->base_address, 0, (long)bitmap->width * bitmap->height * 2);
	slot->handle = wii_ui_gx_texture_dynamic(bitmap->width, bitmap->height);
	if (slot->handle < 0)
		return FALSE;
	slot->bitmap = bitmap;
	slot->dirty = TRUE;
	slot->updates = 0;
	bitmap->hardware_format = slot;
	wii_log("BOOT_UI_BITMAP_NEW glyph_cache %dx%d format=a4r4g4b4 gx=ia4 handle=%ld\n", bitmap->width, bitmap->height,
		slot->handle);

	return TRUE;
}

static void boot_ui_bitmap_changed(
	void *bitmap_pointer)
{
	struct boot_ui_dynamic_bitmap *slot = boot_ui_dynamic_find(bitmap_pointer);

	if (slot)
	{
		/* (the queued characters were drawn from the cache as it was: they
		go first; the texture is copied when the next character is drawn) */
		if (boot_ui.text_quad_count && boot_ui.text_draw.texture == slot->handle)
			boot_ui_text_flush();
		slot->dirty = TRUE;
	}
}

static void boot_ui_bitmap_delete(
	void *bitmap_pointer)
{
	struct boot_ui_dynamic_bitmap *slot = boot_ui_dynamic_find(bitmap_pointer);

	if (slot)
	{
		wii_ui_gx_texture_release(slot->handle);
		csmemset(slot, 0, sizeof(*slot));
		((struct bitmap_data *)bitmap_pointer)->hardware_format = NULL;
	}
}

/* HWI-033: after each of the engine's ticks (engine_hooks.h): the target
map's ticks are counted, and the first tick_digests of them each log the
game state's digest (BOOT_TICK), which the i686 host reference replaying the
launch logs too */
static void boot_ui_after_tick(
	void)
{
	unsigned long state;

	if (!boot_ui.initialized || boot_ui.finished)
		return;
	boot_ui_map_current();
	if (!boot_ui.map_name[0] || boot_ui_in_menu_map())
		return;
	boot_ui.target_ticks++;
	if (boot_ui.tick_digests >= boot_ui.config.tick_digests)
		return;
	state = wii_boot_state_digest(boot_ui.state_digests, BOOT_UI_MAXIMUM_STATE_ALLOCATIONS);
	boot_ui.tick_digests++;
	wii_log("BOOT_TICK map=%s tick=%ld game_time=%ld frame=%ld state=%08lx state_without_scripts=%08lx "
		"bsp=%d\n", boot_ui.map_name, boot_ui.target_ticks, game_in_progress() ? game_time_get() : -1L,
		boot_ui.frame, state, boot_ui_state_without_scripts(), (int)global_structure_bsp_index);
}

/* ---------- public code */

void wii_render_ui_register(
	const struct wii_boot_ui_config *config)
{
	struct wii_boot_render_hooks hooks;

	csmemset(&boot_ui, 0, sizeof(boot_ui));
	boot_ui.config = *config;
	if (boot_ui.config.screen_settle_frames <= 0)
		boot_ui.config.screen_settle_frames = 45;
	boot_ui.scenario_index = NONE;
	boot_ui.current.screen_tag = NONE;
	boot_ui.current.focused_tag = NONE;
	csmemset(&hooks, 0, sizeof(hooks));
	hooks.rasterizer_initialize = boot_ui_rasterizer_initialize;
	hooks.rasterizer_dispose = boot_ui_rasterizer_dispose;
	hooks.frame = boot_ui_frame;
	hooks.frame_pregame = boot_ui_frame_pregame;
	hooks.present = boot_ui_present;
	hooks.window_begin = boot_ui_window_begin;
	hooks.window_end = boot_ui_window_end;
	hooks.screen_quad = boot_ui_screen_quad;
	hooks.text_begin = boot_ui_text_begin;
	hooks.text_character = boot_ui_text_character;
	hooks.text_end = boot_ui_text_end;
	hooks.bitmap_new = boot_ui_bitmap_new;
	hooks.bitmap_changed = boot_ui_bitmap_changed;
	hooks.bitmap_delete = boot_ui_bitmap_delete;
	wii_boot_set_render_hooks(&hooks);
	wii_boot_set_map_check(config->map_check);
	wii_engine_set_bitmap_hardware_format(boot_ui_bitmap_hardware_format);
	wii_engine_set_tick_observer(boot_ui_after_tick);
}

void wii_render_ui_report(
	void)
{
	long index;

	if (boot_ui.current.frames > 0 && boot_ui.screen_count < BOOT_UI_MAXIMUM_SCREENS)
		boot_ui.screens[boot_ui.screen_count++] = boot_ui.current;
	wii_log("BOOT_UI_TOTALS frames=%ld pregame_frames=%lu player_windows=%lu console_windows=%lu nonplayer_windows=%lu "
		"screen_quads=%lu untextured_quads=%lu text_characters=%lu plasma_composited=%lu plasma_missing=%lu "
		"multimap_approximated=%lu meter_approximated=%lu "
		"missing_bitmaps=%lu map_changes=%ld environment_frames=%ld screens=%ld lookups=%ld\n", boot_ui.frame,
		boot_ui.pregame_frames, boot_ui.player_windows, boot_ui.console_windows, boot_ui.nonplayer_windows,
		boot_ui.screen_quads, boot_ui.untextured_quads, boot_ui.text_characters, boot_ui.plasma_composited,
		boot_ui.plasma_missing, boot_ui.plasma_approximated,
		boot_ui.meter_approximated, boot_ui.missing_bitmaps, boot_ui.map_changes, boot_ui.environment_frames,
		boot_ui.screen_count, boot_ui.lookup_count);
	for (index = 0; index < BOOT_UI_MAXIMUM_DYNAMIC_BITMAPS; index++)
	{
		if (boot_ui.dynamic[index].bitmap)
		{
			wii_log("BOOT_UI_GLYPH_CACHE updates=%lu %dx%d\n", boot_ui.dynamic[index].updates,
				boot_ui.dynamic[index].bitmap->width, boot_ui.dynamic[index].bitmap->height);
		}
	}
	for (index = 0; index < boot_ui.screen_count; index++)
	{
		wii_log("BOOT_UI_SCREEN_SPAN index=%ld screen_tag=%ld focused_tag=%ld first_frame=%ld frames=%ld sampled=%d\n",
			index, boot_ui.screens[index].screen_tag == NONE ? -1L :
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(boot_ui.screens[index].screen_tag),
			boot_ui.screens[index].focused_tag == NONE ? -1L :
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(boot_ui.screens[index].focused_tag),
			boot_ui.screens[index].first_frame, boot_ui.screens[index].frames, boot_ui.screens[index].sampled);
	}
	wii_ui_gx_report();
}
