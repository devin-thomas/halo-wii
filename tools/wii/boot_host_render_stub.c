/* The boot mode's GX side and environment path for the i686 host reference
 * (HWI-016D, tools/wii/run_boot_host.py): nothing is drawn. The engine side
 * (port/wii/render/render_ui.c) runs as on the Wii; the textures it asks for
 * are not there (it then draws nothing of them, as for a bitmap not on the
 * card), its samples are 0, and the environment answers that it drew, so a
 * map other than the menus' counts its frames as the Wii's does. */
#include <stdint.h>

#include "../../port/wii/render/wii_render.h"
#include "../../port/wii/render/wii_render_ui.h"

int wii_ui_gx_configure(const struct wii_render_video *video, unsigned long texture_budget)
{
    (void)video;
    (void)texture_budget;
    return 1;
}

void wii_ui_gx_window(int x0, int y0, int x1, int y1)
{
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
}

long wii_ui_gx_texture_tag(const char *map, long tag, long bitmap, int width, int height)
{
    (void)map;
    (void)tag;
    (void)bitmap;
    (void)width;
    (void)height;
    return -1;
}

long wii_ui_gx_texture_dynamic(int width, int height)
{
    (void)width;
    (void)height;
    return 0;
}

void wii_ui_gx_texture_dynamic_update(long handle, const uint16_t *texels)
{
    (void)handle;
    (void)texels;
}

void wii_ui_gx_texture_release(long handle)
{
    (void)handle;
}

void wii_ui_gx_release_map_textures(void)
{
}

void wii_ui_gx_quads(const struct wii_ui_draw *draw, const struct wii_ui_vertex *vertices, int quad_count)
{
    (void)draw;
    (void)vertices;
    (void)quad_count;
}

void wii_ui_gx_plasma(const struct wii_ui_plasma *plasma, const struct wii_ui_plasma_vertex *vertices, int quad_count)
{
    (void)plasma;
    (void)vertices;
    (void)quad_count;
}

int wii_ui_gx_selftest(void)
{
    return 0;
}

void wii_ui_gx_text(int x, int y, int scale, uint32_t argb, const char *text)
{
    (void)x;
    (void)y;
    (void)scale;
    (void)argb;
    (void)text;
}

void wii_ui_gx_fill(int x0, int y0, int x1, int y1, uint32_t argb)
{
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)argb;
}

uint32_t wii_ui_gx_sample_crc(int *covered)
{
    *covered = 0;
    return 0;
}

void wii_ui_gx_present(void)
{
}

void wii_ui_gx_report(void)
{
}

void wii_render_environment_boot(void)
{
}

int wii_render_environment_draw(const void *render_camera)
{
    (void)render_camera;
    return 1;
}
