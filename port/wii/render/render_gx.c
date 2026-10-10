/*
 * RENDER_GX.C
 *
 * The GX side of the Wii rasterizer's environment path (HWI-016B,
 * wii_render.h): the converted geometry and textures of the loaded BSP, GX
 * state, each frame's draw and copy, the fixed-pose EFB checks, timings and
 * memory. Strict C11 against libogc; no engine header.
 *
 * Resources, staged on the card by tools/wii/render_stage.py from the
 * content pipeline's outputs (private: never in Git):
 *   WII_ENGINE_DATA_ROOT/render/<map>/lightmaps/<bsp tag:05>.hwl
 *   WII_ENGINE_DATA_ROOT/render/<map>/textures/<bitmap tag:05>-<bitmap:03>.hwt
 *
 * Geometry: the HWL1 decoded form (hwl_lightmap.c) is reduced to what this
 * path draws, per material and 32-byte aligned: positions (F32 xyz), base
 * texcoords (F32 st) and lightmap texcoords (S16 st, the engine's
 * (2s + 1) / 65535 in the texture matrix), with the surfaces' u16 triangles
 * (indices local to the material), as GX indexed arrays. The decoded form and
 * the file are released after.
 *
 * Textures: every lightmap page the BSP's materials use, then the base maps
 * of its environment materials in order of the surfaces they cover, while
 * they fit the base map budget (wii_render.h); a base map left out is
 * reported with its bytes, and its materials draw lightmap-only, labelled.
 *
 * Materials: an environment material is the lightmap page times the base map
 * (two TEV stages: lightmap colour, then times the base map's). Every other
 * shader type is a labelled placeholder: transparent ones a 50% magenta
 * blend without depth writes, drawn after the opaque ones; others flat
 * yellow. Culling is GX_CULL_BACK with the stored corner order (qualified in
 * HWI-016A; checked again at every pose here).
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wii_platform.h"
#include "wii_render.h"
#include "content_common.h"
#include "hwl_lightmap.h"
#include "hwt_texture.h"

#define RENDER_TXT WII_ENGINE_ROOT "/render.txt"
#define RENDER_ROOT WII_ENGINE_DATA_ROOT "/render"
#define MAX_FILE_BYTES (64u * 1024u * 1024u)
#define MAX_MATERIALS 4096
#define MAX_TEXTURES 256
#define GRID_X 32
#define GRID_Y 24
#define RUN_GRID_X 16
#define RUN_GRID_Y 12

static const GXColor clear_colour = {18, 22, 30, 255};
static const GXColor placeholder_transparent = {255, 0, 255, 128};
static const GXColor placeholder_other = {255, 220, 0, 255};
static const GXColor flat_grey = {128, 128, 128, 255};
static const GXColor front_colour = {0, 255, 0, 255};
static const GXColor back_colour = {255, 0, 0, 255};

enum draw_mode { DRAW_FULL, DRAW_LIGHTMAP_ONLY, DRAW_BASE_ONLY };

struct texture {
    long tag;
    long bitmap;
    struct content_blob blob;
    struct hwt_header header;
    GXTexObj object;
    int resident;
    uint32_t bytes;
    long coverage;
};

struct material {
    struct wii_gx_material engine;
    uint32_t vertex_count, lightmap_vertex_count;
    uint32_t positions, texcoords, lightmap_texcoords; /* offsets into the arrays buffer */
    int lightmap_texture, base_texture;                /* indices into textures, or -1 */
};

struct frame_stats {
    unsigned long frames;
    unsigned long long visibility_us, submit_us, gp_us, copy_us, total_us;
    unsigned long visibility_max, submit_max, gp_max, copy_max, total_max;
    unsigned long long triangles, surfaces, clusters;
    unsigned long triangles_max, surfaces_max, clusters_max;
};

static struct {
    int configured, enabled, poses_only;
    unsigned long base_budget;
    GXRModeObj *mode;
    void *xfb[2];
    int fb;
    char map[64];
    /* the loaded BSP */
    int ready;
    unsigned long bsp_tag;
    long material_count, surface_count;
    struct material materials[MAX_MATERIALS];
    unsigned char *arrays;
    uint32_t arrays_bytes;
    struct hwl_surface *surfaces;
    uint32_t surfaces_bytes;
    struct hwl_lightmap *hwl_lightmaps;
    uint32_t hwl_lightmap_count;
    struct hwl_material *hwl_materials; /* kept while the materials are described */
    struct texture textures[MAX_TEXTURES];
    int texture_count;
    long lightmap_tag;
    long loads;
    /* this map's frame statistics, and the launch's */
    struct frame_stats map_stats, totals;
    unsigned long load_ms_max;
    /* memory this path owns (files, decoded and drawn forms, textures), now
    and at its peak; the heap's arena bounds show which bank it came from
    (mallinfo counts the gap between MEM1 and MEM2 once the heap crosses) */
    unsigned long owned, owned_peak;
    int placeholder_reported[NUMBER_OF_WII_GX_MATERIAL_KINDS];
} gx;

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

static void owned_add(unsigned long bytes)
{
    gx.owned += bytes;
    if (gx.owned > gx.owned_peak)
        gx.owned_peak = gx.owned;
}

static void owned_sub(unsigned long bytes)
{
    gx.owned -= bytes;
}

static unsigned long arena1_free(void)
{
    return (unsigned long)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());
}

static uint32_t align32(uint32_t value)
{
    return (value + 31u) & ~31u;
}

static unsigned long elapsed_us(u64 start)
{
    return (unsigned long)ticks_to_microsecs(gettime() - start);
}

static void stats_add(struct frame_stats *s, unsigned long visibility, unsigned long submit, unsigned long gp,
                      unsigned long copy, unsigned long triangles, long surfaces, long clusters)
{
    unsigned long total = visibility + submit + gp + copy;
    s->frames++;
    s->visibility_us += visibility;
    s->submit_us += submit;
    s->gp_us += gp;
    s->copy_us += copy;
    s->total_us += total;
    s->triangles += triangles;
    s->surfaces += (unsigned long)surfaces;
    s->clusters += (unsigned long)clusters;
#define MAXIMUM(field, value) if ((value) > s->field) s->field = (value)
    MAXIMUM(visibility_max, visibility);
    MAXIMUM(submit_max, submit);
    MAXIMUM(gp_max, gp);
    MAXIMUM(copy_max, copy);
    MAXIMUM(total_max, total);
    MAXIMUM(triangles_max, triangles);
    MAXIMUM(surfaces_max, (unsigned long)surfaces);
    MAXIMUM(clusters_max, (unsigned long)clusters);
#undef MAXIMUM
}

static void stats_log(const char *what, long load, const struct frame_stats *s)
{
    unsigned long n = s->frames ? s->frames : 1;
    wii_log("%s load=%ld frames=%lu visibility_us_mean=%lu visibility_us_max=%lu submit_us_mean=%lu submit_us_max=%lu "
            "gp_us_mean=%lu gp_us_max=%lu copy_us_mean=%lu copy_us_max=%lu total_us_mean=%lu total_us_max=%lu "
            "triangles_mean=%lu triangles_max=%lu surfaces_mean=%lu surfaces_max=%lu clusters_mean=%lu "
            "clusters_max=%lu\n",
            what, load, s->frames, (unsigned long)(s->visibility_us / n), s->visibility_max,
            (unsigned long)(s->submit_us / n), s->submit_max, (unsigned long)(s->gp_us / n), s->gp_max,
            (unsigned long)(s->copy_us / n), s->copy_max, (unsigned long)(s->total_us / n), s->total_max,
            (unsigned long)(s->triangles / n), s->triangles_max, (unsigned long)(s->surfaces / n), s->surfaces_max,
            (unsigned long)(s->clusters / n), s->clusters_max);
}

static void stats_merge(struct frame_stats *into, const struct frame_stats *from)
{
    into->frames += from->frames;
    into->visibility_us += from->visibility_us;
    into->submit_us += from->submit_us;
    into->gp_us += from->gp_us;
    into->copy_us += from->copy_us;
    into->total_us += from->total_us;
    into->triangles += from->triangles;
    into->surfaces += from->surfaces;
    into->clusters += from->clusters;
#define MERGE(field) if (from->field > into->field) into->field = from->field
    MERGE(visibility_max);
    MERGE(submit_max);
    MERGE(gp_max);
    MERGE(copy_max);
    MERGE(total_max);
    MERGE(triangles_max);
    MERGE(surfaces_max);
    MERGE(clusters_max);
#undef MERGE
}

/* ---------- configuration and GX state (driver) */

static void read_config(void)
{
    char text[512] = "";
    FILE *file = fopen(RENDER_TXT, "rb");
    gx.enabled = 1;
    gx.poses_only = 0;
    gx.base_budget = WII_RENDER_DEFAULT_BASE_BUDGET;
    if (!file)
        return;
    size_t used = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[used] = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strcmp(line, "render=off"))
            gx.enabled = 0;
        else if (!strcmp(line, "mode=poses_only"))
            gx.poses_only = 1;
        else if (!strncmp(line, "base_budget=", 12))
            gx.base_budget = strtoul(line + 12, NULL, 10);
    }
}

int wii_render_configure(const struct wii_render_video *video)
{
    memset(&gx, 0, sizeof(gx));
    read_config();
    gx.configured = 1;
    gx.mode = (GXRModeObj *)video->mode;
    gx.xfb[0] = video->xfb[0];
    gx.xfb[1] = video->xfb[1];
    gx.lightmap_tag = -1;
    if (gx.enabled) {
        GXRModeObj *mode = gx.mode;
        GX_SetCopyClear(clear_colour, GX_MAX_Z24);
        f32 y_scale = GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight);
        u32 lines = GX_SetDispCopyYScale(y_scale);
        GX_SetViewport(0, 0, mode->fbWidth, mode->efbHeight, 0, 1);
        GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
        GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
        GX_SetDispCopyDst(mode->fbWidth, lines);
        GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
        GX_SetFieldMode(mode->field_rendering, mode->viHeight == 2 * mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
        GX_SetPixelFmt(mode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
        GX_SetDispCopyGamma(GX_GM_1_0);
        GX_SetDither(GX_FALSE);
        /* format 0: indexed positions, base and lightmap texcoords; format 1:
        indexed positions and a direct colour (the facing classification) */
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_S16, 15);
        GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        /* the engine's lightmap texcoord (2s + 1) / 65535, from the S16 value
        s / 32768 */
        Mtx lightmap;
        memset(lightmap, 0, sizeof(lightmap));
        lightmap[0][0] = lightmap[1][1] = 65536.0f / 65535.0f;
        lightmap[0][3] = lightmap[1][3] = 1.0f / 65535.0f;
        GX_LoadTexMtxImm(lightmap, GX_TEXMTX0, GX_MTX2x4);
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GX_SetCullMode(GX_CULL_BACK);
        GX_CopyDisp(gx.xfb[gx.fb], GX_TRUE);
        GX_DrawDone();
    }
    wii_log("RENDER_CONFIG enabled=%d poses_only=%d base_budget=%lu efb=%ux%u xfb=%ux%u\n", gx.enabled,
            gx.poses_only, gx.base_budget, gx.mode->fbWidth, gx.mode->efbHeight, gx.mode->fbWidth,
            gx.mode->xfbHeight);
    return gx.enabled;
}

int wii_render_enabled(void)
{
    return gx.configured && gx.enabled;
}

int wii_render_poses_only(void)
{
    return wii_render_enabled() && gx.poses_only;
}

void wii_render_set_map(const char *scenario_path)
{
    const char *name = strrchr(scenario_path, '\\');
    name = name ? name + 1 : scenario_path;
    snprintf(gx.map, sizeof(gx.map), "%s", name);
}

void wii_render_report(void)
{
    if (!wii_render_enabled())
        return;
    stats_log("RENDER_TOTALS", gx.loads, &gx.totals);
    wii_log("RENDER_TOTALS_MEMORY loads=%ld load_ms_max=%lu owned_peak=%lu owned_now=%lu arena1_lo=%p arena1_free=%lu "
            "arena2_lo=%p\n", gx.loads, gx.load_ms_max, gx.owned_peak, gx.owned, SYS_GetArena1Lo(), arena1_free(),
            SYS_GetArena2Lo());
}

/* ---------- per map: geometry */

static void release_textures(void)
{
    for (int i = 0; i < gx.texture_count; ++i) {
        owned_sub(gx.textures[i].blob.capacity);
        content_blob_release(&gx.textures[i].blob);
    }
    memset(gx.textures, 0, sizeof(gx.textures));
    gx.texture_count = 0;
}

void wii_gx_map_end(void)
{
    if (gx.ready || gx.arrays || gx.surfaces) {
        stats_log("RENDER_MAP_STATS", gx.loads, &gx.map_stats);
        stats_merge(&gx.totals, &gx.map_stats);
    }
    release_textures();
    if (gx.arrays)
        owned_sub(gx.arrays_bytes + gx.surfaces_bytes);
    content_free_aligned(gx.arrays);
    content_free_aligned(gx.surfaces);
    free(gx.hwl_lightmaps);
    free(gx.hwl_materials);
    gx.arrays = NULL;
    gx.surfaces = NULL;
    gx.hwl_lightmaps = NULL;
    gx.hwl_materials = NULL;
    gx.arrays_bytes = gx.surfaces_bytes = 0;
    gx.hwl_lightmap_count = 0;
    gx.ready = 0;
    gx.material_count = gx.surface_count = 0;
    memset(&gx.map_stats, 0, sizeof(gx.map_stats));
}

int wii_gx_map_ready(void)
{
    return gx.ready;
}

static u64 load_started;
static uintptr_t load_arena1_before, load_arena2_before;

int wii_gx_map_begin(unsigned long bsp_tag_index, long material_count, long surface_count)
{
    char path[192];
    struct content_blob blob;
    struct hwl_geometry geometry;
    enum content_error error;

    wii_gx_map_end();
    gx.loads++;
    load_started = gettime();
    load_arena1_before = (uintptr_t)SYS_GetArena1Lo();
    load_arena2_before = (uintptr_t)SYS_GetArena2Lo();
    gx.bsp_tag = bsp_tag_index;
    snprintf(path, sizeof(path), RENDER_ROOT "/%s/lightmaps/%05lu.hwl", gx.map, bsp_tag_index);
    error = content_load_file(path, MAX_FILE_BYTES, &blob);
    if (error != CONTENT_OK) {
        wii_log("RENDER_FAIL geometry_file bsp_tag=%lu error=%s\n", bsp_tag_index, content_error_name(error));
        return 0;
    }
    owned_add(blob.capacity);
    error = hwl_load(blob.data, blob.bytes, &geometry);
    unsigned long file_bytes = blob.bytes, file_capacity = blob.capacity;
    if (error != CONTENT_OK) {
        owned_sub(file_capacity);
        content_blob_release(&blob);
        wii_log("RENDER_FAIL geometry_decode bsp_tag=%lu error=%s\n", bsp_tag_index, content_error_name(error));
        return 0;
    }
    unsigned long decoded = geometry.arena.bytes;
    owned_add(decoded);
    owned_sub(file_capacity);
    content_blob_release(&blob);
    if ((long)geometry.material_count != material_count || (long)geometry.surface_count != surface_count ||
        material_count > MAX_MATERIALS) {
        wii_log("RENDER_FAIL geometry_counts bsp_tag=%lu materials=%lu/%ld surfaces=%lu/%ld\n", bsp_tag_index,
                (unsigned long)geometry.material_count, material_count, (unsigned long)geometry.surface_count,
                surface_count);
        owned_sub(decoded);
        hwl_release(&geometry);
        return 0;
    }
    /* the arrays this path draws, per material, 32-byte aligned */
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < geometry.material_count; ++i) {
        const struct hwl_material *m = &geometry.materials[i];
        struct material *out = &gx.materials[i];
        memset(out, 0, sizeof(*out));
        out->vertex_count = m->vertex_count;
        out->lightmap_vertex_count = m->lightmap_vertex_count;
        out->positions = bytes;
        bytes += align32(m->vertex_count * 12u);
        out->texcoords = bytes;
        bytes += align32(m->vertex_count * 8u);
        out->lightmap_texcoords = bytes;
        bytes += align32(m->lightmap_vertex_count * 4u);
        out->lightmap_texture = out->base_texture = -1;
    }
    gx.arrays = content_alloc_aligned(bytes ? bytes : 32u);
    gx.surfaces_bytes = geometry.surface_count * (uint32_t)sizeof(struct hwl_surface);
    gx.surfaces = content_alloc_aligned(gx.surfaces_bytes ? gx.surfaces_bytes : 32u);
    gx.hwl_lightmaps = malloc(geometry.lightmap_count * sizeof(struct hwl_lightmap) + 1);
    gx.hwl_materials = malloc(geometry.material_count * sizeof(struct hwl_material) + 1);
    if (!gx.arrays || !gx.surfaces || !gx.hwl_lightmaps || !gx.hwl_materials) {
        wii_log("RENDER_FAIL geometry_memory bsp_tag=%lu bytes=%lu\n", bsp_tag_index, (unsigned long)bytes);
        owned_sub(decoded);
        hwl_release(&geometry);
        content_free_aligned(gx.arrays);
        content_free_aligned(gx.surfaces);
        gx.arrays = NULL;
        gx.surfaces = NULL;
        wii_gx_map_end();
        return 0;
    }
    gx.arrays_bytes = bytes;
    owned_add(gx.arrays_bytes + gx.surfaces_bytes);
    for (uint32_t i = 0; i < geometry.material_count; ++i) {
        const struct hwl_material *m = &geometry.materials[i];
        const struct material *out = &gx.materials[i];
        float *positions = (float *)(gx.arrays + out->positions);
        float *texcoords = (float *)(gx.arrays + out->texcoords);
        int16_t *lightmap = (int16_t *)(gx.arrays + out->lightmap_texcoords);
        for (uint32_t v = 0; v < m->vertex_count; ++v) {
            const struct hwl_vertex *source = &geometry.vertices[m->first_vertex + v];
            memcpy(positions + 3 * v, source->position, 12);
            memcpy(texcoords + 2 * v, source->texcoord, 8);
        }
        for (uint32_t v = 0; v < m->lightmap_vertex_count; ++v) {
            const struct hwl_lightmap_vertex *source = &geometry.lightmap_vertices[m->first_lightmap_vertex + v];
            lightmap[2 * v] = source->texcoord[0];
            lightmap[2 * v + 1] = source->texcoord[1];
        }
    }
    memcpy(gx.surfaces, geometry.surfaces, gx.surfaces_bytes);
    memcpy(gx.hwl_lightmaps, geometry.lightmaps, geometry.lightmap_count * sizeof(struct hwl_lightmap));
    memcpy(gx.hwl_materials, geometry.materials, geometry.material_count * sizeof(struct hwl_material));
    gx.hwl_lightmap_count = geometry.lightmap_count;
    gx.material_count = material_count;
    gx.surface_count = surface_count;
    owned_sub(decoded);
    hwl_release(&geometry);
    DCFlushRange(gx.arrays, gx.arrays_bytes);
    DCFlushRange(gx.surfaces, gx.surfaces_bytes);
    wii_log("RENDER_GEOMETRY bsp_tag=%lu file_bytes=%lu decoded_bytes=%lu materials=%ld surfaces=%ld arrays_bytes=%lu "
            "surfaces_bytes=%lu owned_peak=%lu\n",
            bsp_tag_index, file_bytes, decoded, material_count, surface_count, (unsigned long)gx.arrays_bytes,
            (unsigned long)gx.surfaces_bytes, gx.owned_peak);
    return 1;
}

int wii_gx_material_set(long index, const struct wii_gx_material *material)
{
    if (index < 0 || index >= gx.material_count || !gx.hwl_materials)
        return 0;
    const struct hwl_material *m = &gx.hwl_materials[index];
    long lightmap_page = -2;
    for (uint32_t i = 0; i < gx.hwl_lightmap_count; ++i)
        if ((uint32_t)index >= gx.hwl_lightmaps[i].first_material &&
            (uint32_t)index < gx.hwl_lightmaps[i].first_material + gx.hwl_lightmaps[i].material_count)
            lightmap_page = gx.hwl_lightmaps[i].bitmap_index;
    /* the converted BSP must be the loaded one: each material's surfaces and
    lightmap page as the engine's tags have them */
    if (m->first_surface != material->first_surface || m->surface_count != material->surface_count ||
        (material->lightmap_page >= 0 && lightmap_page != material->lightmap_page)) {
        wii_log("RENDER_FAIL material_mismatch index=%ld surfaces=%ld+%ld/%ld+%ld page=%ld/%ld\n", index,
                (long)m->first_surface, (long)m->surface_count, material->first_surface, material->surface_count,
                lightmap_page, material->lightmap_page);
        return 0;
    }
    gx.materials[index].engine = *material;
    if (material->kind != _wii_gx_material_environment && !gx.placeholder_reported[material->kind]) {
        gx.placeholder_reported[material->kind] = 1;
        wii_log("RENDER_PLACEHOLDER kind=%s first_material=%ld shader_type=%d (labelled: %s)\n",
                material->kind == _wii_gx_material_placeholder_transparent ? "transparent" : "other", index,
                material->shader_type,
                material->kind == _wii_gx_material_placeholder_transparent ? "50% magenta blend, no depth write"
                                                                            : "flat yellow");
    }
    return 1;
}

/* ---------- per map: textures */

static int texture_find(long tag, long bitmap)
{
    for (int i = 0; i < gx.texture_count; ++i)
        if (gx.textures[i].tag == tag && gx.textures[i].bitmap == bitmap)
            return i;
    return -1;
}

static void texture_path(char *path, size_t size, long tag, long bitmap)
{
    snprintf(path, size, RENDER_ROOT "/%s/textures/%05ld-%03ld.hwt", gx.map, tag, bitmap);
}

static int texture_load(struct texture *t, int lightmap, int width, int height)
{
    char path[192];
    struct hwt_image image;
    enum content_error error;

    texture_path(path, sizeof(path), t->tag, t->bitmap);
    error = content_load_file(path, MAX_FILE_BYTES, &t->blob);
    if (error == CONTENT_OK)
        error = hwt_parse(t->blob.data, t->blob.bytes, &t->header);
    if (error == CONTENT_OK && (t->header.type != HWT_TYPE_2D || t->header.faces != 1 || t->header.depth != 1))
        error = CONTENT_DIMENSIONS;
    if (error == CONTENT_OK && width > 0 && ((int)t->header.width != width || (int)t->header.height != height))
        error = CONTENT_DIMENSIONS;
    if (error == CONTENT_OK)
        error = hwt_image_at(&t->header, 0, &image);
    if (error == CONTENT_OK && (!image.gx_loadable || (image.offset & 31u)))
        error = CONTENT_DIMENSIONS;
    if (error != CONTENT_OK) {
        wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=%s error=%s\n", t->tag, t->bitmap,
                lightmap ? "lightmap" : "base", content_error_name(error));
        content_blob_release(&t->blob);
        return 0;
    }
    int levels = (int)t->header.levels;
    u8 wrap = lightmap ? GX_CLAMP : GX_REPEAT;
    GX_InitTexObj(&t->object, t->blob.data + image.offset, (u16)image.width, (u16)image.height,
                  (u8)t->header.gx_format, wrap, wrap, levels > 1 ? GX_TRUE : GX_FALSE);
    if (levels > 1)
        GX_InitTexObjLOD(&t->object, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(levels - 1), 0.0f, GX_FALSE, GX_FALSE,
                         GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(&t->object, GX_LINEAR, GX_LINEAR);
    DCFlushRange(t->blob.data, t->blob.capacity);
    t->bytes = t->blob.bytes;
    t->resident = 1;
    owned_add(t->blob.capacity);
    return 1;
}

static int coverage_order(const void *a, const void *b)
{
    const struct texture *x = a, *y = b;
    if (x->coverage != y->coverage)
        return x->coverage > y->coverage ? -1 : 1;
    if (x->tag != y->tag)
        return x->tag < y->tag ? -1 : 1;
    return x->bitmap < y->bitmap ? -1 : x->bitmap > y->bitmap;
}

int wii_gx_textures_load(long lightmap_tag_index)
{
    unsigned long lightmap_bytes = 0, base_bytes = 0, missing_bytes = 0;
    int lightmap_pages = 0, lightmap_failed = 0, base_total = 0, base_resident = 0, base_failed = 0;
    long kinds[NUMBER_OF_WII_GX_MATERIAL_KINDS] = {0}, kind_surfaces[NUMBER_OF_WII_GX_MATERIAL_KINDS] = {0};
    long detail = 0, bump = 0, no_base = 0, no_lightmap = 0;

    gx.lightmap_tag = lightmap_tag_index;
    release_textures();
    /* the base maps, by the surfaces they cover */
    for (long i = 0; i < gx.material_count; ++i) {
        const struct wii_gx_material *m = &gx.materials[i].engine;
        kinds[m->kind]++;
        kind_surfaces[m->kind] += m->surface_count;
        if (m->kind != _wii_gx_material_environment)
            continue;
        detail += m->detail_maps > 0;
        bump += m->bump_map;
        if (m->base_tag < 0) {
            no_base++;
            continue;
        }
        int t = texture_find(m->base_tag, m->base_bitmap);
        if (t < 0) {
            if (gx.texture_count == MAX_TEXTURES)
                return 0;
            t = gx.texture_count++;
            gx.textures[t].tag = m->base_tag;
            gx.textures[t].bitmap = m->base_bitmap;
        }
        gx.textures[t].coverage += m->surface_count;
    }
    qsort(gx.textures, (size_t)gx.texture_count, sizeof(gx.textures[0]), coverage_order);
    base_total = gx.texture_count;
    /* lightmap pages first (always resident) */
    for (long i = 0; i < gx.material_count; ++i) {
        const struct wii_gx_material *m = &gx.materials[i].engine;
        if (m->lightmap_page < 0 || lightmap_tag_index < 0 || gx.materials[i].lightmap_vertex_count == 0)
            continue;
        if (texture_find(lightmap_tag_index, m->lightmap_page) >= 0)
            continue;
        if (gx.texture_count == MAX_TEXTURES)
            return 0;
        struct texture *t = &gx.textures[gx.texture_count++];
        t->tag = lightmap_tag_index;
        t->bitmap = m->lightmap_page;
        t->coverage = -1;
        lightmap_pages++;
        if (texture_load(t, 1, 0, 0))
            lightmap_bytes += t->bytes;
        else
            lightmap_failed++;
    }
    /* then the base maps that fit the budget */
    for (int i = 0; i < base_total; ++i) {
        struct texture *t = &gx.textures[i];
        char path[192];
        struct stat information;
        int width = 0, height = 0;
        for (long k = 0; k < gx.material_count; ++k)
            if (gx.materials[k].engine.base_tag == t->tag && gx.materials[k].engine.base_bitmap == t->bitmap) {
                width = gx.materials[k].engine.base_width;
                height = gx.materials[k].engine.base_height;
                break;
            }
        texture_path(path, sizeof(path), t->tag, t->bitmap);
        if (stat(path, &information) != 0) {
            wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=base error=missing\n", t->tag, t->bitmap);
            base_failed++;
            continue;
        }
        unsigned long size = (unsigned long)information.st_size;
        if (base_bytes + size > gx.base_budget) {
            missing_bytes += size;
            wii_log("RENDER_BASE_NOT_RESIDENT tag=%ld bitmap=%ld bytes=%lu surfaces=%ld budget_left=%lu\n", t->tag,
                    t->bitmap, size, t->coverage, gx.base_budget - base_bytes);
            continue;
        }
        if (texture_load(t, 0, width, height)) {
            base_bytes += t->bytes;
            base_resident++;
        } else {
            base_failed++;
        }
    }
    /* each material's textures */
    for (long i = 0; i < gx.material_count; ++i) {
        struct material *m = &gx.materials[i];
        int t;
        m->lightmap_texture = m->base_texture = -1;
        if (m->engine.lightmap_page >= 0 && m->lightmap_vertex_count &&
            (t = texture_find(lightmap_tag_index, m->engine.lightmap_page)) >= 0 && gx.textures[t].resident)
            m->lightmap_texture = t;
        if (m->engine.kind == _wii_gx_material_environment && m->engine.base_tag >= 0 &&
            (t = texture_find(m->engine.base_tag, m->engine.base_bitmap)) >= 0 && gx.textures[t].resident)
            m->base_texture = t;
        if (m->engine.kind == _wii_gx_material_environment && m->lightmap_texture < 0)
            no_lightmap++;
    }
    GX_InvalidateTexAll();
    free(gx.hwl_materials);
    gx.hwl_materials = NULL;
    unsigned long load_ms = (unsigned long)ticks_to_millisecs(gettime() - load_started);
    if (load_ms > gx.load_ms_max)
        gx.load_ms_max = load_ms;
    wii_log("RENDER_MAP_LOAD load=%ld map=%s bsp_tag=%lu lightmap_tag=%ld geometry_bytes=%lu lightmap_pages=%d "
            "lightmap_failed=%d lightmap_bytes=%lu base_maps=%d base_resident=%d base_failed=%d base_bytes=%lu "
            "base_not_resident_bytes=%lu base_budget=%lu resident_total=%lu owned=%lu owned_peak=%lu "
            "arena1_lo=%p->%p arena1_free=%lu arena2_lo=%p->%p ms=%lu\n",
            gx.loads, gx.map, gx.bsp_tag, lightmap_tag_index,
            (unsigned long)(gx.arrays_bytes + gx.surfaces_bytes), lightmap_pages, lightmap_failed, lightmap_bytes,
            base_total, base_resident, base_failed, base_bytes, missing_bytes, gx.base_budget,
            (unsigned long)(gx.arrays_bytes + gx.surfaces_bytes) + lightmap_bytes + base_bytes, gx.owned,
            gx.owned_peak, (void *)load_arena1_before, SYS_GetArena1Lo(), arena1_free(), (void *)load_arena2_before,
            SYS_GetArena2Lo(), load_ms);
    wii_log("RENDER_MATERIALS load=%ld environment=%ld environment_surfaces=%ld transparent_placeholder=%ld "
            "transparent_surfaces=%ld other_placeholder=%ld other_surfaces=%ld detail_maps_not_drawn=%ld "
            "bump_maps_not_drawn=%ld environment_without_base=%ld environment_without_lightmap=%ld\n",
            gx.loads, kinds[0], kind_surfaces[0], kinds[1], kind_surfaces[1], kinds[2], kind_surfaces[2], detail, bump,
            no_base, no_lightmap);
    gx.ready = lightmap_failed == 0;
    return gx.ready;
}

/* ---------- draw */

static int surface_visible(const unsigned long *flags, long s)
{
    return (flags[s >> 5] >> (s & 31)) & 1u;
}

static void tev_constant(GXColor colour)
{
    GX_SetNumChans(0);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevKColor(GX_KCOLOR0, colour);
    GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/* the lightmap (TEXMAP1, TEXCOORD1) and/or the base map (TEXMAP0, TEXCOORD0) */
static void tev_textures(const struct texture *lightmap, const struct texture *base)
{
    int stage = 0;
    GX_SetNumChans(0);
    GX_SetNumTexGens(2);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_TEXMTX0);
    if (lightmap) {
        GX_LoadTexObj((GXTexObj *)&lightmap->object, GX_TEXMAP1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
        GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        stage++;
    }
    if (base) {
        u8 tev = stage ? GX_TEVSTAGE1 : GX_TEVSTAGE0;
        GX_LoadTexObj((GXTexObj *)&base->object, GX_TEXMAP0);
        GX_SetTevOrder(tev, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        /* a(1 - c) + b c with a = 0, b = the lightmap colour (or 1), c = the base */
        GX_SetTevColorIn(tev, GX_CC_ZERO, stage ? GX_CC_CPREV : GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GX_SetTevColorOp(tev, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(tev, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
        GX_SetTevKAlphaSel(tev, GX_TEV_KASEL_1);
        GX_SetTevAlphaOp(tev, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        stage++;
    }
    GX_SetNumTevStages((u8)stage);
}

static void set_arrays(const struct material *m, int lightmap)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX1, lightmap ? GX_INDEX16 : GX_NONE);
    GX_SetArray(GX_VA_POS, gx.arrays + m->positions, 12);
    GX_SetArray(GX_VA_TEX0, gx.arrays + m->texcoords, 8);
    if (lightmap)
        GX_SetArray(GX_VA_TEX1, gx.arrays + m->lightmap_texcoords, 4);
}

/* the visible surfaces of one material, as indexed triangles */
static unsigned long emit_material(const struct material *m, const unsigned long *flags, int lightmap)
{
    long first = m->engine.first_surface, end = first + m->engine.surface_count;
    unsigned long count = 0;
    for (long s = first; s < end; ++s)
        count += surface_visible(flags, s);
    if (!count)
        return 0;
    unsigned long left = count;
    long s = first;
    while (left) {
        unsigned long batch = left > 21000 ? 21000 : left;
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)(batch * 3));
        for (unsigned long emitted = 0; emitted < batch; ++s) {
            if (!surface_visible(flags, s))
                continue;
            const struct hwl_surface *surface = &gx.surfaces[s];
            for (int k = 0; k < 3; ++k) {
                u16 index = surface->vertex[k];
                GX_Position1x16(index);
                GX_TexCoord1x16(index);
                if (lightmap)
                    GX_TexCoord1x16(index);
            }
            emitted++;
        }
        GX_End();
        left -= batch;
    }
    return count;
}

static unsigned long draw_materials(const unsigned long *flags, const unsigned char *drawn, enum draw_mode mode,
                                    u8 cull)
{
    unsigned long triangles = 0;
    GX_SetCullMode(cull);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
        }
        for (long i = 0; i < gx.material_count; ++i) {
            const struct material *m = &gx.materials[i];
            int transparent = m->engine.kind == _wii_gx_material_placeholder_transparent;
            if (!drawn[i] || transparent != pass || !m->vertex_count)
                continue;
            int lightmap = 0;
            if (m->engine.kind == _wii_gx_material_environment) {
                const struct texture *l = m->lightmap_texture >= 0 && mode != DRAW_BASE_ONLY
                                              ? &gx.textures[m->lightmap_texture] : NULL;
                const struct texture *b = m->base_texture >= 0 && mode != DRAW_LIGHTMAP_ONLY
                                              ? &gx.textures[m->base_texture] : NULL;
                if (l || b)
                    tev_textures(l, b);
                else
                    tev_constant(flat_grey);
                lightmap = l != NULL;
            } else {
                tev_constant(transparent ? placeholder_transparent : placeholder_other);
            }
            set_arrays(m, lightmap);
            triangles += emit_material(m, flags, lightmap);
        }
    }
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    return triangles;
}

/* every visible triangle coloured by the engine's facing rule */
static void draw_classified(const struct wii_gx_view *view, const unsigned long *flags, const unsigned char *drawn,
                            wii_gx_frontfacing_proc frontfacing, u8 cull)
{
    GX_SetCullMode(cull);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    for (long i = 0; i < gx.material_count; ++i) {
        const struct material *m = &gx.materials[i];
        if (!drawn[i] || !m->vertex_count)
            continue;
        const float *positions = (const float *)(gx.arrays + m->positions);
        GX_SetArray(GX_VA_POS, gx.arrays + m->positions, 12);
        long first = m->engine.first_surface, end = first + m->engine.surface_count;
        for (long s = first; s < end; ++s) {
            if (!surface_visible(flags, s))
                continue;
            const struct hwl_surface *surface = &gx.surfaces[s];
            int front = frontfacing(view->eye, positions + 3 * surface->vertex[0], positions + 3 * surface->vertex[1],
                                    positions + 3 * surface->vertex[2]);
            GXColor c = front ? front_colour : back_colour;
            GX_Begin(GX_TRIANGLES, GX_VTXFMT1, 3);
            for (int k = 0; k < 3; ++k) {
                GX_Position1x16(surface->vertex[k]);
                GX_Color4u8(c.r, c.g, c.b, c.a);
            }
            GX_End();
        }
    }
}

static void load_view(const struct wii_gx_view *view)
{
    Mtx44 projection;
    Mtx position;
    memcpy(projection, view->projection, sizeof(projection));
    memcpy(position, view->world_to_view, sizeof(position));
    GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
    GX_LoadPosMtxImm(position, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

static void present(int vsyncs)
{
    GX_CopyDisp(gx.xfb[gx.fb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(gx.xfb[gx.fb]);
    VIDEO_Flush();
    for (int i = 0; i < vsyncs; ++i)
        VIDEO_WaitVSync();
    gx.fb ^= 1;
}

/* the EFB cleared into the back buffer (the next present overwrites it) */
static void clear_efb(void)
{
    GX_CopyDisp(gx.xfb[gx.fb], GX_TRUE);
    GX_DrawDone();
}

static uint32_t sample_at(int gx_index, int gy_index, int columns, int rows)
{
    GXColor c;
    GX_PeekARGB((u16)((gx_index * 2 + 1) * gx.mode->fbWidth / (columns * 2)),
                (u16)((gy_index * 2 + 1) * gx.mode->efbHeight / (rows * 2)), &c);
    return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
}

static int is_clear(uint32_t packed)
{
    return packed == ((uint32_t)clear_colour.r << 16 | (uint32_t)clear_colour.g << 8 | clear_colour.b);
}

static uint32_t samples_crc(const uint32_t *samples, int count, int *covered)
{
    uint32_t crc = 0;
    *covered = 0;
    for (int i = 0; i < count; ++i) {
        const unsigned char bytes[3] = {(unsigned char)(samples[i] >> 16), (unsigned char)(samples[i] >> 8),
                                        (unsigned char)samples[i]};
        crc = crc_update(crc, bytes, 3);
        *covered += !is_clear(samples[i]);
    }
    return crc;
}

static void sample_grid(uint32_t *samples, int columns, int rows)
{
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            samples[y * columns + x] = sample_at(x, y, columns, rows);
}

void wii_gx_frame(const struct wii_gx_view *view, const unsigned long *surface_flags,
                  const unsigned char *material_drawn, int sample, unsigned long *crc, int *covered)
{
    if (!gx.ready)
        return;
    u64 start = gettime();
    load_view(view);
    unsigned long triangles = draw_materials(surface_flags, material_drawn, DRAW_FULL, GX_CULL_BACK);
    unsigned long submit = elapsed_us(start);
    u64 gp_start = gettime();
    GX_DrawDone();
    unsigned long gp = elapsed_us(gp_start);
    if (sample) {
        uint32_t samples[RUN_GRID_X * RUN_GRID_Y];
        sample_grid(samples, RUN_GRID_X, RUN_GRID_Y);
        *crc = samples_crc(samples, RUN_GRID_X * RUN_GRID_Y, covered);
    }
    u64 copy_start = gettime();
    present(0);
    unsigned long copy = elapsed_us(copy_start);
    stats_add(&gx.map_stats, view->visibility_us, submit, gp, copy, triangles, view->visible_surfaces,
              view->rendered_clusters);
}

/* ---------- the pose checks */

static uint32_t pose_full[GRID_X * GRID_Y], pose_other[GRID_X * GRID_Y];

static int count_distinct(const uint32_t *samples, int count)
{
    static uint32_t sorted[GRID_X * GRID_Y];
    memcpy(sorted, samples, (size_t)count * sizeof(uint32_t));
    for (int i = 1; i < count; ++i) {
        uint32_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = v;
    }
    int distinct = count > 0;
    for (int i = 1; i < count; ++i)
        distinct += sorted[i] != sorted[i - 1];
    return distinct;
}

static int count_differs(const uint32_t *a, const uint32_t *b, int count)
{
    int n = 0;
    for (int i = 0; i < count; ++i)
        n += a[i] != b[i];
    return n;
}

void wii_gx_pose_capture(const struct wii_gx_view *view, const unsigned long *surface_flags,
                         const unsigned char *material_drawn, wii_gx_frontfacing_proc frontfacing,
                         int present_frames, struct wii_gx_pose_result *out)
{
    static const u8 culls[3] = {GX_CULL_NONE, GX_CULL_BACK, GX_CULL_FRONT};
    const int count = GRID_X * GRID_Y;
    int ignored;

    memset(out, 0, sizeof(*out));
    if (!gx.ready)
        return;
    out->samples = count;
    load_view(view);
    clear_efb();
    u64 start = gettime();
    draw_materials(surface_flags, material_drawn, DRAW_FULL, GX_CULL_BACK);
    GX_DrawDone();
    out->draw_us = elapsed_us(start);
    sample_grid(pose_full, GRID_X, GRID_Y);
    out->crc_full = samples_crc(pose_full, count, &out->covered);
    out->distinct = count_distinct(pose_full, count);
    clear_efb();
    draw_materials(surface_flags, material_drawn, DRAW_LIGHTMAP_ONLY, GX_CULL_BACK);
    GX_DrawDone();
    sample_grid(pose_other, GRID_X, GRID_Y);
    out->crc_lightmap_only = samples_crc(pose_other, count, &ignored);
    out->differs_lightmap_only = count_differs(pose_full, pose_other, count);
    clear_efb();
    draw_materials(surface_flags, material_drawn, DRAW_BASE_ONLY, GX_CULL_BACK);
    GX_DrawDone();
    sample_grid(pose_other, GRID_X, GRID_Y);
    out->crc_base_only = samples_crc(pose_other, count, &ignored);
    out->differs_base_only = count_differs(pose_full, pose_other, count);
    for (int k = 0; k < 3; ++k) {
        clear_efb();
        draw_classified(view, surface_flags, material_drawn, frontfacing, culls[k]);
        GX_DrawDone();
        sample_grid(pose_other, GRID_X, GRID_Y);
        for (int i = 0; i < count; ++i) {
            uint32_t c = pose_other[i];
            out->facing_front[k] += c == 0x00FF00u;
            out->facing_back[k] += c == 0xFF0000u;
        }
    }
    /* the full frame again, shown (frame dumps) */
    clear_efb();
    draw_materials(surface_flags, material_drawn, DRAW_FULL, GX_CULL_BACK);
    GX_DrawDone();
    present(present_frames);
    GX_SetCullMode(GX_CULL_BACK);
}
