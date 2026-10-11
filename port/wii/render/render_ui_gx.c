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
    void *alpha_image;         /* HWT1 version 2: the alpha plane's level 0 (I4 or I8), or NULL */
    int alpha_format;
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
    unsigned long alpha_planes;
    unsigned long loads, load_failures, evictions, quads, text_quads, failed_draws;
    /* HWI-033 */
    unsigned long plasma_quads, plasma_stages, minmax_quads, minmax_tiles;
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
    /* (the plasma effect's maps 1 and 2, HWI-033) */
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX2, GX_TEX_ST, GX_F32, 0);
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
    struct hwt_image alpha_image;
    if (!why && header.alpha_kind == HWT_ALPHA_PLANE &&
        (hwt_alpha_image_at(&header, 0, &alpha_image) != CONTENT_OK || (alpha_image.offset & 31u)))
        why = "alpha_plane_not_loadable";
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
    /* HWI-008F's split alpha: CMPR colour and an I4/I8 plane of the alpha
    (a DXT3/DXT5 source); without a plane the alpha is 255 */
    t->alpha_image = header.alpha_kind == HWT_ALPHA_PLANE ? blob.data + alpha_image.offset : NULL;
    t->alpha_format = (int)header.alpha_gx_format;
    if (t->alpha_image)
        ui.alpha_planes++;
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
    case WII_UI_BLEND_REPLACE:
        /* HWI-033: min and max are computed in the TEV against a copy of the
        destination (minmax_*), whose result replaces the pixel */
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        break;
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

/* a texture (and its alpha plane, if any) bound to map and plane_map; 1 if it has a plane */
static int bind_texture(struct ui_texture *t, u8 map, u8 plane_map, int wrapped, int point_sampled)
{
    GXTexObj object;
    u8 wrap = wrapped ? GX_REPEAT : GX_CLAMP;
    int levels = t->levels > 0 ? t->levels : 1;
    u8 filter = point_sampled ? GX_NEAR : GX_LINEAR;
    GX_InitTexObj(&object, t->image, (u16)t->width, (u16)t->height, (u8)t->gx_format, wrap, wrap,
                  levels > 1 ? GX_TRUE : GX_FALSE);
    if (levels > 1)
        GX_InitTexObjLOD(&object, point_sampled ? GX_NEAR_MIP_NEAR : GX_LIN_MIP_LIN, filter, 0.0f,
                         (f32)(levels - 1), 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(&object, filter, filter);
    GX_LoadTexObj(&object, map);
    t->last_frame = ui.frame;
    t->draws++;
    if (!t->alpha_image)
        return 0;
    GXTexObj alpha;
    GX_InitTexObj(&alpha, t->alpha_image, (u16)t->width, (u16)t->height, (u8)t->alpha_format, wrap, wrap,
                  levels > 1 ? GX_TRUE : GX_FALSE);
    if (levels > 1)
        GX_InitTexObjLOD(&alpha, point_sampled ? GX_NEAR_MIP_NEAR : GX_LIN_MIP_LIN, filter, 0.0f,
                         (f32)(levels - 1), 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(&alpha, filter, filter);
    GX_LoadTexObj(&alpha, plane_map);
    return 1;
}

static struct ui_texture *drawable_texture(long handle)
{
    if (handle >= 0 && handle < UI_MAX_TEXTURES &&
        (ui.textures[handle].kind == TEXTURE_TAG || ui.textures[handle].kind == TEXTURE_DYNAMIC))
        return &ui.textures[handle];
    return NULL;
}

/* ---------- TEV stage helpers */

static void tev_colour(u8 stage, u8 a, u8 b, u8 c, u8 d, u8 op, u8 bias, u8 scale, u8 out)
{
    GX_SetTevColorIn(stage, a, b, c, d);
    GX_SetTevColorOp(stage, op, bias, scale, GX_TRUE, out);
}

static void tev_alpha(u8 stage, u8 a, u8 b, u8 c, u8 d, u8 op, u8 bias, u8 scale, u8 out)
{
    GX_SetTevAlphaIn(stage, a, b, c, d);
    GX_SetTevAlphaOp(stage, op, bias, scale, GX_TRUE, out);
}

/* a stage whose colour or alpha half is not used: PREV is kept */
static void tev_colour_keep(u8 stage)
{
    tev_colour(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
               GX_TEVPREV);
}

static void tev_alpha_keep(u8 stage)
{
    tev_alpha(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
              GX_TEVPREV);
}

/* the stages' swap modes back to the identity (other draws assume it) */
static void tev_swaps_reset(int stages)
{
    for (int stage = 0; stage < stages; ++stage)
        GX_SetTevSwapMode((u8)(GX_TEVSTAGE0 + stage), GX_TEV_SWAP0, GX_TEV_SWAP0);
}

/* ---------- min and max (HWI-033)

The Xbox's D3DBLENDOP_MIN / _MAX write the per-channel minimum or maximum of
the shader's colour and the framebuffer's, ignoring the blend factors. GX's
blender has neither, and its logic operations are bitwise. The destination
is copied to a texture (the EFB is RGB8: an exact copy), read at the same
pixel in three compare stages after the source's, and the result replaces
the pixel (GX_BM_NONE): min = (s > d ? d : 0) + (d > s ? s : 0) + (s == d ? s : 0)
per channel, max the same with the other operand taken; exact on 8 bits. A
quad is drawn in tiles of at most MINMAX_TILE pixels square, each with its
own copy, so a later quad reads what an earlier one wrote. */

#define MINMAX_TILE 128
static unsigned char minmax_copy[MINMAX_TILE * MINMAX_TILE * 4] __attribute__((aligned(32)));

/* the three compare stages from stage on; the destination texel at texcoord */
static int minmax_stages(u8 stage, u8 texcoord, int maximum)
{
    u8 s0 = stage, s1 = (u8)(stage + 1), s2 = (u8)(stage + 2);
    for (u8 s = s0; s <= s2; ++s) {
        GX_SetTevOrder(s, texcoord, GX_TEXMAP7, GX_COLORNULL);
        tev_alpha_keep(s);
    }
    /* (s > d ? (max: s, min: d) : 0) -> REG0 */
    tev_colour(s0, GX_CC_CPREV, GX_CC_TEXC, maximum ? GX_CC_CPREV : GX_CC_TEXC, GX_CC_ZERO, GX_TEV_COMP_RGB8_GT,
               GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVREG0);
    /* + (d > s ? (max: d, min: s) : 0) -> REG0 */
    tev_colour(s1, GX_CC_TEXC, GX_CC_CPREV, maximum ? GX_CC_TEXC : GX_CC_CPREV, GX_CC_C0, GX_TEV_COMP_RGB8_GT,
               GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVREG0);
    /* + (s == d ? s : 0) -> PREV */
    tev_colour(s2, GX_CC_CPREV, GX_CC_TEXC, GX_CC_CPREV, GX_CC_C0, GX_TEV_COMP_RGB8_EQ, GX_TB_ZERO, GX_CS_SCALE_1,
               GX_TEVPREV);
    return 3;
}

/* copies the EFB tile (EFB pixels x, y, w, h: even, w and h multiples of 4) to minmax_copy, binds it to
GX_TEXMAP7 and texcoord (from the window position through GX_TEXMTX9) */
static void minmax_tile_copy(int x, int y, int w, int h, u8 texcoord)
{
    GXTexObj object;
    Mtx matrix;
    GX_SetTexCopySrc((u16)x, (u16)y, (u16)w, (u16)h);
    GX_SetTexCopyDst((u16)w, (u16)h, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(minmax_copy, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
    GX_InitTexObj(&object, minmax_copy, (u16)w, (u16)h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&object, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&object, GX_TEXMAP7);
    memset(matrix, 0, sizeof(matrix));
    matrix[0][0] = 1.0f / (f32)w;
    matrix[0][3] = (f32)(ui.window_x0 - x) / (f32)w;
    matrix[1][1] = 1.0f / (f32)h;
    matrix[1][3] = (f32)(ui.window_y0 - y) / (f32)h;
    GX_LoadTexMtxImm(matrix, GX_TEXMTX9, GX_MTX2x4);
    GX_SetTexCoordGen(texcoord, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX9);
}

/* the EFB box (x0, y0, x1, y1) a quad covers in its window, aligned outward to 4 and clipped; 0 if empty */
static int quad_box(const struct wii_ui_vertex *quad, int box[4])
{
    float x0 = quad[0].x, y0 = quad[0].y, x1 = quad[0].x, y1 = quad[0].y;
    for (int k = 1; k < 4; ++k) {
        x0 = quad[k].x < x0 ? quad[k].x : x0;
        y0 = quad[k].y < y0 ? quad[k].y : y0;
        x1 = quad[k].x > x1 ? quad[k].x : x1;
        y1 = quad[k].y > y1 ? quad[k].y : y1;
    }
    int lo_x = ui.window_x0 + (int)x0, lo_y = ui.window_y0 + (int)y0;
    int hi_x = ui.window_x0 + (int)x1 + 1, hi_y = ui.window_y0 + (int)y1 + 1;
    if (lo_x < ui.window_x0) lo_x = ui.window_x0;
    if (lo_y < ui.window_y0) lo_y = ui.window_y0;
    if (hi_x > ui.window_x0 + ui.window_w) hi_x = ui.window_x0 + ui.window_w;
    if (hi_y > ui.window_y0 + ui.window_h) hi_y = ui.window_y0 + ui.window_h;
    box[0] = lo_x & ~3;
    box[1] = lo_y & ~3;
    box[2] = (hi_x + 3) & ~3;
    box[3] = (hi_y + 3) & ~3;
    if (box[2] > ui.mode->fbWidth) box[2] = ui.mode->fbWidth & ~3;
    if (box[3] > ui.mode->efbHeight) box[3] = ui.mode->efbHeight & ~3;
    return box[2] > box[0] && box[3] > box[1] && hi_x > lo_x && hi_y > lo_y;
}

void wii_ui_gx_quads(const struct wii_ui_draw *draw, const struct wii_ui_vertex *vertices, int quad_count)
{
    struct ui_texture *t = NULL;
    if (quad_count <= 0)
        return;
    if (draw->texture >= 0) {
        t = drawable_texture(draw->texture);
        if (!t) {
            ui.failed_draws++;
            return;
        }
    }
    int stages = 1;
    int minmax = draw->blend == WII_UI_BLEND_MIN || draw->blend == WII_UI_BLEND_MAX;
    state_2d();
    blend(draw->blend);
    if (t) {
        int plane = bind_texture(t, GX_TEXMAP0, GX_TEXMAP1, draw->wrap, draw->point_sampled);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetNumTexGens(minmax ? 2 : 1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        if (plane) {
            /* the alpha plane, same chain and footprint: alpha = plane x vertex alpha */
            GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
            GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
            GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
            GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            stages = 2;
        }
    } else {
        GX_SetNumTexGens(minmax ? 1 : 0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }
    if (draw->alpha_weighted) {
        /* the colour times its alpha (Halo PC's HUD overlays) */
        u8 stage = (u8)(GX_TEVSTAGE0 + stages);
        GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO);
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        stages++;
    }
    if (minmax) {
        /* each quad, tile by tile, against its own copy of the destination */
        u8 texcoord = t ? GX_TEXCOORD1 : GX_TEXCOORD0;
        stages += minmax_stages((u8)(GX_TEVSTAGE0 + stages), texcoord, draw->blend == WII_UI_BLEND_MAX);
        GX_SetNumTevStages((u8)stages);
        for (int q = 0; q < quad_count; ++q) {
            int box[4];
            if (!quad_box(&vertices[q * 4], box))
                continue;
            for (int ty = box[1]; ty < box[3]; ty += MINMAX_TILE)
                for (int tx = box[0]; tx < box[2]; tx += MINMAX_TILE) {
                    int tw = box[2] - tx < MINMAX_TILE ? box[2] - tx : MINMAX_TILE;
                    int th = box[3] - ty < MINMAX_TILE ? box[3] - ty : MINMAX_TILE;
                    minmax_tile_copy(tx, ty, tw, th, texcoord);
                    GX_SetScissor((u32)tx, (u32)ty, (u32)tw, (u32)th);
                    GX_Begin(GX_QUADS, GX_VTXFMT2, 4);
                    for (int k = 0; k < 4; ++k)
                        vertex(&vertices[q * 4 + k], t != NULL);
                    GX_End();
                    ui.minmax_tiles++;
                }
        }
        GX_SetScissor((u32)ui.window_x0, (u32)ui.window_y0, (u32)ui.window_w, (u32)ui.window_h);
        ui.minmax_quads += (unsigned long)quad_count;
    } else {
        GX_SetNumTevStages((u8)stages);
        for (int done = 0; done < quad_count;) {
            int batch = quad_count - done > 4096 ? 4096 : quad_count - done;
            GX_Begin(GX_QUADS, GX_VTXFMT2, (u16)(batch * 4));
            for (int q = 0; q < batch; ++q)
                for (int k = 0; k < 4; ++k)
                    vertex(&vertices[(done + q) * 4 + k], t != NULL);
            GX_End();
            done += batch;
        }
    }
    if (t && t->kind == TEXTURE_DYNAMIC)
        ui.text_quads += (unsigned long)quad_count;
    else
        ui.quads += (unsigned long)quad_count;
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

/* ---------- the plasma effect (HWI-033)

The menus' plasma (ui_widget.c, doing_plasma_effect): two animated maps of
the plasma bitmap, combined by the Xbox's register-combiner program for
map0_to_1 = min and map1_to_2 = alpha blend
(rasterizer_xbox_dynavobgeom.c), under the widget's own bitmap. The program,
as the NV2A combiners run it (tools/wii/ui_combiners.py holds it and this
TEV program as models): a = the maps' faded alphas, b = their tinted blues,
x = a1 - a0 + 1/2, y = b0 - b1 + 1/2;
r = min(1, 4 (y >= 1/2 ? x : max(y, 0))^2), u = max(2r - 1, 0),
P = (r + u^2) / 2; colour = P plasma_fade + map2 tint2 vertex, alpha = map2.a fade2.
Its TEV form, 8-bit throughout (12 stages; 15 with three alpha planes):
- maps 0 and 1 with the texture swap (a, a, a, b): faded alpha in the colour,
  tinted blue in the alpha (an alpha plane: blue from the colour texture,
  alpha from the plane, two stages);
- x and y by a subtraction with a half bias; 4x^2 and 4y^2 by a scaled
  square; the mux by two compares (y > 127, 128 > y); 2r - 1 by a scaled
  half subtraction, its square, the halved sum;
- map 2 tinted, P times the plasma fade, plus map 2 times the vertex colour. */

static void plasma_vertex(const struct wii_ui_plasma_vertex *v)
{
    GX_Position2f32(v->x, v->y);
    GX_Color4u8((u8)(v->argb >> 16), (u8)(v->argb >> 8), (u8)v->argb, (u8)(v->argb >> 24));
    GX_TexCoord2f32(v->st[0][0], v->st[0][1]);
    GX_TexCoord2f32(v->st[1][0], v->st[1][1]);
    GX_TexCoord2f32(v->st[2][0], v->st[2][1]);
}

/* one plasma map into out: colour = its faded alpha, alpha = its tinted blue; the factors from the konst
(map 0: K0) or REG2 (map 1, loaded before the draw); returns the stages used */
static int plasma_map_stages(u8 stage, u8 texcoord, u8 map, u8 plane_map, int plane, int konst, u8 out)
{
    u8 colour_factor = konst ? GX_CC_KONST : GX_CC_C2;
    u8 alpha_factor = konst ? GX_CA_KONST : GX_CA_A2;
    if (konst) {
        GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K0);
        GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
    }
    GX_SetTevOrder(stage, texcoord, map, GX_COLORNULL);
    GX_SetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP1);
    tev_alpha(stage, GX_CA_ZERO, GX_CA_TEXA, alpha_factor, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, out);
    if (!plane) {
        tev_colour(stage, GX_CC_ZERO, GX_CC_TEXC, colour_factor, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                   out);
        return 1;
    }
    /* the colour texture gave the blue; the plane (I4/I8: its intensity everywhere) gives the alpha */
    tev_colour_keep(stage);
    u8 next = (u8)(stage + 1);
    if (konst) {
        GX_SetTevKColorSel(next, GX_TEV_KCSEL_K0);
        GX_SetTevKAlphaSel(next, GX_TEV_KASEL_K0_A);
    }
    GX_SetTevOrder(next, texcoord, plane_map, GX_COLORNULL);
    GX_SetTevSwapMode(next, GX_TEV_SWAP0, GX_TEV_SWAP0);
    tev_colour(next, GX_CC_ZERO, GX_CC_TEXC, colour_factor, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, out);
    tev_alpha_keep(next);
    return 2;
}

void wii_ui_gx_plasma(const struct wii_ui_plasma *plasma, const struct wii_ui_plasma_vertex *vertices, int quad_count)
{
    struct ui_texture *t[3];
    int plane[3];
    if (quad_count <= 0)
        return;
    for (int i = 0; i < 3; ++i) {
        t[i] = drawable_texture(plasma->textures[i]);
        if (!t[i]) {
            ui.failed_draws++;
            return;
        }
    }
    state_2d();
    blend(plasma->blend);
    plane[0] = bind_texture(t[0], GX_TEXMAP0, GX_TEXMAP1, plasma->wrap[0], plasma->point_sampled);
    plane[1] = bind_texture(t[1], GX_TEXMAP2, GX_TEXMAP3, plasma->wrap[1], plasma->point_sampled);
    plane[2] = bind_texture(t[2], GX_TEXMAP4, GX_TEXMAP5, plasma->wrap[2], plasma->point_sampled);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX2, GX_DIRECT);
    GX_SetNumTexGens(3);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX2, GX_IDENTITY);
    /* the texture swap of the plasma maps: (a, a, a, b) */
    GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_ALPHA, GX_CH_ALPHA, GX_CH_ALPHA, GX_CH_BLUE);
    /* K0: map 0's fade (rgb) and tint blue (a); REG2: map 1's, read before S2 writes it; K1: 127;
    K2: the plasma fade; K3: map 2's tint and fade */
    GX_SetTevKColor(GX_KCOLOR0, (GXColor){plasma->map_fade[0], plasma->map_fade[0], plasma->map_fade[0],
                                          plasma->map_tint_blue[0]});
    GX_SetTevColor(GX_TEVREG2, (GXColor){plasma->map_fade[1], plasma->map_fade[1], plasma->map_fade[1],
                                         plasma->map_tint_blue[1]});
    GX_SetTevKColor(GX_KCOLOR1, (GXColor){127, 127, 127, 127});
    GX_SetTevKColor(GX_KCOLOR2, (GXColor){(u8)(plasma->plasma_fade >> 16), (u8)(plasma->plasma_fade >> 8),
                                          (u8)plasma->plasma_fade, 255});
    GX_SetTevKColor(GX_KCOLOR3, (GXColor){(u8)(plasma->map2_argb >> 16), (u8)(plasma->map2_argb >> 8),
                                          (u8)plasma->map2_argb, (u8)(plasma->map2_argb >> 24)});
    u8 s = GX_TEVSTAGE0;
    /* maps 0 and 1: REG0 = (a0, b0), REG1 = (a1, b1) */
    s = (u8)(s + plasma_map_stages(s, GX_TEXCOORD0, GX_TEXMAP0, GX_TEXMAP1, plane[0], 1, GX_TEVREG0));
    s = (u8)(s + plasma_map_stages(s, GX_TEXCOORD1, GX_TEXMAP2, GX_TEXMAP3, plane[1], 0, GX_TEVREG1));
    /* REG2 = (x, y): a1 - a0 + 1/2, b0 - b1 + 1/2 */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    tev_colour(s, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C1, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TEVREG2);
    tev_alpha(s, GX_CA_A1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TEVREG2);
    s++;
    /* REG1 = (4x^2, 4y^2) */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    tev_colour(s, GX_CC_ZERO, GX_CC_C2, GX_CC_C2, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_4, GX_TEVREG1);
    tev_alpha(s, GX_CA_ZERO, GX_CA_A2, GX_CA_A2, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_4, GX_TEVREG1);
    s++;
    /* REG0.rgb = r: (y > 127 ? 4x^2 : 0) + (128 > y ? 4y^2 : 0) */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevKColorSel(s, GX_TEV_KCSEL_K1);
    tev_colour(s, GX_CC_A2, GX_CC_KONST, GX_CC_C1, GX_CC_ZERO, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1,
               GX_TEVREG0);
    tev_alpha_keep(s);
    s++;
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevKColorSel(s, GX_TEV_KCSEL_1_2);
    tev_colour(s, GX_CC_KONST, GX_CC_A2, GX_CC_A1, GX_CC_C0, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1,
               GX_TEVREG0);
    tev_alpha_keep(s);
    s++;
    /* REG1.rgb = u = clamp(2r - 1), then u^2 */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    tev_colour(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_2, GX_TEVREG1);
    tev_alpha_keep(s);
    s++;
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    tev_colour(s, GX_CC_ZERO, GX_CC_C1, GX_CC_C1, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVREG1);
    tev_alpha_keep(s);
    s++;
    /* REG0.rgb = P = (r + u^2) / 2 */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    tev_colour(s, GX_CC_ZERO, GX_CC_C1, GX_CC_ONE, GX_CC_C0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TEVREG0);
    tev_alpha_keep(s);
    s++;
    /* REG1 = map 2 times its tint and fade (K3) */
    GX_SetTevOrder(s, GX_TEXCOORD2, GX_TEXMAP4, GX_COLORNULL);
    GX_SetTevSwapMode(s, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GX_SetTevKColorSel(s, GX_TEV_KCSEL_K3);
    GX_SetTevKAlphaSel(s, GX_TEV_KASEL_K3_A);
    tev_colour(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVREG1);
    if (plane[2])
        tev_alpha_keep(s);
    else
        tev_alpha(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                  GX_TEVREG1);
    s++;
    if (plane[2]) {
        GX_SetTevOrder(s, GX_TEXCOORD2, GX_TEXMAP5, GX_COLORNULL);
        GX_SetTevSwapMode(s, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevKAlphaSel(s, GX_TEV_KASEL_K3_A);
        tev_colour_keep(s);
        tev_alpha(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                  GX_TEVREG1);
        s++;
    }
    /* REG0.rgb = P times the plasma fade (K2) */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevKColorSel(s, GX_TEV_KCSEL_K2);
    tev_colour(s, GX_CC_ZERO, GX_CC_C0, GX_CC_KONST, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVREG0);
    tev_alpha_keep(s);
    s++;
    /* PREV = that + map 2 times the vertex colour; alpha = map 2's */
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    tev_colour(s, GX_CC_ZERO, GX_CC_RASC, GX_CC_C1, GX_CC_C0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    tev_alpha(s, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    s++;
    int stages = s - GX_TEVSTAGE0;
    GX_SetNumTevStages((u8)stages);
    for (int done = 0; done < quad_count;) {
        int batch = quad_count - done > 2048 ? 2048 : quad_count - done;
        GX_Begin(GX_QUADS, GX_VTXFMT2, (u16)(batch * 4));
        for (int q = 0; q < batch; ++q)
            for (int k = 0; k < 4; ++k)
                plasma_vertex(&vertices[(done + q) * 4 + k]);
        GX_End();
        done += batch;
    }
    tev_swaps_reset(stages);
    GX_SetVtxDesc(GX_VA_TEX1, GX_NONE);
    GX_SetVtxDesc(GX_VA_TEX2, GX_NONE);
    ui.plasma_quads += (unsigned long)quad_count;
    ui.plasma_stages = (unsigned long)stages;
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

/* ---------- the self-test (HWI-033)

Authored inputs only (no game data): three 4x4 RGBA8 textures of
pseudo-random texels, point sampled at texel centres, so each 8x8 quad sees
one texel of each map; random tints, fades, vertex colours. Each case's
centre pixel is peeked from the EFB and logged with its inputs, for
tools/wii/ui_combiners.py check-log, which holds the Xbox's program. */

#define SELFTEST_PLASMA_CASES 48
#define SELFTEST_BLEND_CASES 32

static unsigned char selftest_texels[3][64] __attribute__((aligned(32)));
static unsigned long selftest_state;

static unsigned char selftest_byte(void)
{
    selftest_state = selftest_state * 1664525UL + 1013904223UL;
    return (unsigned char)(selftest_state >> 24);
}

/* texel (r, g, b, a) of an RGBA8 4x4 tile: AR pairs, then GB pairs */
static void selftest_texel(int map, int index, unsigned char rgba[4])
{
    rgba[0] = selftest_texels[map][index * 2 + 1];
    rgba[1] = selftest_texels[map][32 + index * 2];
    rgba[2] = selftest_texels[map][32 + index * 2 + 1];
    rgba[3] = selftest_texels[map][index * 2];
}

static long selftest_texture(int map)
{
    struct ui_texture *t = slot_new();
    if (!t)
        return -1;
    for (int i = 0; i < 64; ++i)
        selftest_texels[map][i] = selftest_byte();
    DCFlushRange(selftest_texels[map], sizeof(selftest_texels[map]));
    t->kind = TEXTURE_DYNAMIC;
    t->texels = NULL;
    t->width = 4;
    t->height = 4;
    t->levels = 1;
    t->gx_format = GX_TF_RGBA8;
    t->image = selftest_texels[map];
    t->bytes = 0;
    return t - ui.textures;
}

static void selftest_peek(int x, int y, unsigned char rgb[3])
{
    GXColor c;
    GX_PeekARGB((u16)x, (u16)y, &c);
    rgb[0] = c.r;
    rgb[1] = c.g;
    rgb[2] = c.b;
}

static void selftest_fill(int x0, int y0, int x1, int y1, uint32_t argb)
{
    struct wii_ui_vertex v[4] = {
        {(float)x0, (float)y0, 0, 0, argb}, {(float)x1, (float)y0, 0, 0, argb},
        {(float)x1, (float)y1, 0, 0, argb}, {(float)x0, (float)y1, 0, 0, argb}};
    struct wii_ui_draw draw = {-1, WII_UI_BLEND_REPLACE, 0, 0, 0};
    wii_ui_gx_quads(&draw, v, 1);
}

int wii_ui_gx_selftest(void)
{
    long textures[3];
    int cases = 0;
    selftest_state = 0x48574933UL; /* "HWI3" */
    GX_InvalidateTexAll();
    wii_ui_gx_window(0, 0, ui.mode->fbWidth, ui.mode->efbHeight);
    for (int map = 0; map < 3; ++map)
        textures[map] = selftest_texture(map);
    if (textures[0] < 0 || textures[1] < 0 || textures[2] < 0) {
        wii_log("BOOT_UI_SELFTEST_FAIL no texture slot\n");
        return 0;
    }
    selftest_fill(0, 0, ui.mode->fbWidth, ui.mode->efbHeight, 0xFF000000u);
    for (int c = 0; c < SELFTEST_PLASMA_CASES; ++c) {
        struct wii_ui_plasma plasma;
        struct wii_ui_plasma_vertex v[4];
        int index[3] = {c % 16, (c * 5 + 3) % 16, (c * 7 + 1) % 16};
        unsigned char texel[3][4], m[3][4], vertex_colour[4], fade[3], efb[3];
        int x = 16 + (c % 16) * 24, y = 40 + (c / 16) * 24;
        for (int map = 0; map < 3; ++map) {
            selftest_texel(map, index[map], texel[map]);
            for (int k = 0; k < 4; ++k)
                m[map][k] = selftest_byte();
        }
        for (int k = 0; k < 4; ++k)
            vertex_colour[k] = selftest_byte();
        for (int k = 0; k < 3; ++k)
            fade[k] = selftest_byte();
        memset(&plasma, 0, sizeof(plasma));
        for (int map = 0; map < 3; ++map)
            plasma.textures[map] = textures[map];
        plasma.point_sampled = 1;
        plasma.blend = WII_UI_BLEND_REPLACE;
        plasma.map_fade[0] = m[0][3];
        plasma.map_tint_blue[0] = m[0][2];
        plasma.map_fade[1] = m[1][3];
        plasma.map_tint_blue[1] = m[1][2];
        plasma.map2_argb = ((uint32_t)m[2][3] << 24) | ((uint32_t)m[2][0] << 16) | ((uint32_t)m[2][1] << 8) | m[2][2];
        plasma.plasma_fade = ((uint32_t)fade[0] << 16) | ((uint32_t)fade[1] << 8) | fade[2];
        for (int k = 0; k < 4; ++k) {
            v[k].x = (float)(x + ((k == 1 || k == 2) ? 8 : 0));
            v[k].y = (float)(y + (k >= 2 ? 8 : 0));
            for (int map = 0; map < 3; ++map) {
                v[k].st[map][0] = ((float)(index[map] % 4) + 0.5f) / 4.0f;
                v[k].st[map][1] = ((float)(index[map] / 4) + 0.5f) / 4.0f;
            }
            v[k].argb = ((uint32_t)vertex_colour[3] << 24) | ((uint32_t)vertex_colour[0] << 16) |
                        ((uint32_t)vertex_colour[1] << 8) | vertex_colour[2];
        }
        wii_ui_gx_plasma(&plasma, v, 1);
        GX_DrawDone();
        selftest_peek(x + 4, y + 4, efb);
        wii_log("BOOT_UI_SELFTEST plasma case=%d t0=%02x,%02x,%02x,%02x t1=%02x,%02x,%02x,%02x "
                "t2=%02x,%02x,%02x,%02x v0=%02x,%02x,%02x,%02x m0=%02x,%02x,%02x,%02x m1=%02x,%02x,%02x,%02x "
                "m2=%02x,%02x,%02x,%02x pf=%02x,%02x,%02x efb=%02x,%02x,%02x\n",
                c, texel[0][0], texel[0][1], texel[0][2], texel[0][3], texel[1][0], texel[1][1], texel[1][2],
                texel[1][3], texel[2][0], texel[2][1], texel[2][2], texel[2][3], vertex_colour[0], vertex_colour[1],
                vertex_colour[2], vertex_colour[3], m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2],
                m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], fade[0], fade[1], fade[2], efb[0], efb[1], efb[2]);
        cases++;
    }
    for (int c = 0; c < 2 * SELFTEST_BLEND_CASES; ++c) {
        int maximum = c >= SELFTEST_BLEND_CASES;
        unsigned char source[3], destination[3], efb[3];
        int x = 16 + (c % 16) * 24, y = 200 + (c / 16) * 24;
        for (int k = 0; k < 3; ++k) {
            source[k] = selftest_byte();
            destination[k] = selftest_byte();
        }
        /* (a few equal channels: the equality stage) */
        if (c % 4 == 0)
            source[1] = destination[1];
        selftest_fill(x - 4, y - 4, x + 12, y + 12, 0xFF000000u | ((uint32_t)destination[0] << 16) |
                      ((uint32_t)destination[1] << 8) | destination[2]);
        uint32_t argb = 0x80000000u | ((uint32_t)source[0] << 16) | ((uint32_t)source[1] << 8) | source[2];
        struct wii_ui_vertex v[4] = {
            {(float)x, (float)y, 0, 0, argb}, {(float)(x + 8), (float)y, 0, 0, argb},
            {(float)(x + 8), (float)(y + 8), 0, 0, argb}, {(float)x, (float)(y + 8), 0, 0, argb}};
        struct wii_ui_draw draw = {-1, maximum ? WII_UI_BLEND_MAX : WII_UI_BLEND_MIN, 0, 0, 0};
        wii_ui_gx_quads(&draw, v, 1);
        GX_DrawDone();
        selftest_peek(x + 4, y + 4, efb);
        wii_log("BOOT_UI_SELFTEST %s case=%d src=%02x,%02x,%02x dst=%02x,%02x,%02x efb=%02x,%02x,%02x\n",
                maximum ? "max" : "min", c, source[0], source[1], source[2], destination[0], destination[1],
                destination[2], efb[0], efb[1], efb[2]);
        cases++;
    }
    int covered = 0;
    uint32_t crc = wii_ui_gx_sample_crc(&covered);
    wii_log("BOOT_UI_SELFTEST_END cases=%d plasma_stages=%lu minmax_tiles=%lu efb_crc=%08lx\n", cases,
            ui.plasma_stages, ui.minmax_tiles, (unsigned long)crc);
    wii_ui_gx_present();
    for (int map = 0; map < 3; ++map)
        texture_free(&ui.textures[textures[map]]);
    return cases;
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
            "resident=%lu resident_peak=%lu budget=%lu tag_textures_now=%d alpha_planes=%lu load_ms=%llu "
            "plasma_quads=%lu plasma_stages=%lu minmax_quads=%lu minmax_tiles=%lu failed_draws=%lu\n",
            ui.frame, ui.quads, ui.text_quads, ui.loads, ui.load_failures, ui.evictions, ui.resident, ui.resident_peak,
            ui.budget, tags, ui.alpha_planes, ui.load_us / 1000ull, ui.plasma_quads, ui.plasma_stages, ui.minmax_quads,
            ui.minmax_tiles, ui.failed_draws);
}
