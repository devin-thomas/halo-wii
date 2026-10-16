/*
 * RENDER_MODELS_GX.C
 *
 * The GX side of the object models (HWI-082, wii_render.h): each model
 * part the engine side hands over is skinned on the CPU from the tag slot
 * and drawn through GX inside the frame, after the environment, with the
 * view render_gx.c loaded (GX_PNMTX0: the engine's world_to_view).
 * Strict C11 against libogc; no engine header.
 *
 * Geometry: the part's vertex buffer stays where the staged map holds it,
 * in the tag slot's tag data (little-endian Xbox compressed model vertices:
 * map_stage.py converts only what the game reads), and its strip or list of
 * u16 indices (big-endian: map_stage.py converts model indices). Each vertex
 * is decoded and skinned with the model's node matrices by hwm_model.c's
 * one profile (hwm_skin_decode_le, hwm_skin_apply: node = byte / 3, weight
 * s16 / 32767 as the Xbox's vertex declaration reads them); a part with any
 * vertex outside it is refused whole, counted and logged, never drawn with
 * another node. The skinned position (F32 xyz) and a diagnostic colour
 * (RGBA8) go to the skinning buffer, 16 bytes a vertex, which GX reads as
 * indexed arrays (GX_VTXFMT3; the boot mode's interface owns GX_VTXFMT2).
 *
 * The skinning buffer: one block of model_skin_bytes (render.txt; default
 * WII_RENDER_DEFAULT_MODEL_SKIN_BYTES) from the BSP's store (render_gx.c:
 * the map's render pool, or a heap block in the boot mode), taken after the
 * material tables and given back at the BSP's end. Within a frame it fills
 * from its start; a part that does not fit what is left waits for GX
 * (GX_DrawDone) and starts it again (counted: wraps); a part larger than the
 * whole buffer is refused (counted: capacity). Every draw ends with a draw
 * sync token, and the next draw, or the buffer's release, waits for it
 * unless GX has passed it, so GX never reads memory the CPU rewrites.
 *
 * Material (diagnostic, labelled): no texture or shader is applied. One
 * fixed directional light over the skinned normal (ambient 0.3, diffuse
 * 0.7; two-sided parts by the absolute value). Solid parts grey, opaque;
 * the alpha-blended decals of model shaders light blue, depth test without
 * depth writes; transparent shaders' parts a 50% magenta blend without depth
 * writes, after every opaque part. Culling is GX_CULL_BACK, the Xbox's
 * D3DCULL_CCW for models as for the environment; two-sided model shaders
 * draw both faces (GX_CULL_NONE).
 */
#include <gccore.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wii_platform.h"
#include "wii_render.h"
#include "cache_files_wii.h"
#include "content_common.h"
#include "hwm_model.h"

#define MODEL_VERTEX_BYTES 16u
#define MODEL_COMPRESSED_VERTEX_TYPE 5
#define MODEL_MAXIMUM_PART_VERTICES 65535L
/* GX_Begin counts u16 vertices: a strip goes in pieces that start on an
even triangle (so each keeps its winding) and overlap by two indices; a
list in whole triangles */
#define STRIP_PIECE_TRIANGLES 65532u
#define LIST_PIECE_INDICES 65535u
#define DRAW_SYNC_FIRST 0x4800u
#define DRAW_SYNC_LAST 0x48FFu
#define REFUSALS_LOGGED 16

static const float light_direction[3] = {-0.3487f, 0.2491f, 0.9035f}; /* normalised */
static const unsigned char pass_colour[NUMBER_OF_WII_GX_MODEL_PASSES][4] = {
    {204, 200, 190, 255}, /* solid */
    {150, 190, 235, 255}, /* alpha-blended decal (model shader) */
    {255, 0, 255, 128},   /* transparent shader */
};

enum refusal { REFUSE_POINTERS, REFUSE_COUNTS, REFUSE_VERTEX_TYPE, REFUSE_INDEX, REFUSE_CAPACITY, REFUSE_VERTEX,
               NUMBER_OF_REFUSALS };
static const char *const refusal_names[NUMBER_OF_REFUSALS] = {"pointers", "counts", "vertex_type", "index",
                                                              "capacity", "vertex"};

struct model_stats {
    unsigned long draws, models, parts, refused, triangles, vertices, wraps, fence_waits;
    unsigned long refusals[NUMBER_OF_REFUSALS];
    unsigned long vertex_refusals[NUMBER_OF_HWM_SKIN_RESULTS];
    unsigned long parts_max, triangles_max, used_max;
};

static struct {
    int enabled;
    unsigned long skin_bytes;
    wii_gx_objects_proc proc;
    /* the skinning buffer, while a BSP is loaded */
    unsigned char *buffer;
    uint32_t bytes, used;
    /* the draw sync fence of the last draw that submitted parts */
    u16 token;
    int fence_pending;
    /* the draw in progress */
    int drawing, pass;
    uintptr_t tags_first, tags_end;
    const float (*palette)[3][4];
    uint32_t node_count;
    struct model_stats frame, total;
    long refusals_logged;
} models;

void wii_gx_models_configure(int enabled, unsigned long skin_bytes)
{
    models.enabled = enabled;
    models.skin_bytes = skin_bytes >= 32u * MODEL_VERTEX_BYTES ? (skin_bytes & ~31ul) : 0;
    if (!models.skin_bytes)
        models.enabled = 0;
    models.token = DRAW_SYNC_FIRST;
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

/* GX has passed the last draw's parts (or is made to) */
static void fence_wait(void)
{
    if (!models.fence_pending)
        return;
    if (GX_GetDrawSync() != models.token) {
        GX_DrawDone();
        models.frame.fence_waits++;
    }
    models.fence_pending = 0;
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
    /* the store is reused for the next BSP's textures: GX must be done reading */
    fence_wait();
    models.buffer = NULL;
    models.bytes = models.used = 0;
}

static uint32_t align32(uint32_t value)
{
    return (value + 31u) & ~31u;
}

/* the tag slot's tag data (range 0 of the load), where model buffers lie */
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
                "shader_type=%d nodes=%lu pass=%d skin_bytes=%lu (not drawn)\n", refusal_names[why],
                why == REFUSE_VERTEX ? hwm_skin_result_name((enum hwm_skin_result)detail) : "-", part->vertex_type,
                part->vertex_count, part->index_count, part->strip, part->shader_type,
                (unsigned long)models.node_count, models.pass, (unsigned long)models.bytes);
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

static unsigned char shade(const float normal[3], int two_sided, unsigned channel)
{
    float d = normal[0] * light_direction[0] + normal[1] * light_direction[1] + normal[2] * light_direction[2];
    if (two_sided)
        d = fabsf(d);
    else if (d < 0.0f)
        d = 0.0f;
    float value = (float)pass_colour[models.pass][channel] * (0.3f + 0.7f * d);
    return (unsigned char)(value > 255.0f ? 255.0f : value);
}

static void emit(const u16 *indices, uint32_t count, int strip)
{
    if (strip) {
        uint32_t triangles = count - 2u;
        for (uint32_t first = 0; first < triangles; first += STRIP_PIECE_TRIANGLES) {
            uint32_t piece = triangles - first < STRIP_PIECE_TRIANGLES ? triangles - first : STRIP_PIECE_TRIANGLES;
            GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16)(piece + 2u));
            for (uint32_t k = first; k < first + piece + 2u; ++k) {
                GX_Position1x16(indices[k]);
                GX_Color1x16(indices[k]);
            }
            GX_End();
        }
    } else {
        for (uint32_t first = 0; first < count; first += LIST_PIECE_INDICES) {
            uint32_t piece = count - first < LIST_PIECE_INDICES ? count - first : LIST_PIECE_INDICES;
            GX_Begin(GX_TRIANGLES, GX_VTXFMT3, (u16)piece);
            for (uint32_t k = first; k < first + piece; ++k) {
                GX_Position1x16(indices[k]);
                GX_Color1x16(indices[k]);
            }
            GX_End();
        }
    }
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
    /* every index below the part's vertices, before GX is given any */
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
        struct hwm_skin_vertex decoded;
        enum hwm_skin_result result = hwm_skin_decode_le(vertices + HWM_SKIN_VERTEX_BYTES * (uint32_t)v,
                                                         models.node_count, &decoded);
        if (result != HWM_SKIN_OK) {
            refuse(REFUSE_VERTEX, (int)result, part);
            return 0;
        }
        float position[3], normal[3];
        hwm_skin_apply(&decoded, models.palette, position, normal);
        unsigned char *o = out + MODEL_VERTEX_BYTES * (uint32_t)v;
        memcpy(o, position, 12);
        o[12] = shade(normal, two_sided, 0);
        o[13] = shade(normal, two_sided, 1);
        o[14] = shade(normal, two_sided, 2);
        o[15] = pass_colour[models.pass][3];
    }
    /* the CPU wrote these bytes: GX reads memory */
    DCFlushRange(out, need);
    GX_SetCullMode(two_sided ? GX_CULL_NONE : GX_CULL_BACK);
    GX_SetArray(GX_VA_POS, out, MODEL_VERTEX_BYTES);
    GX_SetArray(GX_VA_CLR0, out + 12, MODEL_VERTEX_BYTES);
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
    if (from->parts > into->parts_max)
        into->parts_max = from->parts;
    if (from->triangles > into->triangles_max)
        into->triangles_max = from->triangles;
    if (from->used_max > into->used_max)
        into->used_max = from->used_max;
}

unsigned long wii_gx_models_draw(void)
{
    static const u8 blend[NUMBER_OF_WII_GX_MODEL_PASSES] = {GX_BM_NONE, GX_BM_NONE, GX_BM_BLEND};
    static const u8 depth_write[NUMBER_OF_WII_GX_MODEL_PASSES] = {GX_TRUE, GX_FALSE, GX_FALSE};

    memset(&models.frame, 0, sizeof(models.frame));
    if (!models.enabled || !models.proc || !models.buffer || !tag_data_range(&models.tags_first, &models.tags_end))
        return 0;
    /* the last draw's parts may still be read from the buffer's start */
    fence_wait();
    models.used = 0;
    GX_InvVtxCache();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_SetAlphaUpdate(GX_FALSE);
    models.drawing = 1;
    for (int pass = 0; pass < NUMBER_OF_WII_GX_MODEL_PASSES; ++pass) {
        models.pass = pass;
        if (blend[pass] == GX_BM_BLEND)
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        else
            GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GX_SetZMode(GX_TRUE, GX_LEQUAL, depth_write[pass]);
        models.proc(pass);
    }
    models.drawing = 0;
    models.palette = NULL;
    models.node_count = 0;
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetCullMode(GX_CULL_BACK);
    if (models.frame.parts) {
        models.token = models.token >= DRAW_SYNC_LAST ? DRAW_SYNC_FIRST : (u16)(models.token + 1u);
        GX_SetDrawSync(models.token);
        models.fence_pending = 1;
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

void wii_gx_models_report(void)
{
    const struct model_stats *t = &models.total;
    wii_log("RENDER_MODEL_TOTALS enabled=%d skin_bytes=%lu draws=%lu models=%lu parts=%lu refused=%lu triangles=%lu "
            "vertices=%lu parts_max=%lu triangles_max=%lu skin_used_max=%lu wraps=%lu fence_waits=%lu "
            "refused_pointers=%lu refused_counts=%lu refused_vertex_type=%lu refused_index=%lu refused_capacity=%lu "
            "refused_vertex=%lu vertex_node_stride=%lu vertex_node_range=%lu vertex_weight_range=%lu "
            "vertex_position=%lu material=diagnostic_light_untextured\n", models.enabled, models.skin_bytes, t->draws,
            t->models, t->parts, t->refused, t->triangles, t->vertices, t->parts_max, t->triangles_max, t->used_max,
            t->wraps, t->fence_waits, t->refusals[REFUSE_POINTERS], t->refusals[REFUSE_COUNTS],
            t->refusals[REFUSE_VERTEX_TYPE], t->refusals[REFUSE_INDEX], t->refusals[REFUSE_CAPACITY],
            t->refusals[REFUSE_VERTEX], t->vertex_refusals[HWM_SKIN_NODE_STRIDE],
            t->vertex_refusals[HWM_SKIN_NODE_RANGE], t->vertex_refusals[HWM_SKIN_WEIGHT_RANGE],
            t->vertex_refusals[HWM_SKIN_POSITION]);
}
