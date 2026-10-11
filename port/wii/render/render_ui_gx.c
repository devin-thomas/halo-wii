/*
 * RENDER_UI_GX.C
 *
 * HWI-016D: the GX side of the boot mode's interface drawing
 * (wii_render_ui.h). Strict C11 against libogc; no engine header.
 *
 * - 2D: a window's viewport in screen pixels, an orthographic projection of
 *   its pixels, no depth, no culling; one TEV stage (the texture times the
 *   vertex colour, or the vertex colour alone), a second for the
 *   alpha-weighted blend; the engine's framebuffer blend functions as GX
 *   blend modes (min and max have none: drawn as the alpha blend, counted).
 * - The interface's bitmaps: HWT1 files converted by the content pipeline,
 *   read from the card when the engine first draws them and kept while they
 *   fit the budget, least recently drawn released first. A bitmap that is
 *   not on the card, does not fit, or differs from the engine's dimensions
 *   is not drawn and is reported once.
 * - The engine's glyph cache: its A4R4G4B4 bitmap (rasterizer_text.c writes
 *   white texels whose alpha nibble is the glyph's coverage) becomes an IA4
 *   texture, the same 4-bit alpha and white intensity, re-tiled when the
 *   engine changes it.
 * - A built-in 5x7 diagnostic font (authored here, no game data) for the
 *   manifest line and the load-failure screens.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wii_platform.h"
#include "wii_render.h"
#include "wii_render_ui.h"
#include "content_common.h"
#include "hwt_texture.h"

#define UI_RENDER_ROOT WII_ENGINE_DATA_ROOT "/render"
#define UI_MAX_TEXTURES 512
#define UI_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define UI_GRID_X 32
#define UI_GRID_Y 24

enum texture_kind { TEXTURE_FREE, TEXTURE_TAG, TEXTURE_DYNAMIC, TEXTURE_FAILED };

struct ui_texture {
    int kind;
    char map[32];
    long tag, bitmap;
    struct content_blob blob;  /* TEXTURE_TAG: the HWT1 file */
    unsigned char *texels;     /* TEXTURE_DYNAMIC: IA4, tiled */
    uint32_t bytes;            /* resident bytes (budget) */
    int width, height, levels, gx_format;
    void *image;               /* level 0 */
    unsigned long last_frame;
    unsigned long draws;
};

static struct {
    int configured;
    GXRModeObj *mode;
    void *xfb[2];
    int fb;
    unsigned long budget, resident, resident_peak;
    unsigned long frame;
    int window_x0, window_y0, window_w, window_h;
    struct ui_texture textures[UI_MAX_TEXTURES];
    unsigned long loads, load_failures, evictions, quads, text_quads, blend_approximated, failed_draws;
    unsigned long long load_us;
} ui;

/* ---------- helpers */

static uint32_t crc_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---------- configuration */

int wii_ui_gx_configure(const struct wii_render_video *video, unsigned long texture_budget)
{
    memset(&ui, 0, sizeof(ui));
    ui.mode = (GXRModeObj *)video->mode;
    ui.xfb[0] = video->xfb[0];
    ui.xfb[1] = video->xfb[1];
    ui.budget = texture_budget ? texture_budget : WII_UI_DEFAULT_TEXTURE_BUDGET;
    ui.configured = 1;
    /* 2D vertices: screen position, colour, texture coordinates */
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    /* the menus' frames start black (the environment draws over it) */
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
    wii_ui_gx_window(0, 0, ui.mode->fbWidth, ui.mode->efbHeight);
    wii_log("BOOT_UI_GX configure budget=%lu efb=%ux%u\n", ui.budget, ui.mode->fbWidth, ui.mode->efbHeight);
    return 1;
}

void wii_ui_gx_window(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ui.mode->fbWidth) x1 = ui.mode->fbWidth;
    if (y1 > ui.mode->efbHeight) y1 = ui.mode->efbHeight;
    ui.window_x0 = x0;
    ui.window_y0 = y0;
    ui.window_w = x1 - x0;
    ui.window_h = y1 - y0;
    GX_SetViewport((f32)x0, (f32)y0, (f32)ui.window_w, (f32)ui.window_h, 0.0f, 1.0f);
    GX_SetScissor((u32)x0, (u32)y0, (u32)ui.window_w, (u32)ui.window_h);
}

/* ---------- textures */

static unsigned long texture_bytes(const struct ui_texture *t)
{
    return t->kind == TEXTURE_TAG ? t->blob.capacity : t->kind == TEXTURE_DYNAMIC ? t->bytes : 0;
}

static void texture_free(struct ui_texture *t)
{
    ui.resident -= texture_bytes(t);
    if (t->kind == TEXTURE_TAG)
        content_blob_release(&t->blob);
    if (t->kind == TEXTURE_DYNAMIC)
        free(t->texels);
    memset(t, 0, sizeof(*t));
}

/* the least recently drawn tag bitmap not drawn this frame, released; 0 if none */
static int evict_one(void)
{
    struct ui_texture *oldest = NULL;
    for (int i = 0; i < UI_MAX_TEXTURES; ++i) {
        struct ui_texture *t = &ui.textures[i];
        if (t->kind == TEXTURE_TAG && t->last_frame != ui.frame && (!oldest || t->last_frame < oldest->last_frame))
            oldest = t;
    }
    if (!oldest)
        return 0;
    texture_free(oldest);
    ui.evictions++;
    return 1;
}

static struct ui_texture *slot_new(void)
{
    for (int i = 0; i < UI_MAX_TEXTURES; ++i)
        if (ui.textures[i].kind == TEXTURE_FREE)
            return &ui.textures[i];
    return NULL;
}

static void texture_failed(const char *map, long tag, long bitmap, const char *why, unsigned long bytes)
{
    struct ui_texture *t = slot_new();
    wii_log("BOOT_UI_TEXTURE_FAIL map=%s tag=%ld bitmap=%ld reason=%s bytes=%lu resident=%lu budget=%lu\n", map, tag,
            bitmap, why, bytes, ui.resident, ui.budget);
    ui.load_failures++;
    if (!t)
        return;
    /* remembered, so it is reported and tried once */
    t->kind = TEXTURE_FAILED;
    snprintf(t->map, sizeof(t->map), "%s", map);
    t->tag = tag;
    t->bitmap = bitmap;
}

long wii_ui_gx_texture_tag(const char *map, long tag, long bitmap, int width, int height)
{
    for (int i = 0; i < UI_MAX_TEXTURES; ++i) {
        struct ui_texture *t = &ui.textures[i];
        if ((t->kind == TEXTURE_TAG || t->kind == TEXTURE_FAILED) && t->tag == tag && t->bitmap == bitmap &&
            !strcmp(t->map, map)) {
            if (t->kind == TEXTURE_FAILED)
                return -1;
            t->last_frame = ui.frame;
            return i;
        }
    }
    char path[192];
    snprintf(path, sizeof(path), UI_RENDER_ROOT "/%s/textures/%05ld-%03ld.hwt", map, tag, bitmap);
    struct content_blob blob;
    u64 started = gettime();
    enum content_error error = content_load_file(path, UI_MAX_FILE_BYTES, &blob);
    if (error != CONTENT_OK) {
        texture_failed(map, tag, bitmap, error == CONTENT_IO ? "not_on_card" : content_error_name(error), 0);
        return -1;
    }
    struct hwt_header header;
    struct hwt_image image;
    const char *why = NULL;
    error = hwt_parse(blob.data, blob.bytes, &header);
    if (error != CONTENT_OK)
        why = content_error_name(error);
    else if (header.type != HWT_TYPE_2D || header.faces != 1 || header.depth != 1)
        why = "not_2d";
    else if ((int)header.width != width || (int)header.height != height)
        why = "dimensions_differ_from_engine";
    else if (hwt_image_at(&header, 0, &image) != CONTENT_OK || !image.gx_loadable || (image.offset & 31u))
        why = "not_gx_loadable";
    if (why) {
        content_blob_release(&blob);
        texture_failed(map, tag, bitmap, why, 0);
        return -1;
    }
    if (blob.capacity > ui.budget) {
        content_blob_release(&blob);
        texture_failed(map, tag, bitmap, "larger_than_budget", blob.capacity);
        return -1;
    }
    while (ui.resident + blob.capacity > ui.budget && evict_one())
        ;
    struct ui_texture *t = slot_new();
    if (ui.resident + blob.capacity > ui.budget || !t) {
        content_blob_release(&blob);
        texture_failed(map, tag, bitmap, t ? "budget_full_this_frame" : "table_full", blob.capacity);
        return -1;
    }
    t->kind = TEXTURE_TAG;
    snprintf(t->map, sizeof(t->map), "%s", map);
    t->tag = tag;
    t->bitmap = bitmap;
    t->blob = blob;
    t->width = (int)image.width;
    t->height = (int)image.height;
    t->levels = (int)header.levels;
    t->gx_format = (int)header.gx_format;
    t->image = blob.data + image.offset;
    t->last_frame = ui.frame;
    DCFlushRange(blob.data, blob.capacity);
    ui.resident += blob.capacity;
    if (ui.resident > ui.resident_peak)
        ui.resident_peak = ui.resident;
    ui.loads++;
    unsigned long us = (unsigned long)ticks_to_microsecs(gettime() - started);
    ui.load_us += us;
    wii_log("BOOT_UI_TEXTURE map=%s tag=%ld bitmap=%ld %dx%d format=%d levels=%d bytes=%lu resident=%lu load_us=%lu\n",
            map, tag, bitmap, t->width, t->height, t->gx_format, t->levels, (unsigned long)blob.capacity, ui.resident,
            us);
    return t - ui.textures;
}

long wii_ui_gx_texture_dynamic(int width, int height)
{
    struct ui_texture *t = slot_new();
    /* IA4: 8 bits a texel, 8x4 tiles */
    if (!t || width <= 0 || height <= 0 || (width & 7) || (height & 3) || width > 1024 || height > 1024)
        return -1;
    unsigned char *texels = memalign(32, (size_t)width * (size_t)height);
    if (!texels)
        return -1;
    memset(texels, 0, (size_t)width * (size_t)height);
    t->kind = TEXTURE_DYNAMIC;
    t->texels = texels;
    t->width = width;
    t->height = height;
    t->levels = 1;
    t->gx_format = GX_TF_IA4;
    t->image = texels;
    t->bytes = (uint32_t)(width * height);
    ui.resident += t->bytes;
    return t - ui.textures;
}

void wii_ui_gx_texture_dynamic_update(long handle, const uint16_t *texels)
{
    if (handle < 0 || handle >= UI_MAX_TEXTURES || ui.textures[handle].kind != TEXTURE_DYNAMIC)
        return;
    struct ui_texture *t = &ui.textures[handle];
    unsigned char *out = t->texels;
    /* (draws already queued read the texture as it was) */
    GX_DrawDone();
    /* IA4 texel: alpha in the high nibble, intensity in the low: the
    A4R4G4B4 texel's alpha and its red nibble (white in the glyph cache) */
    for (int ty = 0; ty < t->height; ty += 4)
        for (int tx = 0; tx < t->width; tx += 8)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 8; ++x) {
                    uint16_t texel = texels[(ty + y) * t->width + tx + x];
                    *out++ = (unsigned char)(((texel >> 12) << 4) | ((texel >> 8) & 0xFu));
                }
    DCFlushRange(t->texels, t->bytes);
    GX_InvalidateTexAll();
}

void wii_ui_gx_texture_release(long handle)
{
    if (handle >= 0 && handle < UI_MAX_TEXTURES && ui.textures[handle].kind != TEXTURE_FREE)
        texture_free(&ui.textures[handle]);
}

void wii_ui_gx_release_map_textures(void)
{
    unsigned long released = 0, bytes = ui.resident;
    GX_DrawDone();
    for (int i = 0; i < UI_MAX_TEXTURES; ++i) {
        struct ui_texture *t = &ui.textures[i];
        if (t->kind == TEXTURE_TAG || t->kind == TEXTURE_FAILED) {
            released += t->kind == TEXTURE_TAG;
            texture_free(t);
        }
    }
    wii_log("BOOT_UI_TEXTURES_RELEASED textures=%lu bytes=%lu resident_now=%lu\n", released, bytes - ui.resident,
            ui.resident);
}

/* ---------- drawing */

static void state_2d(void)
{
    /* guOrtho(0, h, 0, w, 0, 1) and the identity, written out: libogc's
    paired-single matrix routines do not link into this image (their
    small-data relocations) */
    Mtx44 projection;
    Mtx identity;
    memset(projection, 0, sizeof(projection));
    projection[0][0] = 2.0f / (f32)ui.window_w;
    projection[0][3] = -1.0f;
    projection[1][1] = -2.0f / (f32)ui.window_h;
    projection[1][3] = 1.0f;
    projection[2][2] = -1.0f;
    projection[2][3] = -1.0f;
    projection[3][3] = 1.0f;
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    memset(identity, 0, sizeof(identity));
    identity[0][0] = identity[1][1] = identity[2][2] = 1.0f;
    GX_LoadPosMtxImm(identity, GX_PNMTX1);
    GX_SetCurrentMtx(GX_PNMTX1);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

static void blend(int function)
{
    switch (function) {
    case WII_UI_BLEND_MULTIPLY:
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    case WII_UI_BLEND_DOUBLE_MULTIPLY:
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_SRCCLR, GX_LO_CLEAR);
        break;
    case WII_UI_BLEND_ADD:
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        break;
    case WII_UI_BLEND_SUBTRACT:
        GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        break;
    case WII_UI_BLEND_ALPHA_MULTIPLY_ADD:
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        break;
    case WII_UI_BLEND_MIN:
    case WII_UI_BLEND_MAX:
        ui.blend_approximated++;
        /* GX has no component min or max */
        __attribute__((fallthrough));
    default:
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        break;
    }
}

static void vertex(const struct wii_ui_vertex *v, int textured)
{
    GX_Position2f32(v->x, v->y);
    GX_Color4u8((u8)(v->argb >> 16), (u8)(v->argb >> 8), (u8)v->argb, (u8)(v->argb >> 24));
    if (textured)
        GX_TexCoord2f32(v->u, v->v);
}

void wii_ui_gx_quads(const struct wii_ui_draw *draw, const struct wii_ui_vertex *vertices, int quad_count)
{
    struct ui_texture *t = NULL;
    if (quad_count <= 0)
        return;
    if (draw->texture >= 0 && draw->texture < UI_MAX_TEXTURES &&
        (ui.textures[draw->texture].kind == TEXTURE_TAG || ui.textures[draw->texture].kind == TEXTURE_DYNAMIC))
        t = &ui.textures[draw->texture];
    else if (draw->texture >= 0) {
        ui.failed_draws++;
        return;
    }
    state_2d();
    blend(draw->blend);
    if (t) {
        GXTexObj object;
        u8 wrap = draw->wrap ? GX_REPEAT : GX_CLAMP;
        int levels = t->levels > 0 ? t->levels : 1;
        GX_InitTexObj(&object, t->image, (u16)t->width, (u16)t->height, (u8)t->gx_format, wrap, wrap,
                      levels > 1 ? GX_TRUE : GX_FALSE);
        u8 filter = draw->point_sampled ? GX_NEAR : GX_LINEAR;
        if (levels > 1)
            GX_InitTexObjLOD(&object, draw->point_sampled ? GX_NEAR_MIP_NEAR : GX_LIN_MIP_LIN, filter, 0.0f,
                             (f32)(levels - 1), 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        else
            GX_InitTexObjFilterMode(&object, filter, filter);
        GX_LoadTexObj(&object, GX_TEXMAP0);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        t->last_frame = ui.frame;
        t->draws++;
    } else {
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }
    if (draw->alpha_weighted) {
        /* the colour times its alpha (Halo PC's HUD overlays) */
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetNumTevStages(2);
    } else {
        GX_SetNumTevStages(1);
    }
    for (int done = 0; done < quad_count;) {
        int batch = quad_count - done > 4096 ? 4096 : quad_count - done;
        GX_Begin(GX_QUADS, GX_VTXFMT2, (u16)(batch * 4));
        for (int q = 0; q < batch; ++q)
            for (int k = 0; k < 4; ++k)
                vertex(&vertices[(done + q) * 4 + k], t != NULL);
        GX_End();
        done += batch;
    }
    if (t && t->kind == TEXTURE_DYNAMIC)
        ui.text_quads += (unsigned long)quad_count;
    else
        ui.quads += (unsigned long)quad_count;
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

void wii_ui_gx_fill(int x0, int y0, int x1, int y1, uint32_t argb)
{
    struct wii_ui_vertex v[4] = {
        {(float)x0, (float)y0, 0, 0, argb}, {(float)x1, (float)y0, 0, 0, argb},
        {(float)x1, (float)y1, 0, 0, argb}, {(float)x0, (float)y1, 0, 0, argb}};
    struct wii_ui_draw draw = {-1, WII_UI_BLEND_ALPHA, 0, 0, 0};
    wii_ui_gx_quads(&draw, v, 1);
}

/* ---------- the diagnostic font: 5x7, rows top to bottom, bit 4 leftmost */

struct glyph {
    char c;
    unsigned char rows[7];
};

static const struct glyph font[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'\\', {0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}}, {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {'"', {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}}, {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},
    {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}}, {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
    {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}}, {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},
};

static const unsigned char *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); ++i)
        if (font[i].c == c)
            return font[i].rows;
    return NULL;
}

void wii_ui_gx_text(int x, int y, int scale, uint32_t argb, const char *text)
{
    /* one quad per lit pixel */
    static struct wii_ui_vertex quads[4 * 35 * 8];
    struct wii_ui_draw draw = {-1, WII_UI_BLEND_ALPHA, 0, 0, 0};
    int count = 0;
    if (scale < 1)
        scale = 1;
    for (const char *p = text; *p; ++p, x += 6 * scale) {
        const unsigned char *rows = glyph_rows(*p);
        if (!rows)
            continue;
        for (int row = 0; row < 7; ++row)
            for (int column = 0; column < 5; ++column) {
                if (!(rows[row] & (0x10 >> column)))
                    continue;
                float x0 = (float)(x + column * scale), y0 = (float)(y + row * scale);
                float x1 = x0 + (float)scale, y1 = y0 + (float)scale;
                struct wii_ui_vertex *v = &quads[count * 4];
                v[0] = (struct wii_ui_vertex){x0, y0, 0, 0, argb};
                v[1] = (struct wii_ui_vertex){x1, y0, 0, 0, argb};
                v[2] = (struct wii_ui_vertex){x1, y1, 0, 0, argb};
                v[3] = (struct wii_ui_vertex){x0, y1, 0, 0, argb};
                if (++count == (int)(sizeof(quads) / sizeof(quads[0]) / 4)) {
                    wii_ui_gx_quads(&draw, quads, count);
                    count = 0;
                }
            }
    }
    if (count)
        wii_ui_gx_quads(&draw, quads, count);
}

/* ---------- the frame */

uint32_t wii_ui_gx_sample_crc(int *covered)
{
    uint32_t crc = 0;
    *covered = 0;
    GX_DrawDone();
    for (int gy = 0; gy < UI_GRID_Y; ++gy)
        for (int gx_index = 0; gx_index < UI_GRID_X; ++gx_index) {
            GXColor c;
            GX_PeekARGB((u16)((gx_index * 2 + 1) * ui.mode->fbWidth / (UI_GRID_X * 2)),
                        (u16)((gy * 2 + 1) * ui.mode->efbHeight / (UI_GRID_Y * 2)), &c);
            const unsigned char bytes[3] = {c.r, c.g, c.b};
            crc = crc_update(crc, bytes, 3);
            *covered += c.r || c.g || c.b;
        }
    return crc;
}

void wii_ui_gx_present(void)
{
    GX_CopyDisp(ui.xfb[ui.fb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(ui.xfb[ui.fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    ui.fb ^= 1;
    ui.frame++;
    /* the next frame's first draws: the whole screen */
    wii_ui_gx_window(0, 0, ui.mode->fbWidth, ui.mode->efbHeight);
}

void wii_ui_gx_report(void)
{
    int tags = 0;
    for (int i = 0; i < UI_MAX_TEXTURES; ++i)
        tags += ui.textures[i].kind == TEXTURE_TAG;
    wii_log("BOOT_UI_GX_TOTALS frames=%lu quads=%lu text_quads=%lu loads=%lu load_failures=%lu evictions=%lu "
            "resident=%lu resident_peak=%lu budget=%lu tag_textures_now=%d load_ms=%llu blend_approximated=%lu "
            "failed_draws=%lu\n",
            ui.frame, ui.quads, ui.text_quads, ui.loads, ui.load_failures, ui.evictions, ui.resident, ui.resident_peak,
            ui.budget, tags, ui.load_us / 1000ull, ui.blend_approximated, ui.failed_draws);
}
