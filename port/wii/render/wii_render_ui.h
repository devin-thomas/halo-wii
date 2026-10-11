/*
WII_RENDER_UI.H

HWI-016D: the boot mode's rasterizer: the engine's own interface (its
widgets' bitmaps and its text, drawn through the engine's own
render_ui_widgets and draw_string) and the environment, from the engine's
own main_loop, through GX. Three parts, one interface (C types only):

- render_ui.c, an engine unit: the boot hooks (port/wii/engine/boot_hooks.h)
  in the engine's terms: main_loop's render_frame for its windows, the
  window's viewport, the screen quads of the interface (their bitmaps: tag
  and bitmap index), the text from the engine's own glyph cache, which
  screen is up, the frame's end;
- render_ui_gx.c, a driver unit (libogc): the 2D GX state, the interface's
  textures (HWT1 from the card, by tag and bitmap, in a bounded cache), the
  glyph cache's texture, the built-in diagnostic font, the frame copy and
  the EFB samples;
- the boot driver (port/wii/engine/boot_main.c): the boot configuration, the
  manifests, the pad, the load-failure screens, and the end of the launch.

Converted textures are read from the card (tools/wii/boot_stage.py, private):
  WII_ENGINE_DATA_ROOT/render/<map>/textures/<bitmap tag:05>-<bitmap:03>.hwt
*/

#ifndef __HALO_WII_RENDER_UI_H
#define __HALO_WII_RENDER_UI_H

#include <stdint.h>

/* the interface's texture budget (HWT1 bytes resident), pending the Wii
texture cache (HWI-032) */
#define WII_UI_DEFAULT_TEXTURE_BUDGET (3u * 1024u * 1024u)

/* ---------- boot configuration (driver -> engine side) */

struct wii_boot_ui_config
{
	unsigned long texture_budget;
	long max_frames;            /* the launch ends (fail) after this many presented frames */
	long environment_frames;    /* ends (pass) this many frames after a map other than the menu's draws */
	long screen_settle_frames;  /* a screen is sampled once it has stood this many frames */
	const char *overlay;        /* the manifest line drawn at the bottom of every frame */
	/* the driver ends the launch: result 1 pass, 0 fail, with the reason */
	void (*finish)(int result, const char *reason);
	/* the map a load asks for: 0 if it may load, else the reason it may not
	(a manifest mismatch: the file is not the one the manifest names) */
	int (*map_check)(const char *map_name, char *reason, unsigned long reason_size);
};

/* registers the boot hooks (boot_hooks.h) and the environment's boot mode */
void wii_render_ui_register(const struct wii_boot_ui_config *config);
/* the launch's totals (BOOT_UI_TOTALS) */
void wii_render_ui_report(void);

/* ---------- engine side -> GX side */

struct wii_render_video;

/* 2D vertex: screen position (pixels of the window), texture coordinates
(normalised), colour (ARGB) */
struct wii_ui_vertex
{
	float x, y;
	float u, v;
	uint32_t argb;
};

/* the engine's framebuffer blend functions (shader_framebuffer_blend_function) */
enum
{
	WII_UI_BLEND_ALPHA = 0,
	WII_UI_BLEND_MULTIPLY,
	WII_UI_BLEND_DOUBLE_MULTIPLY,
	WII_UI_BLEND_ADD,
	WII_UI_BLEND_SUBTRACT,
	WII_UI_BLEND_MIN,
	WII_UI_BLEND_MAX,
	WII_UI_BLEND_ALPHA_MULTIPLY_ADD,
	WII_UI_BLEND_COUNT
};

struct wii_ui_draw
{
	long texture;           /* a handle, or -1 for the vertex colour alone */
	int blend;              /* WII_UI_BLEND_* */
	int wrap;               /* repeat (else clamp) */
	int point_sampled;
	int alpha_weighted;     /* colour times its alpha before the blend */
};

int wii_ui_gx_configure(const struct wii_render_video *video, unsigned long texture_budget);
/* the window's viewport (screen pixels); 2D drawing is in its pixels */
void wii_ui_gx_window(int x0, int y0, int x1, int y1);
/* a bitmap of the loaded map's: its HWT1 from the card (width and height
are the engine's, checked); a handle, or -1 if it is not there, does not
fit or is not loadable (reported once each) */
long wii_ui_gx_texture_tag(const char *map, long tag, long bitmap, int width, int height);
/* the engine's own bitmaps (its glyph cache): A4R4G4B4, made into an IA4
texture; a handle or -1 */
long wii_ui_gx_texture_dynamic(int width, int height);
/* the bitmap's texels changed (A4R4G4B4, native 16-bit words, row by row) */
void wii_ui_gx_texture_dynamic_update(long handle, const uint16_t *texels);
void wii_ui_gx_texture_release(long handle);
/* a map's textures let go (its map is unloaded) */
void wii_ui_gx_release_map_textures(void);
void wii_ui_gx_quads(const struct wii_ui_draw *draw, const struct wii_ui_vertex *vertices, int quad_count);
/* the diagnostic font (built in: no game data), uppercase ASCII */
void wii_ui_gx_text(int x, int y, int scale, uint32_t argb, const char *text);
void wii_ui_gx_fill(int x0, int y0, int x1, int y1, uint32_t argb);
/* CRC of a 32x24 EFB sample grid (EFB access must be on), and the samples
that are not black */
uint32_t wii_ui_gx_sample_crc(int *covered);
/* the frame copied to the screen, waiting for the retrace */
void wii_ui_gx_present(void);
void wii_ui_gx_report(void);

#endif
