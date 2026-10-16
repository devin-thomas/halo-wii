/*
 * RENDER_MODELS_GX.C
 *
 * The GX side of the object models (HWI-082, wii_render.h): each model
 * part the engine side hands over is skinned on the CPU from the tag slot
 * and drawn through GX inside the frame, after the environment, with the
 * view render_gx.c loaded (GX_PNMTX0: the world_to_view of the engine).
 * Strict C11 against libogc; no engine header.
 *
 * Geometry: the vertex buffer of a part stays where the staged map holds it,
 * in the tag data of the tag slot (little-endian Xbox compressed model
 * vertices: map_stage.py converts only what the game reads), and its strip or
 * list of u16 indices (big-endian: map_stage.py converts model indices). Each
 * vertex is decoded and skinned with the node matrices of the model by the
 * one profile of hwm_model.c (hwm_skin_decode_le, hwm_skin_apply: node =
 * byte / 3, weight s16 / 32767 as the vertex declaration of the Xbox reads
 * them); a part with any vertex outside it is refused whole, counted and
 * logged, never drawn with another node. The skinned position (F32 xyz), a
 * light colour (RGBA8) and the stored base texcoord (S16 st, HWI-086) go to
 * the skinning buffer, 20 bytes a vertex, which GX reads as indexed arrays
 * (GX_VTXFMT3; the interface of the boot mode owns GX_VTXFMT2).
 *
 * The skinning buffer: one block of model_skin_bytes (render.txt; default
 * WII_RENDER_DEFAULT_MODEL_SKIN_BYTES) from the store of the BSP
 * (render_gx.c: the render pool of the map, or a heap block in the boot
 * mode), taken after the material tables and given back at the end of the
 * BSP. Within a frame it fills from its start; a part that does not fit what
 * is left waits for GX (GX_DrawDone) and starts it again (counted: wraps); a
 * part larger than the whole buffer is refused (counted: capacity). Every
 * draw ends with a draw sync token, and the next draw, or the release of the
 * buffer, waits for it unless GX has passed it, so GX never reads memory the
 * CPU rewrites.
 *
 * Material (HWI-086, render_model_material.h): a shader_model part whose
 * base map is a 2D bitmap, resident in the bitmap reserve of the models,
 * draws it as _rasterizer_model_draw binds it (stage 0, wrapped, trilinear):
 * the base colour times the light colour, the base alpha as the output
 * alpha (a split HWT1 alpha plane on a second stage), its texcoord the
 * stored NORMSHORT2 through the texture matrix of the shader (GX_TEXMTX1:
 * 1 / 32767 times the rows of the engine side), alpha-tested (alpha > 0x7F,
 * depth compared after texturing) unless the shader says not, alpha-blended
 * decals blended over the frame without depth writes. Every other part is
 * the diagnostic material of HWI-082, counted by reason: no texture, solid
 * parts grey, the alpha-blended decals of model shaders light blue, depth
 * tested without depth writes, transparent shaders a 50% magenta blend
 * without depth writes, after every opaque part. The light colour is one
 * fixed directional light over the skinned normal (ambient 0.3, diffuse 0.7;
 * two-sided parts by the absolute value), white under a base map: the
 * lighting of the engine is not read. Culling is GX_CULL_BACK, D3DCULL_CCW
 * for models as for the environment; two-sided model shaders draw both
 * faces (GX_CULL_NONE).
 *
 * The bitmap reserve (render.txt model_texture_bytes, from the same store,
 * after the skinning buffer): a texture cache of its own
 * (render_model_material.c over render_texture_cache.c), so no model bitmap
 * shares an entry, a key or bytes with the lightmap pages or base maps of
 * the environment. A draw wants the bitmap of each part; before the next
 * draw the files of new keys are looked up and the most wanted absent
 * bitmaps read (staged HWT1, checked against the dimensions of the engine)
 * into bytes the cache gives, evicting only bitmaps nobody wanted for
 * model_texture_keep_frames frames. Each draw that binds a bitmap is a GPU
 * sequence; the cache waits for GX before it reuses bytes a draw it has not
 * seen complete may read. Nothing is allocated per frame.
 *
 * HWI-084, the first-person weapon: after the three passes of the objects,
 * the first-person proc of the engine side draws the first-person models of
 * the weapon and the hands in the same three passes, through the same part
 * path, material, skinning buffer and fence, with the projection the engine
 * side set for the frame (wii_gx_first_person_view_set): the x and y of the
 * frame, depth from the first-person near and far clip distances into the
 * front slice [0, WII_GX_FIRST_PERSON_DEPTH_SLICE] of the depth range, so
 * that it is drawn over the world as the stencil of the Xbox makes it; then
 * the projection of the frame again. render.txt first_person=0 draws none
 * (counted as disabled).
 */
#include <gccore.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wii_platform.h"
#include "wii_render.h"
#include "cache_files_wii.h"
#include "content_common.h"
#include "hwm_model.h"
#include "hwt_texture.h"
#include "render_model_material.h"

#define MODEL_VERTEX_BYTES 20u
#define MODEL_VERTEX_COLOUR_OFFSET 12u
#define MODEL_VERTEX_TEXCOORD_OFFSET 16u
#define MODEL_COMPRESSED_VERTEX_TYPE 5
#define MODEL_MAXIMUM_PART_VERTICES 65535L
#define MODEL_SHADER_TYPE_ENVIRONMENT 3
/* GX_Begin counts u16 vertices: a strip goes in pieces that start on an
even triangle (so each keeps its winding) and overlap by two indices; a
list in whole triangles */
#define STRIP_PIECE_TRIANGLES 65532u
#define LIST_PIECE_INDICES 65535u
#define DRAW_SYNC_FIRST 0x4800u
#define DRAW_SYNC_LAST 0x48FFu
#define REFUSALS_LOGGED 16
#define TEXTURE_FAILURES_LOGGED 16
#define MAX_TEXTURE_FILE_BYTES (16u * 1024u * 1024u)
/* HWI-086: the reserve defaults beside wii_render.h (loads, bytes and keep
as the streaming of environment base maps) */
#define TEXTURE_LOADS_DEFAULT 2
#define TEXTURE_FRAME_BYTES_DEFAULT 524288ul
#define TEXTURE_KEEP_FRAMES_DEFAULT 30ul
#define TEXTURE_LOOKUPS_PER_FRAME 8

static const float light_direction[3] = {-0.3487f, 0.2491f, 0.9035f}; /* normalised */
static const unsigned char pass_colour[NUMBER_OF_WII_GX_MODEL_PASSES][4] = {
    {204, 200, 190, 255}, /* solid */
    {150, 190, 235, 255}, /* alpha-blended decal (model shader) */
    {255, 0, 255, 128},   /* transparent shader */
};
static const unsigned char textured_colour[4] = {255, 255, 255, 255};

enum refusal { REFUSE_POINTERS, REFUSE_COUNTS, REFUSE_VERTEX_TYPE, REFUSE_INDEX, REFUSE_CAPACITY, REFUSE_VERTEX,
               NUMBER_OF_REFUSALS };
static const char *const refusal_names[NUMBER_OF_REFUSALS] = {"pointers", "counts", "vertex_type", "index",
                                                              "capacity", "vertex"};

/* HWI-086: the material of each part drawn */
enum material {
    MATERIAL_BASE_MAP,          /* the shader_model base map */
    MATERIAL_TEXTURES_OFF,      /* model_textures=0, or no reserve for this BSP */
    MATERIAL_ENVIRONMENT_SHADER, /* a shader_environment on a model: not this path */
    MATERIAL_TRANSPARENT_SHADER, /* transparent shaders: placeholder */
    MATERIAL_OTHER_SHADER,
    MATERIAL_BASE_DEFAULT,      /* no base map: the Xbox binds the default texture */
    MATERIAL_BASE_NOT_2D,
    MATERIAL_BASE_BAD_BITMAP,
    MATERIAL_TABLE_FULL,        /* more distinct bitmaps than model_texture_entries */
    MATERIAL_NOT_RESIDENT,      /* wanted: looked up or loaded at a later update */
    MATERIAL_REFUSED,           /* missing, oversize, invalid or failed: not for this BSP */
    NUMBER_OF_MATERIALS
};
static const char *const material_names[NUMBER_OF_MATERIALS] = {
    "base_map", "textures_off", "environment_shader", "transparent_shader", "other_shader", "base_default",
    "base_not_2d", "base_bad_bitmap", "table_full", "not_resident", "refused"};
static const char *const feature_names[NUMBER_OF_WII_GX_MODEL_FEATURES] = {
    "detail_map", "multipurpose_map", "reflection_cube_map", "self_illumination", "change_color",
    "texture_animation"};

struct model_stats {
    unsigned long draws, models, parts, refused, triangles, vertices, wraps, fence_waits;
    unsigned long refusals[NUMBER_OF_REFUSALS];
    unsigned long vertex_refusals[NUMBER_OF_HWM_SKIN_RESULTS];
    unsigned long materials[NUMBER_OF_MATERIALS];
    unsigned long features[NUMBER_OF_WII_GX_MODEL_FEATURES]; /* of the base map parts: not drawn */
    unsigned long alpha_tested, blended, both_faces, alpha_planes;
    unsigned long parts_max, triangles_max, used_max;
};

/* a resident bitmap: its texture objects (valid while its entry is resident) */
struct model_texture {
    GXTexObj object, alpha_object;
    int alpha_plane;
};

static struct {
    int enabled;
    unsigned long skin_bytes;
    wii_gx_objects_proc proc;
    /* the skinning buffer, while a BSP is loaded */
    unsigned char *buffer;
    uint32_t bytes, used;
    /* the draw sync fence of the last draw that submitted parts, and its
    GPU sequence (HWI-086: the bitmaps it bound) */
    u16 token;
    int fence_pending;
    uint32_t sequence, fenced_sequence;
    /* the draw in progress */
    int drawing, pass;
    uintptr_t tags_first, tags_end;
    const float (*palette)[3][4];
    uint32_t node_count;
    struct model_stats frame, total;
    long refusals_logged;
    /* HWI-086: the bitmap reserve of the models (render.txt model_textures,
    model_texture_*), while a BSP is loaded */
    struct {
        int disabled;
        unsigned long bytes;
        long entries, loads;
        unsigned long frame_bytes, keep_frames;
        struct wii_model_textures cache;
        struct model_texture *objects;
        char directory[160];
        long binds, bind_failed, failures_logged, gx_invalidations;
    } textures;
    /* HWI-084: the first-person weapon (render.txt first_person,
    first_person_store_pose; both on unless set to 0) */
    struct {
        int disabled, no_store;
        wii_gx_first_person_proc proc;
        int view_valid, drawing;
        struct wii_gx_first_person_view view;
        struct model_stats frame, total;
        unsigned long frames_drawn, frames_without_view;
    } first_person;
} models = {
    .textures = {.bytes = WII_RENDER_DEFAULT_MODEL_TEXTURE_BYTES, .entries = WII_RENDER_DEFAULT_MODEL_TEXTURE_ENTRIES,
                 .loads = TEXTURE_LOADS_DEFAULT, .frame_bytes = TEXTURE_FRAME_BYTES_DEFAULT,
                 .keep_frames = TEXTURE_KEEP_FRAMES_DEFAULT},
};

void wii_gx_models_configure(int enabled, unsigned long skin_bytes)
{
    models.enabled = enabled;
    models.skin_bytes = skin_bytes >= 32u * MODEL_VERTEX_BYTES ? (skin_bytes & ~31ul) : 0;
    if (!models.skin_bytes)
        models.enabled = 0;
    models.token = DRAW_SYNC_FIRST;
}

int wii_gx_models_config_line(const char *line)
{
    if (!strncmp(line, "first_person=", 13))
        models.first_person.disabled = strtol(line + 13, NULL, 10) == 0;
    else if (!strncmp(line, "first_person_store_pose=", 24))
        models.first_person.no_store = strtol(line + 24, NULL, 10) == 0;
    else if (!strncmp(line, "model_textures=", 15))
        models.textures.disabled = strtol(line + 15, NULL, 10) == 0;
    else if (!strncmp(line, "model_texture_bytes=", 20))
        models.textures.bytes = strtoul(line + 20, NULL, 10);
    else if (!strncmp(line, "model_texture_entries=", 22))
        models.textures.entries = strtol(line + 22, NULL, 10);
    else if (!strncmp(line, "model_texture_loads=", 20)) {
        long loads = strtol(line + 20, NULL, 10);
        models.textures.loads = loads < 1 ? 1 : loads > 64 ? 64 : loads;
    } else if (!strncmp(line, "model_texture_frame_bytes=", 26))
        models.textures.frame_bytes = strtoul(line + 26, NULL, 10);
    else if (!strncmp(line, "model_texture_keep_frames=", 26))
        models.textures.keep_frames = strtoul(line + 26, NULL, 10);
    else
        return 0;
    return 1;
}

int wii_gx_first_person_enabled(void)
{
    /* drawn through the path and skinning buffer of the models */
    return models.enabled && !models.first_person.disabled;
}

int wii_gx_first_person_store_pose(void)
{
    return wii_gx_first_person_enabled() && !models.first_person.no_store;
}

void wii_gx_set_first_person_proc(wii_gx_first_person_proc proc)
{
    models.first_person.proc = proc;
}

void wii_gx_first_person_view_set(const struct wii_gx_first_person_view *view)
{
    models.first_person.view_valid = view != NULL;
    if (view)
        models.first_person.view = *view;
}

int wii_gx_models_enabled(void)
{
    return models.enabled;
}

unsigned long wii_gx_models_skin_bytes(void)
{
    return models.enabled ? models.skin_bytes : 0;
}

void wii_gx_set_objects_proc(wii_gx_objects_proc proc)
{
    models.proc = proc;
}

static uint32_t align32(uint32_t value)
{
    return (value + 31u) & ~31u;
}

/* GX has passed the parts of the last draw (or is made to): the bitmaps it
bound are read no more */
static void fence_wait(void)
{
    if (!models.fence_pending)
        return;
    if (GX_GetDrawSync() != models.token) {
        GX_DrawDone();
        models.frame.fence_waits++;
    }
    models.fence_pending = 0;
    wii_model_textures_gpu_done(&models.textures.cache, models.fenced_sequence);
}

/* the wait of the bitmap cache: bytes a draw may still read are about to be reused */
static uint32_t textures_wait(void *context)
{
    (void)context;
    GX_DrawDone();
    models.fence_pending = 0;
    return models.sequence;
}

static uint32_t texture_objects_bytes(long entries)
{
    return align32((uint32_t)((size_t)entries * sizeof(struct model_texture)));
}

unsigned long wii_gx_model_textures_bytes(void)
{
    const long entries = models.textures.entries;
    if (!models.enabled || models.textures.disabled || entries < WII_MODEL_TEXTURES_MINIMUM_ENTRIES ||
        entries > WII_MODEL_TEXTURES_MAXIMUM_ENTRIES || models.textures.bytes > 0x7FFFFFFFul)
        return 0;
    /* the objects, the tables and at least one 32-byte block of arena */
    const unsigned long need = texture_objects_bytes(entries) + wii_model_textures_table_bytes((int)entries) + 32u;
    return models.textures.bytes >= need ? (models.textures.bytes & ~31ul) : 0;
}

static void textures_unbind(void)
{
    struct wii_model_textures *c = &models.textures.cache;
    /* GX finishes reading the bitmaps before their bytes go back */
    fence_wait();
    if (wii_model_textures_ready(c)) {
        const struct wii_texture_cache_stats *st = &c->cache.stats;
        wii_log("RENDER_MODEL_TEXTURE_STATS bind=%ld keys=%d entries=%d arena=%lu tables=%lu used_end=%lu "
                "used_peak=%lu wanted=%lu table_full=%lu looked_up=%lu missing=%lu oversize=%lu loads=%lu "
                "loaded_bytes=%llu load_failed=%lu invalid=%lu deferred=%lu evictions=%lu gpu_waits=%lu no_space=%lu "
                "stale=%lu gx_invalidations=%ld\n", models.textures.binds, c->count, c->capacity,
                (unsigned long)c->arena_bytes, (unsigned long)c->table_bytes, (unsigned long)st->used,
                (unsigned long)st->used_peak, c->stats.wanted, c->stats.table_full, c->stats.looked_up,
                c->stats.missing, c->stats.oversize, c->stats.loads, c->stats.loaded_bytes, c->stats.load_failed,
                c->stats.invalid, c->stats.deferred, st->evictions, st->waits, st->no_space, st->stale,
                models.textures.gx_invalidations);
    }
    wii_model_textures_reset(c);
    models.textures.objects = NULL;
    models.textures.directory[0] = 0;
    models.textures.gx_invalidations = 0;
}

void wii_gx_model_textures_bind(void *memory, unsigned long bytes, const char *directory)
{
    const long entries = models.textures.entries;
    const uint32_t objects = texture_objects_bytes(entries);
    textures_unbind();
    models.textures.binds++;
    if (!memory || !bytes || !directory || bytes > 0x7FFFFFFFul || bytes < objects ||
        snprintf(models.textures.directory, sizeof(models.textures.directory), "%s", directory) >=
            (int)sizeof(models.textures.directory)) {
        models.textures.directory[0] = 0;
        models.textures.bind_failed++;
        wii_log("RENDER_MODEL_TEXTURES_OFF bind=%ld bytes=%lu entries=%ld reason=%s (diagnostic material)\n",
                models.textures.binds, bytes, entries, memory ? "arguments" : "no_reserve");
        return;
    }
    models.textures.objects = memory;
    memset(models.textures.objects, 0, objects);
    if (!wii_model_textures_init(&models.textures.cache, (unsigned char *)memory + objects, (uint32_t)bytes - objects,
                                 (int)entries, TEXTURE_LOOKUPS_PER_FRAME, (int)models.textures.loads,
                                 (uint32_t)models.textures.frame_bytes, (uint32_t)models.textures.keep_frames,
                                 models.sequence, textures_wait, NULL)) {
        models.textures.objects = NULL;
        models.textures.bind_failed++;
        wii_log("RENDER_MODEL_TEXTURES_OFF bind=%ld bytes=%lu entries=%ld reason=init (diagnostic material)\n",
                models.textures.binds, bytes, entries);
        return;
    }
    wii_log("RENDER_MODEL_TEXTURES_BEGIN bind=%ld reserve=%p+%lu objects=%lu tables=%lu arena=%lu entries=%ld "
            "loads=%ld frame_bytes=%lu keep_frames=%lu role=model_base_map wrap=repeat filter=trilinear\n",
            models.textures.binds, memory, bytes, (unsigned long)objects,
            (unsigned long)models.textures.cache.table_bytes, (unsigned long)models.textures.cache.arena_bytes,
            entries, models.textures.loads, models.textures.frame_bytes, models.textures.keep_frames);
}

static void texture_path(char *path, size_t size, long tag, long bitmap)
{
    snprintf(path, size, "%s/%05ld-%03ld.hwt", models.textures.directory, tag, bitmap);
}

/* a wii_model_texture_size_proc: the staged file, by stat */
static uint32_t texture_size(void *context, long tag, long bitmap, int *found)
{
    char path[224];
    struct stat information;
    (void)context;
    texture_path(path, sizeof(path), tag, bitmap);
    *found = stat(path, &information) == 0;
    if (!*found) {
        if (models.textures.failures_logged < TEXTURE_FAILURES_LOGGED) {
            models.textures.failures_logged++;
            wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=model_base error=missing state=refused\n", tag,
                    bitmap);
        }
        return 0;
    }
    /* larger than any file this reads: the cache refuses it as oversize */
    return (unsigned long)information.st_size > MAX_TEXTURE_FILE_BYTES ? 0xFFFFFFFFu : (uint32_t)information.st_size;
}

static int texture_fail(const struct wii_model_texture_slot *slot, int result, const char *why)
{
    if (models.textures.failures_logged < TEXTURE_FAILURES_LOGGED) {
        models.textures.failures_logged++;
        wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=model_base error=%s state=%s\n", slot->tag, slot->bitmap,
                why, result == 1 ? "retry_later" : "refused");
    }
    return result;
}

/* a wii_model_texture_load_proc: the file whole into the bytes of the
ticket, checked as a drawable HWT1 (2D, one face, the dimensions of the
engine, a GX-loadable first image and alpha plane on 32 bytes), and its
texture objects made as stage 0 of _rasterizer_model_draw samples it
(D3DTADDRESS_WRAP in u and v; linear magnification, minification and mip
filters). Nothing is drawn until the cache publishes it */
static int texture_load(void *context, int index, const struct wii_model_texture_slot *slot,
                        const struct wii_texture_cache_ticket *ticket)
{
    struct model_texture *t = &models.textures.objects[index];
    struct hwt_header header;
    struct hwt_image image, alpha_image;
    unsigned char extra;
    char path[224];
    (void)context;
    memset(t, 0, sizeof(*t));
    texture_path(path, sizeof(path), slot->tag, slot->bitmap);
    FILE *file = fopen(path, "rb");
    if (!file)
        return texture_fail(slot, 1, "io");
    memset(ticket->data + ticket->bytes, 0, align32(ticket->bytes) - ticket->bytes);
    int read_ok = fread(ticket->data, 1, ticket->bytes, file) == ticket->bytes && fread(&extra, 1, 1, file) == 0;
    if (fclose(file) != 0)
        read_ok = 0;
    if (!read_ok)
        return texture_fail(slot, 1, "io");
    enum content_error error = hwt_parse(ticket->data, ticket->bytes, &header);
    if (error != CONTENT_OK)
        return texture_fail(slot, 2, content_error_name(error));
    if (header.type != HWT_TYPE_2D || header.faces != 1 || header.depth != 1 ||
        (slot->width > 0 && ((int)header.width != slot->width || (int)header.height != slot->height)))
        return texture_fail(slot, 2, "dimensions");
    if (hwt_image_at(&header, 0, &image) != CONTENT_OK || !image.gx_loadable || (image.offset & 31u))
        return texture_fail(slot, 2, "image");
    t->alpha_plane = header.alpha_kind == HWT_ALPHA_PLANE;
    if (t->alpha_plane && (hwt_alpha_image_at(&header, 0, &alpha_image) != CONTENT_OK || (alpha_image.offset & 31u))) {
        t->alpha_plane = 0;
        return texture_fail(slot, 2, "alpha_plane");
    }
    const int levels = (int)header.levels;
    GX_InitTexObj(&t->object, ticket->data + image.offset, (u16)image.width, (u16)image.height, (u8)header.gx_format,
                  GX_REPEAT, GX_REPEAT, levels > 1 ? GX_TRUE : GX_FALSE);
    if (levels > 1)
        GX_InitTexObjLOD(&t->object, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(levels - 1), 0.0f, GX_FALSE, GX_FALSE,
                         GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(&t->object, GX_LINEAR, GX_LINEAR);
    if (t->alpha_plane) {
        /* the same texcoord, wrap, filter and chain: one RGBA texel */
        GX_InitTexObj(&t->alpha_object, ticket->data + alpha_image.offset, (u16)image.width, (u16)image.height,
                      (u8)header.alpha_gx_format, GX_REPEAT, GX_REPEAT, levels > 1 ? GX_TRUE : GX_FALSE);
        if (levels > 1)
            GX_InitTexObjLOD(&t->alpha_object, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(levels - 1), 0.0f, GX_FALSE,
                             GX_FALSE, GX_ANISO_1);
        else
            GX_InitTexObjFilterMode(&t->alpha_object, GX_LINEAR, GX_LINEAR);
    }
    /* the CPU wrote these bytes: GX reads memory */
    DCFlushRange(ticket->data, align32(ticket->bytes));
    return 0;
}

/* before the draws of a frame: what the last draw wanted, looked up and loaded */
static void textures_update(void)
{
    if (!wii_model_textures_ready(&models.textures.cache))
        return;
    if (wii_model_textures_update(&models.textures.cache, texture_size, texture_load, NULL) > 0) {
        /* new texels where the texture cache of GX may hold evicted ones */
        GX_InvalidateTexAll();
        models.textures.gx_invalidations++;
    }
}

void wii_gx_models_bind(void *buffer, unsigned long bytes)
{
    fence_wait();
    models.buffer = buffer;
    models.bytes = buffer ? (uint32_t)(bytes & ~31ul) : 0;
    models.used = 0;
}

void wii_gx_models_unbind(void)
{
    /* the store is reused for the textures of the next BSP: GX must be done reading */
    fence_wait();
    textures_unbind();
    models.buffer = NULL;
    models.bytes = models.used = 0;
}

/* the tag slot's tag data (range 0 of the load), where the model buffers lie */
static int tag_data_range(uintptr_t *first, uintptr_t *end)
{
    struct wii_cache_load_report load;
    size_t slot_bytes = 0;
    uintptr_t slot = (uintptr_t)wii_arena_region(_wii_arena_tag_cache, &slot_bytes);
    wii_cache_load_report_get(&load);
    if (!slot || load.range_count < 1 || load.range_offset[0] + load.range_size[0] > slot_bytes)
        return 0;
    *first = slot + load.range_offset[0];
    *end = *first + load.range_size[0];
    return 1;
}

static int in_tags(const void *pointer, uint32_t bytes)
{
    uintptr_t p = (uintptr_t)pointer;
    return p >= models.tags_first && p <= models.tags_end && bytes <= models.tags_end - p;
}

static void refuse(enum refusal why, int detail, const struct wii_gx_model_part *part)
{
    models.frame.refused++;
    models.frame.refusals[why]++;
    if (why == REFUSE_VERTEX && (unsigned)detail < NUMBER_OF_HWM_SKIN_RESULTS)
        models.frame.vertex_refusals[detail]++;
    if (models.refusals_logged < REFUSALS_LOGGED) {
        models.refusals_logged++;
        wii_log("RENDER_MODEL_PART_REFUSED reason=%s detail=%s vertex_type=%d vertices=%ld indices=%ld strip=%d "
                "shader_type=%d nodes=%lu pass=%d first_person=%d skin_bytes=%lu (not drawn)\n", refusal_names[why],
                why == REFUSE_VERTEX ? hwm_skin_result_name((enum hwm_skin_result)detail) : "-", part->vertex_type,
                part->vertex_count, part->index_count, part->strip, part->shader_type,
                (unsigned long)models.node_count, models.pass, models.first_person.drawing,
                (unsigned long)models.bytes);
    }
}

int wii_gx_model_begin(const float (*palette)[3][4], int node_count)
{
    if (!models.drawing || !palette || node_count < 1 || node_count > WII_GX_MODEL_MAXIMUM_NODES) {
        models.palette = NULL;
        models.node_count = 0;
        return 0;
    }
    models.palette = palette;
    models.node_count = (uint32_t)node_count;
    models.frame.models++;
    return 1;
}

void wii_gx_model_end(void)
{
    models.palette = NULL;
    models.node_count = 0;
}


static unsigned char shade(const float normal[3], int two_sided, const unsigned char colour[4], unsigned channel)
{
    float d = normal[0] * light_direction[0] + normal[1] * light_direction[1] + normal[2] * light_direction[2];
    if (two_sided)
        d = fabsf(d);
    else if (d < 0.0f)
        d = 0.0f;
    float value = (float)colour[channel] * (0.3f + 0.7f * d);
    return (unsigned char)(value > 255.0f ? 255.0f : value);
}

static void emit_vertex(u16 index)
{
    GX_Position1x16(index);
    GX_Color1x16(index);
    GX_TexCoord1x16(index);
}

static void emit(const u16 *indices, uint32_t count, int strip)
{
    if (strip) {
        uint32_t triangles = count - 2u;
        for (uint32_t first = 0; first < triangles; first += STRIP_PIECE_TRIANGLES) {
            uint32_t piece = triangles - first < STRIP_PIECE_TRIANGLES ? triangles - first : STRIP_PIECE_TRIANGLES;
            GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16)(piece + 2u));
            for (uint32_t k = first; k < first + piece + 2u; ++k)
                emit_vertex(indices[k]);
            GX_End();
        }
    } else {
        for (uint32_t first = 0; first < count; first += LIST_PIECE_INDICES) {
            uint32_t piece = count - first < LIST_PIECE_INDICES ? count - first : LIST_PIECE_INDICES;
            GX_Begin(GX_TRIANGLES, GX_VTXFMT3, (u16)piece);
            for (uint32_t k = first; k < first + piece; ++k)
                emit_vertex(indices[k]);
            GX_End();
        }
    }
}

/* HWI-086: the material of a part, and its bitmap slot when it is the base map */
static enum material part_material(const struct wii_gx_model_part *part, int *slot)
{
    const struct wii_gx_model_material *m = &part->material;
    *slot = -1;
    switch (m->base) {
    case _wii_gx_model_base_not_model_shader:
        if (models.pass == _wii_gx_model_pass_transparent)
            return MATERIAL_TRANSPARENT_SHADER;
        return part->shader_type == MODEL_SHADER_TYPE_ENVIRONMENT ? MATERIAL_ENVIRONMENT_SHADER
                                                                  : MATERIAL_OTHER_SHADER;
    case _wii_gx_model_base_bitmap:
        break;
    case _wii_gx_model_base_default:
        return MATERIAL_BASE_DEFAULT;
    case _wii_gx_model_base_not_2d:
        return MATERIAL_BASE_NOT_2D;
    default:
        return MATERIAL_BASE_BAD_BITMAP;
    }
    if (!wii_model_textures_ready(&models.textures.cache))
        return MATERIAL_TEXTURES_OFF;
    int index = wii_model_textures_want(&models.textures.cache, m->base_tag, m->base_bitmap, m->base_width,
                                        m->base_height);
    if (index < 0)
        return MATERIAL_TABLE_FULL;
    if (!wii_model_textures_resident(&models.textures.cache, index))
        return models.textures.cache.cache.entries[index].state == _wii_texture_cache_refused ? MATERIAL_REFUSED
                                                                                              : MATERIAL_NOT_RESIDENT;
    *slot = index;
    return MATERIAL_BASE_MAP;
}

/* the TEV, texture matrix and blend of a base map part */
static void apply_base_map(const struct wii_gx_model_part *part, int slot, const struct wii_model_material_state *state)
{
    const struct model_texture *t = &models.textures.objects[slot];
    float matrix[2][4];
    Mtx mtx;
    wii_model_texture_matrix(part->material.texcoord, matrix);
    memset(mtx, 0, sizeof(mtx));
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 4; ++c)
            mtx[r][c] = matrix[r][c];
    GX_LoadTexMtxImm(mtx, GX_TEXMTX1, GX_MTX2x4);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1);
    GX_LoadTexObj((GXTexObj *)&t->object, GX_TEXMAP0);
    /* colour: the base map times the light colour; alpha: the base map (the
    final combiner of the Xbox outputs T0 alpha) */
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    if (t->alpha_plane) {
        /* a split texture: its I4/I8 plane (alpha = intensity) on a second stage */
        GX_LoadTexObj((GXTexObj *)&t->alpha_object, GX_TEXMAP1);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        models.frame.alpha_planes++;
    }
    GX_SetNumTevStages(t->alpha_plane ? 2 : 1);
    if (state->alpha_test) {
        /* D3DCMP_GREATER against 0x7F; depth compared after texturing so a
        discarded texel writes no depth */
        GX_SetZCompLoc(GX_FALSE);
        GX_SetAlphaCompare(GX_GREATER, (u8)WII_MODEL_ALPHA_REFERENCE, GX_AOP_AND, GX_ALWAYS, 0);
    } else {
        GX_SetZCompLoc(GX_TRUE);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    }
    if (state->blend)
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    else
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, state->depth_write ? GX_TRUE : GX_FALSE);
}

/* the GX state of the diagnostic material: blend and depth of the pass */
static int state_textured;

static void set_pass_blend(int pass)
{
    static const u8 blend[NUMBER_OF_WII_GX_MODEL_PASSES] = {GX_BM_NONE, GX_BM_NONE, GX_BM_BLEND};
    static const u8 depth_write[NUMBER_OF_WII_GX_MODEL_PASSES] = {GX_TRUE, GX_FALSE, GX_FALSE};

    if (blend[pass] == GX_BM_BLEND)
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    else
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, depth_write[pass]);
}

static void apply_diagnostic(void)
{
    if (!state_textured)
        return;
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    set_pass_blend(models.pass);
    state_textured = 0;
}

int wii_gx_model_part_draw(const struct wii_gx_model_part *part)
{
    if (!models.drawing || !models.palette || !part)
        return 0;
    long vertex_count = part->vertex_count, index_count = part->index_count;
    if (part->vertex_type != MODEL_COMPRESSED_VERTEX_TYPE) {
        refuse(REFUSE_VERTEX_TYPE, 0, part);
        return 0;
    }
    if (vertex_count < 1 || vertex_count > MODEL_MAXIMUM_PART_VERTICES || index_count < 3 ||
        index_count > 3L * MODEL_MAXIMUM_PART_VERTICES || (!part->strip && index_count % 3)) {
        refuse(REFUSE_COUNTS, 0, part);
        return 0;
    }
    const unsigned char *vertices = part->vertices;
    const u16 *indices = part->indices;
    if (((uintptr_t)vertices & 3u) || ((uintptr_t)indices & 1u) ||
        !in_tags(vertices, (uint32_t)vertex_count * HWM_SKIN_VERTEX_BYTES) ||
        !in_tags(indices, (uint32_t)index_count * 2u)) {
        refuse(REFUSE_POINTERS, 0, part);
        return 0;
    }
    /* every index below the vertices of the part, before GX is given any */
    for (long k = 0; k < index_count; ++k) {
        if (indices[k] >= vertex_count) {
            refuse(REFUSE_INDEX, 0, part);
            return 0;
        }
    }
    uint32_t need = align32((uint32_t)vertex_count * MODEL_VERTEX_BYTES);
    if (need > models.bytes) {
        refuse(REFUSE_CAPACITY, 0, part);
        return 0;
    }
    /* HWI-086: its material (a base map wanted now is drawn once resident) */
    int slot;
    const enum material material = part_material(part, &slot);
    struct wii_model_material_state state;
    wii_model_material_state(part->material.shader_flags, &state);
    const unsigned char *colour = material == MATERIAL_BASE_MAP ? textured_colour : pass_colour[models.pass];
    if (models.bytes - models.used < need) {
        /* what is left will not hold it: GX finishes what it reads, then from the start */
        GX_DrawDone();
        GX_InvVtxCache();
        models.used = 0;
        models.frame.wraps++;
    }
    unsigned char *out = models.buffer + models.used;
    const int two_sided = part->two_sided != 0;
    for (long v = 0; v < vertex_count; ++v) {
        const unsigned char *in = vertices + HWM_SKIN_VERTEX_BYTES * (uint32_t)v;
        struct hwm_skin_vertex decoded;
        enum hwm_skin_result result = hwm_skin_decode_le(in, models.node_count, &decoded);
        if (result != HWM_SKIN_OK) {
            refuse(REFUSE_VERTEX, (int)result, part);
            return 0;
        }
        float position[3], normal[3];
        int16_t texcoord[2];
        hwm_skin_apply(&decoded, models.palette, position, normal);
        hwm_texcoord_decode_le(in, texcoord);
        unsigned char *o = out + MODEL_VERTEX_BYTES * (uint32_t)v;
        memcpy(o, position, 12);
        o[MODEL_VERTEX_COLOUR_OFFSET + 0] = shade(normal, two_sided, colour, 0);
        o[MODEL_VERTEX_COLOUR_OFFSET + 1] = shade(normal, two_sided, colour, 1);
        o[MODEL_VERTEX_COLOUR_OFFSET + 2] = shade(normal, two_sided, colour, 2);
        o[MODEL_VERTEX_COLOUR_OFFSET + 3] = colour[3];
        /* (native order: GX reads big-endian s16) */
        memcpy(o + MODEL_VERTEX_TEXCOORD_OFFSET, texcoord, 4);
    }
    /* the CPU wrote these bytes: GX reads memory */
    DCFlushRange(out, need);
    if (material == MATERIAL_BASE_MAP) {
        apply_base_map(part, slot, &state);
        wii_model_textures_used(&models.textures.cache, slot, models.sequence);
        state_textured = 1;
        models.frame.alpha_tested += (unsigned long)state.alpha_test;
        models.frame.blended += (unsigned long)state.blend;
        for (int f = 0; f < NUMBER_OF_WII_GX_MODEL_FEATURES; ++f)
            models.frame.features[f] += (part->material.features >> f) & 1u;
    } else {
        apply_diagnostic();
    }
    models.frame.materials[material]++;
    models.frame.both_faces += (unsigned long)two_sided;
    GX_SetCullMode(two_sided ? GX_CULL_NONE : GX_CULL_BACK);
    GX_SetArray(GX_VA_POS, out, MODEL_VERTEX_BYTES);
    GX_SetArray(GX_VA_CLR0, out + MODEL_VERTEX_COLOUR_OFFSET, MODEL_VERTEX_BYTES);
    GX_SetArray(GX_VA_TEX0, out + MODEL_VERTEX_TEXCOORD_OFFSET, MODEL_VERTEX_BYTES);
    emit(indices, (uint32_t)index_count, part->strip);
    models.used += need;
    if (models.used > models.frame.used_max)
        models.frame.used_max = models.used;
    models.frame.parts++;
    models.frame.vertices += (unsigned long)vertex_count;
    models.frame.triangles += (unsigned long)(part->strip ? index_count - 2 : index_count / 3);
    return 1;
}

static void stats_merge(struct model_stats *into, const struct model_stats *from)
{
    into->draws += from->draws;
    into->models += from->models;
    into->parts += from->parts;
    into->refused += from->refused;
    into->triangles += from->triangles;
    into->vertices += from->vertices;
    into->wraps += from->wraps;
    into->fence_waits += from->fence_waits;
    for (int i = 0; i < NUMBER_OF_REFUSALS; ++i)
        into->refusals[i] += from->refusals[i];
    for (int i = 0; i < NUMBER_OF_HWM_SKIN_RESULTS; ++i)
        into->vertex_refusals[i] += from->vertex_refusals[i];
    for (int i = 0; i < NUMBER_OF_MATERIALS; ++i)
        into->materials[i] += from->materials[i];
    for (int i = 0; i < NUMBER_OF_WII_GX_MODEL_FEATURES; ++i)
        into->features[i] += from->features[i];
    into->alpha_tested += from->alpha_tested;
    into->blended += from->blended;
    into->both_faces += from->both_faces;
    into->alpha_planes += from->alpha_planes;
    if (from->parts > into->parts_max)
        into->parts_max = from->parts;
    if (from->triangles > into->triangles_max)
        into->triangles_max = from->triangles;
    if (from->used_max > into->used_max)
        into->used_max = from->used_max;
}

/* what the first-person passes added to the counts of the frame */
static void stats_delta(struct model_stats *into, const struct model_stats *after, const struct model_stats *before)
{
    memset(into, 0, sizeof(*into));
    into->models = after->models - before->models;
    into->parts = after->parts - before->parts;
    into->refused = after->refused - before->refused;
    into->triangles = after->triangles - before->triangles;
    into->vertices = after->vertices - before->vertices;
    into->wraps = after->wraps - before->wraps;
    into->fence_waits = after->fence_waits - before->fence_waits;
    for (int i = 0; i < NUMBER_OF_REFUSALS; ++i)
        into->refusals[i] = after->refusals[i] - before->refusals[i];
    for (int i = 0; i < NUMBER_OF_HWM_SKIN_RESULTS; ++i)
        into->vertex_refusals[i] = after->vertex_refusals[i] - before->vertex_refusals[i];
    for (int i = 0; i < NUMBER_OF_MATERIALS; ++i)
        into->materials[i] = after->materials[i] - before->materials[i];
    for (int i = 0; i < NUMBER_OF_WII_GX_MODEL_FEATURES; ++i)
        into->features[i] = after->features[i] - before->features[i];
    into->alpha_tested = after->alpha_tested - before->alpha_tested;
    into->blended = after->blended - before->blended;
    into->both_faces = after->both_faces - before->both_faces;
    into->alpha_planes = after->alpha_planes - before->alpha_planes;
    into->used_max = after->used_max;
}

static void set_pass_state(int pass)
{
    models.pass = pass;
    /* the diagnostic material of the pass (a base map part sets its own) */
    state_textured = 1;
    apply_diagnostic();
}

/* HWI-084: the three passes of the first-person weapon, with its projection,
then the projection of the frame again */
static void first_person_passes(void)
{
    struct model_stats before = models.frame;
    Mtx44 projection;

    memset(&models.first_person.frame, 0, sizeof(models.first_person.frame));
    if (!wii_gx_first_person_enabled() || !models.first_person.proc)
        return;
    if (!models.first_person.view_valid) {
        models.first_person.frames_without_view++;
        return;
    }
    memcpy(projection, models.first_person.view.projection, sizeof(projection));
    GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
    models.first_person.drawing = 1;
    for (int pass = 0; pass < NUMBER_OF_WII_GX_MODEL_PASSES; ++pass) {
        set_pass_state(pass);
        models.first_person.proc(pass);
    }
    models.first_person.drawing = 0;
    memcpy(projection, models.first_person.view.restore, sizeof(projection));
    GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
    stats_delta(&models.first_person.frame, &models.frame, &before);
    models.first_person.frame.draws = 1;
    models.first_person.frames_drawn += models.first_person.frame.parts > 0;
    stats_merge(&models.first_person.total, &models.first_person.frame);
}

unsigned long wii_gx_models_draw(void)
{
    memset(&models.frame, 0, sizeof(models.frame));
    memset(&models.first_person.frame, 0, sizeof(models.first_person.frame));
    if (!models.enabled || (!models.proc && !models.first_person.proc) || !models.buffer ||
        !tag_data_range(&models.tags_first, &models.tags_end))
        return 0;
    /* the parts of the last draw may still be read from the start of the buffer */
    fence_wait();
    /* HWI-086: the bitmaps the last draw wanted, before any part binds one;
    then this draw is a new GPU sequence */
    textures_update();
    models.sequence++;
    models.used = 0;
    GX_InvVtxCache();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    /* the stored s16: the texture matrix normalises it (1 / 32767) */
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetCurrentMtx(GX_PNMTX0);
    /* D3DRS_COLORWRITEENABLE: red, green and blue */
    GX_SetAlphaUpdate(GX_FALSE);
    models.drawing = 1;
    for (int pass = 0; models.proc && pass < NUMBER_OF_WII_GX_MODEL_PASSES; ++pass) {
        set_pass_state(pass);
        models.proc(pass);
    }
    /* HWI-084: the first-person weapon over them (the render_objects of
    render.c draws it first, under the stencil it writes; here its depth slice) */
    first_person_passes();
    models.drawing = 0;
    models.palette = NULL;
    models.node_count = 0;
    /* what the environment and the interface expect */
    state_textured = 1;
    apply_diagnostic();
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetCullMode(GX_CULL_BACK);
    if (models.frame.parts) {
        models.token = models.token >= DRAW_SYNC_LAST ? DRAW_SYNC_FIRST : (u16)(models.token + 1u);
        GX_SetDrawSync(models.token);
        models.fence_pending = 1;
        models.fenced_sequence = models.sequence;
    }
    models.frame.draws = 1;
    stats_merge(&models.total, &models.frame);
    return models.frame.parts;
}

void wii_gx_models_last(unsigned long *parts, unsigned long *refused, unsigned long *triangles)
{
    *parts = models.frame.parts;
    *refused = models.frame.refused;
    *triangles = models.frame.triangles;
}

void wii_gx_first_person_last(unsigned long *parts, unsigned long *refused, unsigned long *triangles)
{
    *parts = models.first_person.frame.parts;
    *refused = models.first_person.frame.refused;
    *triangles = models.first_person.frame.triangles;
}

/* HWI-086: the parts of each material, and the features of the base map parts not drawn */
static void materials_report(const char *what, const struct model_stats *s)
{
    char text[1024];
    int at = snprintf(text, sizeof(text), "RENDER_MODEL_MATERIALS of=%s textures=%d", what,
                      models.enabled && !models.textures.disabled);
    for (int i = 0; i < NUMBER_OF_MATERIALS && at > 0 && at < (int)sizeof(text); ++i)
        at += snprintf(text + at, sizeof(text) - (size_t)at, " %s=%lu", material_names[i], s->materials[i]);
    for (int i = 0; i < NUMBER_OF_WII_GX_MODEL_FEATURES && at > 0 && at < (int)sizeof(text); ++i)
        at += snprintf(text + at, sizeof(text) - (size_t)at, " not_drawn_%s=%lu", feature_names[i], s->features[i]);
    if (at > 0 && at < (int)sizeof(text))
        snprintf(text + at, sizeof(text) - (size_t)at, " alpha_tested=%lu blended=%lu both_faces=%lu alpha_planes=%lu "
                 "lighting=diagnostic_directional fog=not_drawn decal_zbias=not_applied\n", s->alpha_tested,
                 s->blended, s->both_faces, s->alpha_planes);
    wii_log("%s", text);
}

void wii_gx_models_report(void)
{
    const struct model_stats *f = &models.first_person.total;
    wii_log("RENDER_FIRST_PERSON_TOTALS enabled=%d store_pose=%d draws=%lu frames_drawn=%lu frames_without_view=%lu "
            "models=%lu parts=%lu refused=%lu triangles=%lu vertices=%lu parts_max=%lu triangles_max=%lu wraps=%lu "
            "refused_pointers=%lu refused_counts=%lu refused_vertex_type=%lu refused_index=%lu refused_capacity=%lu "
            "refused_vertex=%lu vertex_node_stride=%lu vertex_node_range=%lu vertex_weight_range=%lu "
            "vertex_position=%lu depth_slice=%.6f material=model_base_map+diagnostic_light (included in RENDER_MODEL_TOTALS)\n",
            wii_gx_first_person_enabled(), wii_gx_first_person_store_pose(), f->draws, models.first_person.frames_drawn,
            models.first_person.frames_without_view, f->models, f->parts, f->refused, f->triangles, f->vertices,
            f->parts_max, f->triangles_max, f->wraps, f->refusals[REFUSE_POINTERS], f->refusals[REFUSE_COUNTS],
            f->refusals[REFUSE_VERTEX_TYPE], f->refusals[REFUSE_INDEX], f->refusals[REFUSE_CAPACITY],
            f->refusals[REFUSE_VERTEX], f->vertex_refusals[HWM_SKIN_NODE_STRIDE], f->vertex_refusals[HWM_SKIN_NODE_RANGE],
            f->vertex_refusals[HWM_SKIN_WEIGHT_RANGE], f->vertex_refusals[HWM_SKIN_POSITION],
            (double)WII_GX_FIRST_PERSON_DEPTH_SLICE);
    const struct model_stats *t = &models.total;
    wii_log("RENDER_MODEL_TOTALS enabled=%d skin_bytes=%lu draws=%lu models=%lu parts=%lu refused=%lu triangles=%lu "
            "vertices=%lu parts_max=%lu triangles_max=%lu skin_used_max=%lu wraps=%lu fence_waits=%lu "
            "refused_pointers=%lu refused_counts=%lu refused_vertex_type=%lu refused_index=%lu refused_capacity=%lu "
            "refused_vertex=%lu vertex_node_stride=%lu vertex_node_range=%lu vertex_weight_range=%lu "
            "vertex_position=%lu material=model_base_map+diagnostic_light\n", models.enabled, models.skin_bytes, t->draws,
            t->models, t->parts, t->refused, t->triangles, t->vertices, t->parts_max, t->triangles_max, t->used_max,
            t->wraps, t->fence_waits, t->refusals[REFUSE_POINTERS], t->refusals[REFUSE_COUNTS],
            t->refusals[REFUSE_VERTEX_TYPE], t->refusals[REFUSE_INDEX], t->refusals[REFUSE_CAPACITY],
            t->refusals[REFUSE_VERTEX], t->vertex_refusals[HWM_SKIN_NODE_STRIDE],
            t->vertex_refusals[HWM_SKIN_NODE_RANGE], t->vertex_refusals[HWM_SKIN_WEIGHT_RANGE],
            t->vertex_refusals[HWM_SKIN_POSITION]);
    materials_report("first_person", f);
    materials_report("all", t);
}
