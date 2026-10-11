/*
 * RENDER_GX.C
 *
 * The GX side of the Wii rasterizer's environment path (HWI-016B,
 * wii_render.h): the loaded BSP's geometry and textures, GX state, each
 * frame's draw and copy, the fixed-pose EFB checks, timings and memory.
 * Strict C11 against libogc; no engine header.
 *
 * Resources, staged on the card by tools/wii/render_stage.py from the
 * content pipeline's outputs (private: never in Git):
 *   WII_ENGINE_DATA_ROOT/render/<map>/lightmaps/<bsp tag:05>.hwl
 *   WII_ENGINE_DATA_ROOT/render/<map>/textures/<bitmap tag:05>-<bitmap:03>.hwt
 *
 * Geometry (HWI-030): GX draws from the tag slot. The staged map
 * (map_stage.py, HWIMAP02) holds each BSP material's compressed_vertex_data
 * in big-endian: vertex_count compressed environment vertices of 32 bytes
 * (position F32 xyz at 0, three packed normals, texcoord F32 st at 24), then
 * lightmap_vertex_count lightmap vertices of 8 bytes (a packed incident
 * radiosity direction, then the lightmap texcoord S16 st at 4: the engine's
 * (2s + 1) / 65535 is in the texture matrix); and the BSP's surfaces block,
 * three u16 vertex indices each, local to the surface's material. Those are
 * GX's indexed arrays (strides 32 and 8): nothing is decoded or copied, and
 * the BSP's geometry costs no memory beyond the tag slot. The HWL1 file holds
 * the same bytes, converted by the content pipeline from the same source
 * map; with geometry_check (the default) it is read in chunks at every BSP
 * load and compared byte for byte, and a BSP whose bytes differ is not drawn.
 * HWI-016B's path (the HWL1 file decoded whole, then reduced to 24 bytes a
 * vertex) needed the file and its decoded form at once: a10's first BSP
 * 8,100,736 bytes, its largest 11,664,352.
 *
 * Textures: one render pool a map (pool_allocate), taken from the heap at
 * the map's first BSP load and given back when the map unloads; each BSP
 * load refills it from the start: the material and texture tables, every
 * lightmap page the BSP's materials use, then the base maps of its
 * environment materials in order of the surfaces they cover, while they fit
 * the base map budget (wii_render.h) and the pool. A base map left out is
 * reported with its bytes and why, and its materials draw lightmap-only,
 * labelled.
 *
 * Textures are HWT1 version 1, or version 2 (gx-splitalpha-v1, HWI-008F):
 * CMPR colour with an alpha plane (I4 or I8) loaded as a second texture
 * object, or none when the alpha is 255 everywhere.
 *
 * Materials: an environment material is the lightmap page times the base map
 * (two TEV stages: lightmap colour, then times the base map's). Where the
 * shader reads the base map's alpha (environment types blended and blended
 * base specular), the TEV output alpha is the base map's: RGBA8's own, or a
 * split texture's plane in a third stage (TEXMAP2, the base texcoord); other
 * materials output alpha 1. The detail maps and the specular that consume it
 * are not drawn yet (HWI-032). Every other
 * shader type is a labelled placeholder: transparent ones a 50% magenta
 * blend without depth writes, drawn after the opaque ones; others flat
 * yellow. Culling is GX_CULL_BACK with the stored corner order (qualified in
 * HWI-016A; checked again at every pose here).
 */
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wii_platform.h"
#include "wii_render.h"
#include "cache_files_wii.h"
#include "content_common.h"
#include "hwl_lightmap.h"
#include "hwt_texture.h"

#define RENDER_TXT WII_ENGINE_ROOT "/render.txt"
#define RENDER_ROOT WII_ENGINE_DATA_ROOT "/render"
#define MAX_FILE_BYTES (64u * 1024u * 1024u)
#define MAX_MATERIALS 4096
#define GRID_X 32
#define GRID_Y 24
#define RUN_GRID_X 16
#define RUN_GRID_Y 12
#define MAX_POSE_TICKS WII_RENDER_MAXIMUM_POSE_TICKS
#define MAX_POSE_PHASES 4
/* the compressed environment vertex and the lightmap vertex, as staged */
#define VERTEX_BYTES 32u
#define VERTEX_TEXCOORD_OFFSET 24u
#define LIGHTMAP_VERTEX_BYTES 8u
#define LIGHTMAP_TEXCOORD_OFFSET 4u
#define ENVIRONMENT_COMPRESSED 1
#define LIGHTMAP_COMPRESSED 3
/* the MEM2 arena reserve below the engine's plan (wii_memory.c), left to
libogc and IOS buffers: the pool never takes it */
#define MEM2_RESERVE_BYTES 0x200000u
/* what the pool leaves the engine's heap in each bank by default: the
engine's own allocations after a map's start (stdio buffers, a BSP switch's
reads) grow the heap there */
#define POOL_KEEP_MEM1 65536u
#define POOL_KEEP_MEM2 262144u
/* the HWL1 comparison's chunk, taken from the pool's free end */
#define CHECK_CHUNK_BYTES 32768u

static const GXColor clear_colour = {18, 22, 30, 255};
static const GXColor placeholder_transparent = {255, 0, 255, 128};
static const GXColor placeholder_other = {255, 220, 0, 255};
static const GXColor flat_grey = {128, 128, 128, 255};
static const GXColor opaque_white = {255, 255, 255, 255};
static const GXColor front_colour = {0, 255, 0, 255};
static const GXColor back_colour = {255, 0, 0, 255};
static const long default_pose_ticks[3] = {30, 150, 300};

/* DRAW_BASE_ALPHA: each environment material's base map alpha, as TEV
combines it, shown as grey (255 where no base map is resident) */
enum draw_mode { DRAW_FULL, DRAW_LIGHTMAP_ONLY, DRAW_BASE_ONLY, DRAW_BASE_ALPHA };

struct texture {
    long tag;
    long bitmap;
    unsigned char *data;     /* in the render pool */
    uint32_t file_bytes;
    struct hwt_header header;
    GXTexObj object;
    GXTexObj alpha_object;   /* a version 2 texture's alpha plane */
    int alpha_plane;
    int resident;
    uint32_t bytes;
    long coverage;
};

struct material {
    struct wii_gx_material engine;
    uint32_t vertex_count, lightmap_vertex_count;
    const unsigned char *vertices;           /* the tag slot: stride 32 */
    const unsigned char *lightmap_vertices;  /* the tag slot: stride 8 */
    int lightmap_texture, base_texture;      /* indices into textures, or -1 */
};

struct frame_stats {
    unsigned long frames;
    unsigned long long visibility_us, submit_us, gp_us, copy_us, total_us;
    unsigned long visibility_max, submit_max, gp_max, copy_max, total_max;
    unsigned long long triangles, surfaces, clusters;
    unsigned long triangles_max, surfaces_max, clusters_max;
    /* HWI-030: visible triangles of environment materials with a base map,
    drawn textured or lightmap-only (the base map not resident) */
    unsigned long long textured, base_missing;
};

/* the render pool: two parts, MEM1's then MEM2's, each filled from its start */
struct pool_part {
    unsigned char *base;
    uint32_t bytes, used, peak;
};

/* the memory as a load leaves it */
struct memory_now {
    unsigned long mem1_free;             /* MEM1's arena above the heap */
    unsigned long mem2_free;             /* MEM2's arena above the heap, below the engine's plan */
    unsigned long mem2_above_reserve;    /* of which above the reserve */
    unsigned long heap_free;             /* free chunks inside the heap (mallinfo fordblks) */
    unsigned long heap_in_use;           /* mallinfo uordblks (counts the gap between the banks once crossed) */
    uintptr_t arena1_lo, arena2_lo;
};

static struct {
    int configured, enabled, poses_only;
    /* the boot mode (HWI-016D): the menus' texture cache shares the heap, so
    each texture is its own heap block, as before HWI-030, not a pool */
    int heap_textures;
    unsigned long base_budget;
    long base_limit;
    unsigned long pool_max, pool_keep[2];
    int geometry_check;
    long run_pose_ticks[MAX_POSE_TICKS];
    int run_pose_count;
    char pose_map[MAX_POSE_PHASES][64];
    struct wii_render_pose_plan pose_plans[MAX_POSE_PHASES];
    int pose_phases;
    GXRModeObj *mode;
    void *xfb[2];
    int fb;
    char map[64];
    /* the render pool, for the map loaded */
    struct pool_part pool[2];
    int pool_live;
    long pools;
    /* the loaded BSP */
    int ready;
    unsigned long bsp_tag;
    long material_count, surface_count;
    struct material *materials;  /* in the pool, material_count of them */
    const struct hwl_surface *surfaces;  /* the BSP's surfaces block, in the tag slot */
    struct texture *textures;    /* in the pool: at most a base map and a lightmap page a material */
    int texture_count, texture_capacity;
    long lightmap_tag;
    long loads;
    long vertex_data_unaligned32; /* materials whose vertex data does not start on 32 bytes */
    unsigned long vertex_data_bytes;
    /* this map's frame statistics, and the launch's */
    struct frame_stats map_stats, totals;
    unsigned long load_ms_max;
    /* memory this path owns (the pool), now and at its peak; and the least
    free memory any frame or load saw (the arena tails above the heap) */
    unsigned long owned, owned_peak;
    unsigned long mem1_free_min, mem2_above_reserve_min;
    /* geometry checks against HWL1 */
    long checks, checks_equal, checks_missing;
    int placeholder_reported[NUMBER_OF_WII_GX_MATERIAL_KINDS];
    /* the last full draw's textured and lightmap-only triangles (HWI-030) */
    unsigned long drawn_textured, drawn_base_missing;
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

/* the MEM2 arena's top while the engine's plan is open (its high bound), or
the arena's own */
static void memory_now(struct memory_now *m)
{
    struct mallinfo heap = mallinfo();
    uintptr_t lo2 = (uintptr_t)SYS_GetArena2Lo(), hi2 = (uintptr_t)SYS_GetArena2Hi();
    m->arena1_lo = (uintptr_t)SYS_GetArena1Lo();
    m->arena2_lo = lo2;
    m->mem1_free = arena1_free();
    m->mem2_free = hi2 > lo2 ? (unsigned long)(hi2 - lo2) : 0;
    m->mem2_above_reserve = m->mem2_free > MEM2_RESERVE_BYTES ? m->mem2_free - MEM2_RESERVE_BYTES : 0;
    m->heap_free = (unsigned long)heap.fordblks;
    m->heap_in_use = (unsigned long)heap.uordblks;
}

/* the least free memory seen: cheap enough for every frame */
static void memory_low_water(void)
{
    uintptr_t lo2 = (uintptr_t)SYS_GetArena2Lo(), hi2 = (uintptr_t)SYS_GetArena2Hi();
    unsigned long mem1 = arena1_free();
    unsigned long mem2 = hi2 > lo2 + MEM2_RESERVE_BYTES ? (unsigned long)(hi2 - lo2 - MEM2_RESERVE_BYTES) : 0;
    if (mem1 < gx.mem1_free_min)
        gx.mem1_free_min = mem1;
    if (mem2 < gx.mem2_above_reserve_min)
        gx.mem2_above_reserve_min = mem2;
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
    s->textured += gx.drawn_textured;
    s->base_missing += gx.drawn_base_missing;
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
            "clusters_max=%lu textured_triangles=%llu base_missing_triangles=%llu\n",
            what, load, s->frames, (unsigned long)(s->visibility_us / n), s->visibility_max,
            (unsigned long)(s->submit_us / n), s->submit_max, (unsigned long)(s->gp_us / n), s->gp_max,
            (unsigned long)(s->copy_us / n), s->copy_max, (unsigned long)(s->total_us / n), s->total_max,
            (unsigned long)(s->triangles / n), s->triangles_max, (unsigned long)(s->surfaces / n), s->surfaces_max,
            (unsigned long)(s->clusters / n), s->clusters_max, s->textured, s->base_missing);
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
    into->textured += from->textured;
    into->base_missing += from->base_missing;
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

/* "t,t,t" into at most max ticks; the count, or -1 */
static int parse_ticks(const char *text, long *ticks, int max)
{
    int count = 0;
    while (*text && count < max) {
        char *end;
        long value = strtol(text, &end, 10);
        if (end == text || value < 1)
            return -1;
        ticks[count++] = value;
        text = end;
        if (*text == ',')
            text++;
        else
            break;
    }
    return count;
}

/* ---------- configuration and GX state (driver) */

static void config_defaults(void)
{
    gx.enabled = 1;
    gx.poses_only = 0;
    gx.base_budget = WII_RENDER_DEFAULT_BASE_BUDGET;
    gx.base_limit = -1;
    gx.pool_max = 0xFFFFFFFFul;
    gx.pool_keep[0] = POOL_KEEP_MEM1;
    gx.pool_keep[1] = POOL_KEEP_MEM2;
    gx.geometry_check = 1;
    memcpy(gx.run_pose_ticks, default_pose_ticks, sizeof(default_pose_ticks));
    gx.run_pose_count = 3;
    gx.pose_phases = 0;
    gx.mem1_free_min = gx.mem2_above_reserve_min = 0xFFFFFFFFul;
}

static void read_config(void)
{
    char text[2048] = "";
    FILE *file = fopen(RENDER_TXT, "rb");
    config_defaults();
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
        else if (!strncmp(line, "base_limit=", 11))
            gx.base_limit = strtol(line + 11, NULL, 10);
        else if (!strncmp(line, "pool_max=", 9))
            gx.pool_max = strtoul(line + 9, NULL, 10);
        else if (!strncmp(line, "pool_keep=", 10)) {
            char *comma;
            gx.pool_keep[0] = strtoul(line + 10, &comma, 10);
            if (*comma == ',')
                gx.pool_keep[1] = strtoul(comma + 1, NULL, 10);
        } else if (!strncmp(line, "geometry_check=", 15))
            gx.geometry_check = (int)strtol(line + 15, NULL, 10);
        else if (!strncmp(line, "run_pose_ticks=", 15)) {
            int count = parse_ticks(line + 15, gx.run_pose_ticks, MAX_POSE_TICKS);
            gx.run_pose_count = count > 0 ? count : 0;
        } else if (!strncmp(line, "poses=", 6) && gx.pose_phases < MAX_POSE_PHASES) {
            int phase = gx.pose_phases++;
            struct wii_render_pose_plan *plan = &gx.pose_plans[phase];
            char *path = line + 6, *word = strchr(path, ' ');
            memset(plan, 0, sizeof(*plan));
            if (word)
                *word++ = 0;
            snprintf(gx.pose_map[phase], sizeof(gx.pose_map[phase]), "%s", path);
            plan->map = gx.pose_map[phase];
            plan->tick_count = -1;
            for (; word && *word; word = strchr(word, ' ') ? strchr(word, ' ') + 1 : NULL) {
                if (!strncmp(word, "ticks=", 6))
                    plan->tick_count = parse_ticks(word + 6, plan->ticks, MAX_POSE_TICKS);
                else if (!strncmp(word, "overrides=each", 14))
                    plan->overrides_each = 1;
                else if (!strncmp(word, "switch_bsp=", 11)) {
                    char *at = word + 11, *end;
                    while (plan->switch_count < WII_RENDER_MAXIMUM_POSE_SWITCHES) {
                        long tick = strtol(at, &end, 10);
                        if (end == at || *end != ':')
                            break;
                        plan->switch_ticks[plan->switch_count] = tick;
                        plan->switch_indices[plan->switch_count++] = (int)strtol(end + 1, &end, 10);
                        if (*end != ',')
                            break;
                        at = end + 1;
                    }
                }
            }
            if (plan->tick_count <= 0) {
                memcpy(plan->ticks, default_pose_ticks, sizeof(default_pose_ticks));
                plan->tick_count = 3;
            }
        }
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
    wii_log("RENDER_CONFIG enabled=%d poses_only=%d base_budget=%lu base_limit=%ld pool_max=%lu pool_keep=%lu,%lu "
            "geometry=tag_slot geometry_check=%d run_pose_ticks=%d pose_phases=%d efb=%ux%u xfb=%ux%u\n",
            gx.enabled, gx.poses_only, gx.base_budget, gx.base_limit, gx.pool_max, gx.pool_keep[0], gx.pool_keep[1],
            gx.geometry_check, gx.run_pose_count, gx.pose_phases, gx.mode->fbWidth, gx.mode->efbHeight,
            gx.mode->fbWidth, gx.mode->xfbHeight);
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

int wii_render_run_pose_ticks(long *ticks, int max)
{
    int count = gx.run_pose_count < max ? gx.run_pose_count : max;
    memcpy(ticks, gx.run_pose_ticks, (size_t)count * sizeof(long));
    return count;
}

int wii_render_pose_phase_count(void)
{
    return gx.pose_phases ? gx.pose_phases : 1;
}

void wii_render_pose_plan_get(int phase, struct wii_render_pose_plan *plan)
{
    if (gx.pose_phases && phase >= 0 && phase < gx.pose_phases) {
        *plan = gx.pose_plans[phase];
        return;
    }
    memset(plan, 0, sizeof(*plan));
    memcpy(plan->ticks, default_pose_ticks, sizeof(default_pose_ticks));
    plan->tick_count = 3;
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
    wii_log("RENDER_TOTALS_MEMORY loads=%ld pools=%ld load_ms_max=%lu owned_peak=%lu owned_now=%lu arena1_lo=%p "
            "arena1_free=%lu arena2_lo=%p mem1_free_min=%lu mem2_above_reserve_min=%lu geometry_checks=%ld "
            "geometry_equal=%ld geometry_missing=%ld\n", gx.loads, gx.pools, gx.load_ms_max, gx.owned_peak, gx.owned,
            SYS_GetArena1Lo(), arena1_free(), SYS_GetArena2Lo(), gx.mem1_free_min, gx.mem2_above_reserve_min,
            gx.checks, gx.checks_equal, gx.checks_missing);
}

/* ---------- the render pool */

static int in_mem1(const void *pointer)
{
    uintptr_t p = (uintptr_t)pointer;
    return p >= 0x80000000u && p < 0x81800000u;
}

/* the map's pool: MEM1's arena above its keep, then MEM2's above the
reserve and its keep, at most pool_max together. Each part is one heap
block (32-byte aligned); 1 if any was taken */
static int pool_allocate(void)
{
    struct memory_now before, after;
    uint32_t want[2] = {0, 0};
    unsigned long left = gx.pool_max;

    if (gx.pool_live)
        return 1;
    memory_now(&before);
    if (before.mem1_free > gx.pool_keep[0] + 8192u)
        want[0] = (uint32_t)((before.mem1_free - gx.pool_keep[0] - 8192u) & ~4095ul);
    if (want[0] > left)
        want[0] = (uint32_t)(left & ~31ul);
    if (want[0]) {
        gx.pool[0].base = content_alloc_aligned(want[0]);
        /* only MEM1's: a block the heap had to take from MEM2 is not this part */
        if (gx.pool[0].base && !in_mem1(gx.pool[0].base)) {
            content_free_aligned(gx.pool[0].base);
            gx.pool[0].base = NULL;
        }
        gx.pool[0].bytes = gx.pool[0].base ? want[0] : 0;
        left -= gx.pool[0].bytes;
    }
    {
        struct memory_now middle;
        memory_now(&middle);
        if (middle.mem2_above_reserve > gx.pool_keep[1] + 8192u)
            want[1] = (uint32_t)((middle.mem2_above_reserve - gx.pool_keep[1] - 8192u) & ~4095ul);
    }
    if (want[1] > left)
        want[1] = (uint32_t)(left & ~31ul);
    if (want[1]) {
        gx.pool[1].base = content_alloc_aligned(want[1]);
        gx.pool[1].bytes = gx.pool[1].base ? want[1] : 0;
    }
    memory_now(&after);
    gx.pool_live = gx.pool[0].base != NULL || gx.pool[1].base != NULL;
    gx.pools += gx.pool_live;
    owned_add(gx.pool[0].bytes + gx.pool[1].bytes);
    wii_log("RENDER_POOL map=%s mem1=%p+%lu mem2=%p+%lu total=%lu keep=%lu,%lu max=%lu "
            "before_mem1_free=%lu before_mem2_above_reserve=%lu before_heap_free=%lu "
            "after_mem1_free=%lu after_mem2_above_reserve=%lu after_heap_free=%lu arena1_lo=%p arena2_lo=%p ok=%d\n",
            gx.map, (void *)gx.pool[0].base, (unsigned long)gx.pool[0].bytes, (void *)gx.pool[1].base,
            (unsigned long)gx.pool[1].bytes, (unsigned long)(gx.pool[0].bytes + gx.pool[1].bytes), gx.pool_keep[0],
            gx.pool_keep[1], gx.pool_max, before.mem1_free, before.mem2_above_reserve, before.heap_free,
            after.mem1_free, after.mem2_above_reserve, after.heap_free, (void *)after.arena1_lo,
            (void *)after.arena2_lo, gx.pool_live);
    return gx.pool_live;
}

static void pool_reset(void)
{
    for (int i = 0; i < 2; ++i)
        gx.pool[i].used = 0;
}

static uint32_t pool_used(void)
{
    return gx.pool[0].used + gx.pool[1].used;
}

static uint32_t pool_bytes(void)
{
    return gx.pool[0].bytes + gx.pool[1].bytes;
}

/* the first part with room: bytes, 32-byte aligned, or NULL */
static void *pool_take(uint32_t bytes)
{
    bytes = align32(bytes ? bytes : 32u);
    for (int i = 0; i < 2; ++i) {
        struct pool_part *p = &gx.pool[i];
        if (p->base && p->bytes - p->used >= bytes) {
            void *out = p->base + p->used;
            p->used += bytes;
            if (p->used > p->peak)
                p->peak = p->used;
            return out;
        }
    }
    return NULL;
}

/* would bytes fit (without taking them)? */
static int pool_fits(uint32_t bytes)
{
    bytes = align32(bytes ? bytes : 32u);
    for (int i = 0; i < 2; ++i)
        if (gx.pool[i].base && gx.pool[i].bytes - gx.pool[i].used >= bytes)
            return 1;
    return 0;
}

void wii_gx_pool_release(void)
{
    if (!gx.pool_live)
        return;
    wii_log("RENDER_POOL_RELEASE map=%s mem1_peak=%lu/%lu mem2_peak=%lu/%lu\n", gx.map,
            (unsigned long)gx.pool[0].peak, (unsigned long)gx.pool[0].bytes, (unsigned long)gx.pool[1].peak,
            (unsigned long)gx.pool[1].bytes);
    owned_sub(gx.pool[0].bytes + gx.pool[1].bytes);
    content_free_aligned(gx.pool[1].base);
    content_free_aligned(gx.pool[0].base);
    memset(gx.pool, 0, sizeof(gx.pool));
    gx.pool_live = 0;
}

void wii_gx_heap_textures(int on)
{
    wii_gx_map_end();
    wii_gx_pool_release();
    gx.heap_textures = on;
}

/* a BSP's memory: the map's pool, or (the boot mode) its own heap block */
static void *store_take(uint32_t bytes)
{
    if (!gx.heap_textures)
        return pool_take(bytes);
    void *p = content_alloc_aligned(align32(bytes ? bytes : 32u));
    if (p)
        owned_add(align32(bytes ? bytes : 32u));
    return p;
}

/* given back: a heap block is freed; the pool is refilled from its start at
the next BSP load (pool_reset) */
static void store_give(void *p, uint32_t bytes)
{
    if (!gx.heap_textures || !p)
        return;
    owned_sub(align32(bytes ? bytes : 32u));
    content_free_aligned(p);
}

static int store_fits(uint32_t bytes)
{
    return gx.heap_textures ? 1 : pool_fits(bytes);
}

/* ---------- per BSP: geometry */

static void release_textures(void)
{
    if (gx.textures) {
        for (int i = 0; i < gx.texture_count; ++i)
            store_give(gx.textures[i].data, gx.textures[i].file_bytes);
        memset(gx.textures, 0, (size_t)gx.texture_capacity * sizeof(*gx.textures));
    }
    gx.texture_count = 0;
}

void wii_gx_map_end(void)
{
    if (gx.ready || gx.materials) {
        stats_log("RENDER_MAP_STATS", gx.loads, &gx.map_stats);
        stats_merge(&gx.totals, &gx.map_stats);
    }
    release_textures();
    if (gx.materials)
        store_give(gx.materials, (uint32_t)((size_t)gx.material_count + 1) * (uint32_t)sizeof(*gx.materials));
    if (gx.textures)
        store_give(gx.textures, (uint32_t)gx.texture_capacity * (uint32_t)sizeof(*gx.textures));
    gx.materials = NULL;
    gx.textures = NULL;
    gx.surfaces = NULL;
    gx.texture_capacity = 0;
    gx.ready = 0;
    gx.material_count = gx.surface_count = 0;
    gx.vertex_data_unaligned32 = 0;
    gx.vertex_data_bytes = 0;
    pool_reset();
    memset(&gx.map_stats, 0, sizeof(gx.map_stats));
}

int wii_gx_map_ready(void)
{
    return gx.ready;
}

static u64 load_started;
static uintptr_t load_arena1_before, load_arena2_before;

/* the BSP's range of the tag slot (the structure BSP read last) */
static int tag_slot_bsp_range(uintptr_t *first, uintptr_t *end)
{
    struct wii_cache_load_report load;
    size_t slot_bytes = 0;
    uintptr_t slot = (uintptr_t)wii_arena_region(_wii_arena_tag_cache, &slot_bytes);
    wii_cache_load_report_get(&load);
    if (!slot || load.range_count < 2 || load.range_offset[1] + load.range_size[1] > slot_bytes)
        return 0;
    *first = slot + load.range_offset[1];
    *end = *first + load.range_size[1];
    return 1;
}

int wii_gx_map_begin(unsigned long bsp_tag_index, long material_count, long surface_count, const void *surfaces)
{
    uintptr_t first, end;

    char path[192];
    struct stat information;

    wii_gx_map_end();
    gx.loads++;
    load_started = gettime();
    load_arena1_before = (uintptr_t)SYS_GetArena1Lo();
    load_arena2_before = (uintptr_t)SYS_GetArena2Lo();
    gx.bsp_tag = bsp_tag_index;
    if (material_count < 0 || material_count > MAX_MATERIALS || surface_count < 0 ||
        (surface_count && !surfaces) || ((uintptr_t)surfaces & 1u)) {
        wii_log("RENDER_FAIL geometry_counts bsp_tag=%lu materials=%ld surfaces=%ld\n", bsp_tag_index, material_count,
                surface_count);
        return 0;
    }
    /* the surfaces block lies in the BSP's range of the tag slot */
    if (surface_count && (!tag_slot_bsp_range(&first, &end) || (uintptr_t)surfaces < first ||
                          (uintptr_t)surfaces + 6u * (uintptr_t)surface_count > end)) {
        wii_log("RENDER_FAIL geometry_surfaces bsp_tag=%lu surfaces=%ld (not in the loaded BSP's range)\n",
                bsp_tag_index, surface_count);
        return 0;
    }
    /* a BSP whose converted resources are not staged is not drawn (its HWL1
    file marks them, as in HWI-016B), and takes no memory */
    snprintf(path, sizeof(path), RENDER_ROOT "/%s/lightmaps/%05lu.hwl", gx.map, bsp_tag_index);
    if (stat(path, &information) != 0) {
        wii_log("RENDER_FAIL geometry_file bsp_tag=%lu error=io (not staged: not drawn)\n", bsp_tag_index);
        return 0;
    }
    if (!gx.heap_textures && !pool_allocate()) {
        wii_log("RENDER_FAIL pool bsp_tag=%lu\n", bsp_tag_index);
        return 0;
    }
    gx.material_count = material_count;
    gx.texture_capacity = 2 * (int)material_count + 1;
    gx.materials = store_take((uint32_t)((size_t)material_count + 1) * (uint32_t)sizeof(*gx.materials));
    gx.textures = store_take((uint32_t)gx.texture_capacity * (uint32_t)sizeof(*gx.textures));
    if (!gx.materials || !gx.textures) {
        wii_log("RENDER_FAIL material_memory bsp_tag=%lu materials=%ld pool=%lu\n", bsp_tag_index, material_count,
                (unsigned long)pool_bytes());
        wii_gx_map_end();
        return 0;
    }
    memset(gx.materials, 0, ((size_t)material_count + 1) * sizeof(*gx.materials));
    memset(gx.textures, 0, (size_t)gx.texture_capacity * sizeof(*gx.textures));
    for (long i = 0; i < material_count; ++i)
        gx.materials[i].lightmap_texture = gx.materials[i].base_texture = -1;
    gx.material_count = material_count;
    gx.surface_count = surface_count;
    gx.surfaces = surfaces;
    /* the tag slot's bytes changed under the same addresses: GX's vertex
    cache must not keep the last BSP's */
    GX_InvVtxCache();
    return 1;
}

int wii_gx_material_set(long index, const struct wii_gx_material *material)
{
    uintptr_t first, end;
    if (index < 0 || index >= gx.material_count || !gx.materials)
        return 0;
    struct material *out = &gx.materials[index];
    const unsigned char *data = material->vertex_data;
    long vertices = material->vertex_count, lightmap = material->lightmap_vertex_count;
    /* what the converter (content_geometry.py) and hwl_load require of a
    material, checked on the tag slot's own: compressed vertex types, counts
    in bounds, the data's size from its counts, no uncompressed vertices,
    surfaces inside the block and every index below the vertex count */
    int ok = material->vertex_type == ENVIRONMENT_COMPRESSED &&
             material->lightmap_vertex_type == (lightmap ? LIGHTMAP_COMPRESSED : material->lightmap_vertex_type) &&
             vertices >= 0 && lightmap >= 0 && vertices <= (long)HWL_MAX_MATERIAL_VERTICES && lightmap <= vertices &&
             material->uncompressed_bytes == 0 &&
             material->vertex_data_bytes == vertices * (long)VERTEX_BYTES + lightmap * (long)LIGHTMAP_VERTEX_BYTES &&
             material->first_surface >= 0 && material->surface_count >= 0 &&
             material->first_surface + material->surface_count <= gx.surface_count;
    if (ok && vertices) {
        ok = data && !((uintptr_t)data & 3u) && tag_slot_bsp_range(&first, &end) && (uintptr_t)data >= first &&
             (uintptr_t)data + (uintptr_t)material->vertex_data_bytes <= end;
    }
    for (long s = 0; ok && s < material->surface_count; ++s)
        for (int k = 0; k < 3; ++k)
            if (gx.surfaces[material->first_surface + s].vertex[k] >= (unsigned long)vertices)
                ok = 0;
    if (!ok) {
        wii_log("RENDER_FAIL material_geometry index=%ld vertex_type=%d/%d vertices=%ld/%ld bytes=%ld uncompressed=%ld "
                "surfaces=%ld+%ld/%ld\n", index, material->vertex_type, material->lightmap_vertex_type, vertices,
                lightmap, material->vertex_data_bytes, material->uncompressed_bytes, material->first_surface,
                material->surface_count, gx.surface_count);
        return 0;
    }
    out->engine = *material;
    out->vertex_count = (uint32_t)vertices;
    out->lightmap_vertex_count = (uint32_t)lightmap;
    out->vertices = data;
    out->lightmap_vertices = data ? data + VERTEX_BYTES * (uint32_t)vertices : NULL;
    out->lightmap_texture = out->base_texture = -1;
    if (vertices) {
        /* the CPU loaded these bytes into the tag slot: GX reads memory */
        DCStoreRange((void *)data, (u32)material->vertex_data_bytes);
        gx.vertex_data_unaligned32 += ((uintptr_t)data & 31u) != 0;
        gx.vertex_data_bytes += (unsigned long)material->vertex_data_bytes;
    }
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

/* ---------- per BSP: the HWL1 comparison, in chunks */

struct hwl_stream {
    FILE *file;
    unsigned char *chunk;
    uint32_t chunk_bytes;
    unsigned long compared, differ;
};

/* bytes of the file at offset against count memory segments (pointer, size)
in order: the differing bytes are counted */
static int stream_compare(struct hwl_stream *s, uint32_t offset, uint32_t bytes, int kind)
{
    long material = 0;
    uint32_t inside = 0;
    if (fseek(s->file, (long)offset, SEEK_SET) != 0)
        return 0;
    while (bytes) {
        uint32_t take = bytes < s->chunk_bytes ? bytes : s->chunk_bytes;
        if (fread(s->chunk, 1, take, s->file) != take)
            return 0;
        uint32_t at = 0;
        while (at < take) {
            const unsigned char *memory;
            uint32_t size;
            if (kind == 2) {
                memory = (const unsigned char *)gx.surfaces;
                size = (uint32_t)gx.surface_count * 6u;
            } else {
                if (material >= gx.material_count)
                    return 0;
                const struct material *m = &gx.materials[material];
                memory = kind == 0 ? m->vertices : m->lightmap_vertices;
                size = kind == 0 ? m->vertex_count * VERTEX_BYTES : m->lightmap_vertex_count * LIGHTMAP_VERTEX_BYTES;
            }
            if (inside >= size) {
                if (kind == 2)
                    return 0;
                material++;
                inside = 0;
                continue;
            }
            uint32_t n = size - inside < take - at ? size - inside : take - at;
            for (uint32_t i = 0; i < n; ++i)
                s->differ += memory[inside + i] != s->chunk[at + i];
            s->compared += n;
            inside += n;
            at += n;
        }
        bytes -= take;
    }
    return 1;
}

/* the BSP's HWL1 file (the content generation's) against what is drawn: its
records against the engine's materials, its vertex, lightmap vertex and
surface sections byte for byte against the tag slot's. 1 if equal or not
checked; 0 if they differ or the file is malformed */
static int geometry_check(void)
{
    static const uint32_t record_bytes[6] = {16, 12, 188, 32, 8, 6};
    char path[192];
    unsigned char head[CONTENT_SECTIONS_HEADER_BYTES + 6 * CONTENT_SECTIONS_ENTRY_BYTES];
    uint32_t count[6], offset[6];
    struct hwl_stream s;
    struct stat information;
    unsigned long materials_differ = 0;
    int ok = 1;
    u64 started = gettime();

    snprintf(path, sizeof(path), RENDER_ROOT "/%s/lightmaps/%05lu.hwl", gx.map, gx.bsp_tag);
    memset(&s, 0, sizeof(s));
    if (stat(path, &information) != 0 || !(s.file = fopen(path, "rb"))) {
        gx.checks_missing++;
        wii_log("RENDER_GEOMETRY_CHECK bsp_tag=%lu result=unreadable\n", gx.bsp_tag);
        return 0;
    }
    gx.checks++;
    /* the chunk: the pool's free end, given back after */
    uint32_t mark[2] = {gx.pool[0].used, gx.pool[1].used};
    s.chunk_bytes = CHECK_CHUNK_BYTES;
    s.chunk = store_take(s.chunk_bytes);
    if (!s.chunk || fread(head, 1, sizeof(head), s.file) != sizeof(head) || memcmp(head, "HWL1", 4) ||
        content_be16(head + 4) != 1 || content_be16(head + 6) != CONTENT_SECTIONS_HEADER_BYTES ||
        content_be16(head + 8) != 6 || content_be16(head + 10) != 0 ||
        content_be32(head + 12) != (uint32_t)information.st_size)
        ok = 0;
    for (int i = 0; ok && i < 6; ++i) {
        const unsigned char *e = head + CONTENT_SECTIONS_HEADER_BYTES + CONTENT_SECTIONS_ENTRY_BYTES * i;
        count[i] = content_be32(e + 4);
        offset[i] = content_be32(e + 8);
        if (content_be16(e) != (uint32_t)i + 1 || content_be16(e + 2) != record_bytes[i] ||
            content_be32(e + 12) != count[i] * record_bytes[i] ||
            (uint64_t)offset[i] + content_be32(e + 12) > (uint64_t)information.st_size)
            ok = 0;
    }
    /* the BSP record, the lightmaps and the materials, record by record */
    if (ok && (count[0] != 1 || count[2] != (uint32_t)gx.material_count))
        ok = 0;
    if (ok) {
        unsigned char r[16];
        ok = fseek(s.file, (long)offset[0], SEEK_SET) == 0 && fread(r, 1, 16, s.file) == 16 &&
             content_be32(r + 4) == count[1] && content_be32(r + 8) == count[2] &&
             content_be32(r + 12) == (uint32_t)gx.surface_count && count[5] == (uint32_t)gx.surface_count;
    }
    if (ok && fseek(s.file, (long)offset[1], SEEK_SET) == 0) {
        uint32_t next = 0;
        for (uint32_t i = 0; ok && i < count[1]; ++i) {
            unsigned char r[12];
            if (fread(r, 1, 12, s.file) != 12) {
                ok = 0;
                break;
            }
            long page = content_s16(r);
            uint32_t first = content_be32(r + 4), n = content_be32(r + 8);
            if (first != next || (uint64_t)first + n > (uint64_t)gx.material_count)
                ok = 0;
            for (uint32_t k = first; ok && k < first + n; ++k)
                materials_differ += gx.materials[k].engine.lightmap_page >= 0 &&
                                    gx.materials[k].engine.lightmap_page != page;
            next = first + n;
        }
        ok = ok && next == (uint32_t)gx.material_count;
    } else {
        ok = 0;
    }
    if (ok && fseek(s.file, (long)offset[2], SEEK_SET) == 0) {
        uint32_t vertices = 0, lightmap = 0;
        for (long i = 0; ok && i < gx.material_count; ++i) {
            unsigned char r[188];
            const struct material *m = &gx.materials[i];
            if (fread(r, 1, 188, s.file) != 188) {
                ok = 0;
                break;
            }
            materials_differ += (long)(int32_t)content_be32(r + 12) != m->engine.first_surface ||
                                (long)(int32_t)content_be32(r + 16) != m->engine.surface_count ||
                                content_s16(r + 168) != m->engine.vertex_type ||
                                content_be32(r + 172) != m->vertex_count || content_be32(r + 176) != vertices ||
                                content_be32(r + 180) != m->lightmap_vertex_count ||
                                content_be32(r + 184) != lightmap;
            vertices += m->vertex_count;
            lightmap += m->lightmap_vertex_count;
        }
        ok = ok && vertices == count[3] && lightmap == count[4];
    } else {
        ok = 0;
    }
    /* the vertices, the lightmap vertices and the surfaces, byte for byte */
    unsigned long vertex_differ = 0, lightmap_differ = 0, surface_differ = 0;
    if (ok) {
        ok = stream_compare(&s, offset[3], count[3] * 32u, 0);
        vertex_differ = s.differ;
    }
    if (ok) {
        ok = stream_compare(&s, offset[4], count[4] * 8u, 1);
        lightmap_differ = s.differ - vertex_differ;
    }
    if (ok) {
        ok = stream_compare(&s, offset[5], count[5] * 6u, 2);
        surface_differ = s.differ - vertex_differ - lightmap_differ;
    }
    fclose(s.file);
    store_give(s.chunk, s.chunk_bytes);
    gx.pool[0].used = mark[0];
    gx.pool[1].used = mark[1];
    int equal = ok && materials_differ == 0 && s.differ == 0;
    gx.checks_equal += equal;
    wii_log("RENDER_GEOMETRY_CHECK bsp_tag=%lu file_bytes=%lu materials=%ld vertices=%lu lightmap_vertices=%lu "
            "surfaces=%ld bytes_compared=%lu materials_differ=%lu vertex_bytes_differ=%lu lightmap_bytes_differ=%lu "
            "surface_bytes_differ=%lu well_formed=%d equal=%d chunk=%lu ms=%lu\n",
            gx.bsp_tag, (unsigned long)information.st_size, gx.material_count, ok ? (unsigned long)count[3] : 0ul,
            ok ? (unsigned long)count[4] : 0ul, gx.surface_count, s.compared, materials_differ, vertex_differ,
            lightmap_differ, surface_differ, ok, equal, (unsigned long)s.chunk_bytes,
            (unsigned long)ticks_to_millisecs(gettime() - started));
    return equal;
}

/* ---------- per BSP: textures */

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

/* the file whole into the pool (its size as stat gave it); CONTENT_MEMORY
if the pool has no room */
static enum content_error texture_read(const char *path, uint32_t size, unsigned char **out)
{
    *out = NULL;
    if (size == 0 || size > MAX_FILE_BYTES)
        return CONTENT_SIZE;
    if (!store_fits(size))
        return CONTENT_MEMORY;
    FILE *file = fopen(path, "rb");
    if (!file)
        return CONTENT_IO;
    unsigned char *data = store_take(size);
    unsigned char extra;
    if (!data) {
        fclose(file);
        return CONTENT_MEMORY;
    }
    enum content_error error = CONTENT_OK;
    memset(data + size, 0, align32(size) - size);
    if (fread(data, 1, size, file) != size || fread(&extra, 1, 1, file) != 0)
        error = CONTENT_IO;
    if (fclose(file) != 0 && error == CONTENT_OK)
        error = CONTENT_IO;
    if (error == CONTENT_OK)
        *out = data;
    else
        store_give(data, size);
    return error;
}

static int texture_load(struct texture *t, int lightmap, int width, int height, uint32_t size)
{
    char path[192];
    struct hwt_image image;
    enum content_error error;
    uint32_t mark[2] = {gx.pool[0].used, gx.pool[1].used};

    texture_path(path, sizeof(path), t->tag, t->bitmap);
    error = texture_read(path, size, &t->data);
    if (error == CONTENT_OK)
        error = hwt_parse(t->data, size, &t->header);
    if (error == CONTENT_OK && (t->header.type != HWT_TYPE_2D || t->header.faces != 1 || t->header.depth != 1))
        error = CONTENT_DIMENSIONS;
    if (error == CONTENT_OK && width > 0 && ((int)t->header.width != width || (int)t->header.height != height))
        error = CONTENT_DIMENSIONS;
    if (error == CONTENT_OK)
        error = hwt_image_at(&t->header, 0, &image);
    if (error == CONTENT_OK && (!image.gx_loadable || (image.offset & 31u)))
        error = CONTENT_DIMENSIONS;
    /* the alpha plane: the same chain in I4 or I8, after the colour images */
    struct hwt_image alpha_image;
    t->alpha_plane = error == CONTENT_OK && t->header.alpha_kind == HWT_ALPHA_PLANE;
    if (t->alpha_plane) {
        error = hwt_alpha_image_at(&t->header, 0, &alpha_image);
        if (error == CONTENT_OK && (alpha_image.offset & 31u))
            error = CONTENT_DIMENSIONS;
    }
    if (error != CONTENT_OK) {
        wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=%s error=%s\n", t->tag, t->bitmap,
                lightmap ? "lightmap" : "base", content_error_name(error));
        store_give(t->data, size);
        t->data = NULL;
        gx.pool[0].used = mark[0];
        gx.pool[1].used = mark[1];
        return 0;
    }
    int levels = (int)t->header.levels;
    u8 wrap = lightmap ? GX_CLAMP : GX_REPEAT;
    GX_InitTexObj(&t->object, t->data + image.offset, (u16)image.width, (u16)image.height,
                  (u8)t->header.gx_format, wrap, wrap, levels > 1 ? GX_TRUE : GX_FALSE);
    if (levels > 1)
        GX_InitTexObjLOD(&t->object, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(levels - 1), 0.0f, GX_FALSE, GX_FALSE,
                         GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(&t->object, GX_LINEAR, GX_LINEAR);
    if (t->alpha_plane) {
        /* sampled with the colour's texcoord, wrap and filter: the same texel
        footprint, so colour and alpha stay one RGBA texel */
        GX_InitTexObj(&t->alpha_object, t->data + alpha_image.offset, (u16)image.width, (u16)image.height,
                      (u8)t->header.alpha_gx_format, wrap, wrap, levels > 1 ? GX_TRUE : GX_FALSE);
        if (levels > 1)
            GX_InitTexObjLOD(&t->alpha_object, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(levels - 1), 0.0f, GX_FALSE,
                             GX_FALSE, GX_ANISO_1);
        else
            GX_InitTexObjFilterMode(&t->alpha_object, GX_LINEAR, GX_LINEAR);
    }
    DCFlushRange(t->data, align32(size));
    t->file_bytes = size;
    t->bytes = size;
    t->resident = 1;
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

static unsigned long file_size(long tag, long bitmap, int *found)
{
    char path[192];
    struct stat information;
    texture_path(path, sizeof(path), tag, bitmap);
    *found = stat(path, &information) == 0;
    return *found ? (unsigned long)information.st_size : 0ul;
}

int wii_gx_textures_load(long lightmap_tag_index)
{
    unsigned long lightmap_bytes = 0, base_bytes = 0, missing_bytes = 0, pool_short_bytes = 0;
    int lightmap_pages = 0, lightmap_failed = 0, base_total = 0, base_resident = 0, base_failed = 0;
    int base_pool_short = 0, base_budget_short = 0;
    long kinds[NUMBER_OF_WII_GX_MATERIAL_KINDS] = {0}, kind_surfaces[NUMBER_OF_WII_GX_MATERIAL_KINDS] = {0};
    long detail = 0, bump = 0, no_base = 0, no_lightmap = 0;
    long alpha_used = 0, alpha_used_split = 0, environment_types[4] = {0};
    long environment_surfaces_textured = 0, environment_surfaces_lightmap_only = 0;
    int base_split = 0, base_planes = 0, base_alpha_omitted = 0, base_limited = 0;
    int geometry_equal = 1;

    gx.lightmap_tag = lightmap_tag_index;
    release_textures();
    if (gx.geometry_check)
        geometry_equal = geometry_check();
    /* the base maps, by the surfaces they cover */
    for (long i = 0; i < gx.material_count; ++i) {
        const struct wii_gx_material *m = &gx.materials[i].engine;
        kinds[m->kind]++;
        kind_surfaces[m->kind] += m->surface_count;
        if (m->kind != _wii_gx_material_environment)
            continue;
        detail += m->detail_maps > 0;
        bump += m->bump_map;
        environment_types[m->environment_type >= 0 && m->environment_type < 3 ? m->environment_type : 3]++;
        if (m->base_tag < 0) {
            no_base++;
            continue;
        }
        int t = texture_find(m->base_tag, m->base_bitmap);
        if (t < 0) {
            if (gx.texture_count == gx.texture_capacity)
                return 0;
            t = gx.texture_count++;
            gx.textures[t].tag = m->base_tag;
            gx.textures[t].bitmap = m->base_bitmap;
        }
        gx.textures[t].coverage += m->surface_count;
    }
    if (gx.texture_count)
        qsort(gx.textures, (size_t)gx.texture_count, sizeof(gx.textures[0]), coverage_order);
    base_total = gx.texture_count;
    /* lightmap pages first (always resident) */
    for (long i = 0; i < gx.material_count; ++i) {
        const struct wii_gx_material *m = &gx.materials[i].engine;
        if (m->lightmap_page < 0 || lightmap_tag_index < 0 || gx.materials[i].lightmap_vertex_count == 0)
            continue;
        if (texture_find(lightmap_tag_index, m->lightmap_page) >= 0)
            continue;
        if (gx.texture_count == gx.texture_capacity)
            return 0;
        struct texture *t = &gx.textures[gx.texture_count++];
        int found;
        unsigned long size;
        t->tag = lightmap_tag_index;
        t->bitmap = m->lightmap_page;
        t->coverage = -1;
        lightmap_pages++;
        size = file_size(t->tag, t->bitmap, &found);
        if (found && texture_load(t, 1, 0, 0, (uint32_t)size))
            lightmap_bytes += t->bytes;
        else {
            if (!found)
                wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=lightmap error=missing\n", t->tag, t->bitmap);
            lightmap_failed++;
        }
    }
    /* then the base maps that fit the budget and the pool */
    for (int i = 0; i < base_total; ++i) {
        struct texture *t = &gx.textures[i];
        int width = 0, height = 0, found;
        for (long k = 0; k < gx.material_count; ++k)
            if (gx.materials[k].engine.base_tag == t->tag && gx.materials[k].engine.base_bitmap == t->bitmap) {
                width = gx.materials[k].engine.base_width;
                height = gx.materials[k].engine.base_height;
                break;
            }
        unsigned long size = file_size(t->tag, t->bitmap, &found);
        if (!found) {
            wii_log("RENDER_TEXTURE_FAIL tag=%ld bitmap=%ld kind=base error=missing\n", t->tag, t->bitmap);
            base_failed++;
            continue;
        }
        if (gx.base_limit >= 0 && base_resident >= gx.base_limit) {
            missing_bytes += size;
            base_limited++;
            wii_log("RENDER_BASE_NOT_RESIDENT tag=%ld bitmap=%ld bytes=%lu surfaces=%ld reason=base_limit\n", t->tag,
                    t->bitmap, size, t->coverage);
            continue;
        }
        if (base_bytes + size > gx.base_budget) {
            missing_bytes += size;
            base_budget_short++;
            wii_log("RENDER_BASE_NOT_RESIDENT tag=%ld bitmap=%ld bytes=%lu surfaces=%ld reason=budget budget_left=%lu\n",
                    t->tag, t->bitmap, size, t->coverage, gx.base_budget - base_bytes);
            continue;
        }
        if (!store_fits((uint32_t)size)) {
            missing_bytes += size;
            pool_short_bytes += size;
            base_pool_short++;
            wii_log("RENDER_BASE_NOT_RESIDENT tag=%ld bitmap=%ld bytes=%lu surfaces=%ld reason=pool pool_left=%lu,%lu\n",
                    t->tag, t->bitmap, size, t->coverage, (unsigned long)(gx.pool[0].bytes - gx.pool[0].used),
                    (unsigned long)(gx.pool[1].bytes - gx.pool[1].used));
            continue;
        }
        if (texture_load(t, 0, width, height, (uint32_t)size)) {
            base_bytes += t->bytes;
            base_resident++;
            base_split += t->header.version == 2;
            base_planes += t->alpha_plane;
            base_alpha_omitted += t->header.alpha_kind == HWT_ALPHA_OMITTED;
            wii_log("RENDER_BASE_RESIDENT tag=%ld bitmap=%ld bytes=%lu surfaces=%ld gx=%lu version=%lu alpha=%s\n",
                    t->tag, t->bitmap, (unsigned long)t->bytes, t->coverage, (unsigned long)t->header.gx_format,
                    (unsigned long)t->header.version,
                    t->alpha_plane ? (t->header.alpha_gx_format == HWT_GX_I4 ? "I4" : "I8")
                                   : t->header.alpha_kind == HWT_ALPHA_OMITTED ? "omitted_255" : "own");
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
        if (m->engine.kind == _wii_gx_material_environment && m->engine.base_alpha_used) {
            alpha_used++;
            alpha_used_split += m->base_texture >= 0 && gx.textures[m->base_texture].header.version == 2;
        }
        if (m->engine.kind == _wii_gx_material_environment && m->engine.base_tag >= 0) {
            if (m->base_texture >= 0)
                environment_surfaces_textured += m->engine.surface_count;
            else
                environment_surfaces_lightmap_only += m->engine.surface_count;
        }
    }
    GX_InvalidateTexAll();
    unsigned long load_ms = (unsigned long)ticks_to_millisecs(gettime() - load_started);
    if (load_ms > gx.load_ms_max)
        gx.load_ms_max = load_ms;
    struct memory_now memory;
    struct wii_cache_load_report slot;
    memory_now(&memory);
    memory_low_water();
    wii_cache_load_report_get(&slot);
    wii_log("RENDER_MAP_LOAD load=%ld map=%s bsp_tag=%lu lightmap_tag=%ld geometry=tag_slot geometry_bytes=%lu "
            "geometry_owned=0 vertex_data_unaligned32=%ld geometry_check=%d lightmap_pages=%d "
            "lightmap_failed=%d lightmap_bytes=%lu base_maps=%d base_resident=%d base_failed=%d base_bytes=%lu "
            "base_not_resident_bytes=%lu base_budget=%lu base_limited=%d base_budget_short=%d base_pool_short=%d "
            "base_pool_short_bytes=%lu base_split=%d base_alpha_planes=%d "
            "base_alpha_omitted=%d environment_surfaces_textured=%ld environment_surfaces_lightmap_only=%ld "
            "pool_used=%lu pool_bytes=%lu pool_used_mem1=%lu pool_used_mem2=%lu owned=%lu owned_peak=%lu "
            "arena1_lo=%p->%p mem1_free=%lu arena2_lo=%p->%p mem2_free=%lu mem2_above_reserve=%lu heap_free=%lu "
            "heap_in_use=%lu tag_slot_tags=%lu tag_slot_bsp=%lu+%lu ms=%lu\n",
            gx.loads, gx.map, gx.bsp_tag, lightmap_tag_index, gx.vertex_data_bytes +
            (unsigned long)gx.surface_count * 6ul, gx.vertex_data_unaligned32, gx.geometry_check ? geometry_equal : -1,
            lightmap_pages, lightmap_failed, lightmap_bytes, base_total, base_resident, base_failed, base_bytes,
            missing_bytes, gx.base_budget, base_limited, base_budget_short, base_pool_short, pool_short_bytes,
            base_split, base_planes, base_alpha_omitted, environment_surfaces_textured,
            environment_surfaces_lightmap_only, (unsigned long)pool_used(), (unsigned long)pool_bytes(),
            (unsigned long)gx.pool[0].used, (unsigned long)gx.pool[1].used, gx.owned, gx.owned_peak,
            (void *)load_arena1_before, (void *)memory.arena1_lo, memory.mem1_free, (void *)load_arena2_before,
            (void *)memory.arena2_lo, memory.mem2_free, memory.mem2_above_reserve, memory.heap_free,
            memory.heap_in_use, slot.range_count > 0 ? slot.range_size[0] : 0ul,
            slot.range_count > 1 ? slot.range_offset[1] : 0ul, slot.range_count > 1 ? slot.range_size[1] : 0ul,
            load_ms);
    wii_log("RENDER_MATERIALS load=%ld environment=%ld environment_surfaces=%ld transparent_placeholder=%ld "
            "transparent_surfaces=%ld other_placeholder=%ld other_surfaces=%ld detail_maps_not_drawn=%ld "
            "bump_maps_not_drawn=%ld environment_without_base=%ld environment_without_lightmap=%ld "
            "environment_types=%ld/%ld/%ld/%ld base_alpha_used=%ld base_alpha_used_split=%ld\n",
            gx.loads, kinds[0], kind_surfaces[0], kinds[1], kind_surfaces[1], kinds[2], kind_surfaces[2], detail, bump,
            no_base, no_lightmap, environment_types[0], environment_types[1], environment_types[2],
            environment_types[3], alpha_used, alpha_used_split);
    if (!geometry_equal)
        wii_log("RENDER_FAIL geometry_check bsp_tag=%lu (the tag slot's geometry differs from the HWL1 file: not drawn)\n",
                gx.bsp_tag);
    gx.ready = lightmap_failed == 0 && geometry_equal;
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

/* the base map's alpha into APREV from stage tev on (the base map's own
alpha, or a split texture's plane in the next stage); returns the stages used */
static int tev_base_alpha(u8 tev, const struct texture *base)
{
    if (!base->alpha_plane) {
        /* RGBA8's alpha, or 255: a version 2 texture without a plane decodes
        CMPR's four-colour blocks as opaque */
        GX_SetTevAlphaIn(tev, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        GX_SetTevAlphaOp(tev, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        return 0;
    }
    u8 next = (u8)(tev + 1);
    GX_LoadTexObj((GXTexObj *)&base->alpha_object, GX_TEXMAP2);
    GX_SetTevOrder(next, GX_TEXCOORD0, GX_TEXMAP2, GX_COLORNULL);
    GX_SetTevColorIn(next, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GX_SetTevColorOp(next, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(next, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);   /* I4/I8: alpha = intensity */
    GX_SetTevAlphaOp(next, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    return 1;
}

/* the lightmap (TEXMAP1, TEXCOORD1) and/or the base map (TEXMAP0, TEXCOORD0);
with base_alpha, the output alpha is the base map's (tev_base_alpha) */
static void tev_textures(const struct texture *lightmap, const struct texture *base, int base_alpha)
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
        if (base_alpha)
            stage += tev_base_alpha(tev, base);
    }
    GX_SetNumTevStages((u8)stage);
}

/* DRAW_BASE_ALPHA: the base map's alpha as tev_textures combines it, shown as
grey through a last stage (APREV) */
static void tev_alpha_view(const struct texture *base)
{
    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_LoadTexObj((GXTexObj *)&base->object, GX_TEXMAP0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    u8 last = (u8)(GX_TEVSTAGE1 + tev_base_alpha(GX_TEVSTAGE0, base));
    GX_SetTevOrder(last, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(last, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_APREV);
    GX_SetTevColorOp(last, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(last, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(last, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetNumTevStages((u8)(last + 1));
}

static void set_arrays(const struct material *m, int lightmap)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX1, lightmap ? GX_INDEX16 : GX_NONE);
    /* the tag slot's vertices: position at 0 and texcoord at 24 of each
    32-byte vertex, the lightmap texcoord at 4 of each 8-byte lightmap vertex */
    GX_SetArray(GX_VA_POS, (void *)m->vertices, VERTEX_BYTES);
    GX_SetArray(GX_VA_TEX0, (void *)(m->vertices + VERTEX_TEXCOORD_OFFSET), VERTEX_BYTES);
    if (lightmap)
        GX_SetArray(GX_VA_TEX1, (void *)(m->lightmap_vertices + LIGHTMAP_TEXCOORD_OFFSET), LIGHTMAP_VERTEX_BYTES);
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
    gx.drawn_textured = gx.drawn_base_missing = 0;
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
            if (m->engine.kind == _wii_gx_material_environment && mode == DRAW_BASE_ALPHA) {
                if (m->base_texture >= 0)
                    tev_alpha_view(&gx.textures[m->base_texture]);
                else
                    tev_constant(opaque_white);
            } else if (m->engine.kind == _wii_gx_material_environment) {
                const struct texture *l = m->lightmap_texture >= 0 && mode != DRAW_BASE_ONLY
                                              ? &gx.textures[m->lightmap_texture] : NULL;
                const struct texture *b = m->base_texture >= 0 && mode != DRAW_LIGHTMAP_ONLY
                                              ? &gx.textures[m->base_texture] : NULL;
                if (l || b)
                    tev_textures(l, b, m->engine.base_alpha_used);
                else
                    tev_constant(flat_grey);
                lightmap = l != NULL;
            } else {
                tev_constant(transparent ? placeholder_transparent : placeholder_other);
            }
            set_arrays(m, lightmap);
            unsigned long drawn = emit_material(m, flags, lightmap);
            triangles += drawn;
            if (mode == DRAW_FULL && m->engine.kind == _wii_gx_material_environment && m->engine.base_tag >= 0) {
                if (m->base_texture >= 0)
                    gx.drawn_textured += drawn;
                else
                    gx.drawn_base_missing += drawn;
            }
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
        GX_SetArray(GX_VA_POS, (void *)m->vertices, VERTEX_BYTES);
        long first = m->engine.first_surface, end = first + m->engine.surface_count;
        for (long s = first; s < end; ++s) {
            if (!surface_visible(flags, s))
                continue;
            const struct hwl_surface *surface = &gx.surfaces[s];
            float corner[3][3];
            for (int k = 0; k < 3; ++k)
                memcpy(corner[k], m->vertices + VERTEX_BYTES * surface->vertex[k], sizeof(corner[k]));
            int front = frontfacing(view->eye, corner[0], corner[1], corner[2]);
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
    memory_low_water();
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

/* HWI-016D: the environment drawn into the EFB, without the frame copy: the
boot mode's frame (render_ui.c) draws the interface over it and presents the
frame itself */
void wii_gx_environment_draw(const struct wii_gx_view *view, const unsigned long *surface_flags,
                             const unsigned char *material_drawn)
{
    if (!gx.ready)
        return;
    memory_low_water();
    u64 start = gettime();
    load_view(view);
    unsigned long triangles = draw_materials(surface_flags, material_drawn, DRAW_FULL, GX_CULL_BACK);
    unsigned long submit = elapsed_us(start);
    stats_add(&gx.map_stats, view->visibility_us, submit, 0, 0, triangles, view->visible_surfaces,
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
    out->textured_triangles = gx.drawn_textured;
    out->base_missing_triangles = gx.drawn_base_missing;
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
    clear_efb();
    draw_materials(surface_flags, material_drawn, DRAW_BASE_ALPHA, GX_CULL_BACK);
    GX_DrawDone();
    sample_grid(pose_other, GRID_X, GRID_Y);
    out->crc_base_alpha = samples_crc(pose_other, count, &ignored);
    for (int i = 0; i < count; ++i) {
        uint32_t c = pose_other[i], r = c >> 16, g = (c >> 8) & 255u, b = c & 255u;
        out->base_alpha_below_255 += r == g && g == b && r < 255u;
    }
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
