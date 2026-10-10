/* Real-geometry diagnostic (HWI-007A/HWI-016A): one owned Halo BSP material
 * section streamed from SD and drawn through GX with flat per-triangle shading.
 * Not Halo materials, lightmaps, collision, gameplay or engine integration.
 * Private inputs and their expected identities come from an SD manifest. */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "build_id.h"
#include "../../../tools/wii/cache_arena_plan.h"
#include "../../../tools/wii/cache_stream_io.h"
#include "../../../tools/wii/cache_address_owned.h"
#include "../../../tools/wii/cache_bsp_probe.h"
#include "../../../tools/wii/cache_material_probe.h"
#include "../../linux/include/halo_port_capacity.h"

#define VIEW_DIR "sd:/halo-wii-geometry"
#define MANIFEST_PATH VIEW_DIR "/section.txt"
#define LOG_PATH VIEW_DIR "/view.log"
#define RUNS_PATH VIEW_DIR "/runs.txt"
#define RUNS_PREFIX "halo-wii-geometry-v1 "
#define FIFO_BYTES (256u * 1024u)
#define RESERVE_BYTES ((size_t)0x200000)
#define WORKSPACE_BYTES 16384u
#define GRID_X 32
#define GRID_Y 24
#define CHECK_CYCLES 3

enum { TAG_SLOT, STATE_SLOT, SOUND_SLOT, IO0_SLOT, IO1_SLOT, BSP_CONTROL_SLOT, MATERIAL_CONTROL_SLOT,
       MATERIAL_WORKSPACE_SLOT, POSITION_SLOT, INDEX_SLOT, SHADE_SLOT, SLOT_COUNT };
_Static_assert(SLOT_COUNT <= CACHE_ARENA_MAX_SLOTS, "arena slot count");

static const GXColor clear_colour = {18, 22, 30, 255};

struct manifest {
    char tag_file[96], bsp_file[96];
    uint32_t tag_bytes, tag_crc32, bsp_bytes, bsp_crc32, map_bytes;
    uint32_t bsp_ordinal, material, surfaces, vertices, index_crc32, position_crc32;
};

struct section {
    struct cache_arena_owner owner;
    struct cache_arena_plan plan;
    uintptr_t begin;
    struct cache_bsp_control *bsp_control;
    struct cache_material_control *material_control;
    struct cache_bsp_view bsp_view;
    struct cache_material_view material_view;
    struct cache_arena_handle tag;
    uint32_t *positions;
    uint16_t *indices;
    GXColor *shades;
    float centre[3], radius;
    uint32_t index_crc32, position_crc32;
    unsigned degenerate, floors;
    u64 load_us;
};

static FILE *record;
static int log_failed;
static uintptr_t stack_entry, stack_lowest;

static void view_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void view_log(const char *format, ...)
{
    va_list args;
    if (record == NULL)
        return;
    va_start(args, format);
    if (vfprintf(record, format, args) < 0)
        log_failed = 1;
    va_end(args);
}

__attribute__((noinline)) static void sample_stack(void)
{
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (here < stack_lowest)
        stack_lowest = here;
}

/* Reflected CRC-32 (0xEDB88320), incremental; checked against the probe's CRC. */
static uint32_t crc_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
    crc = ~crc;
    while (length--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & -(crc & 1));
    }
    return ~crc;
}

static uint32_t crc_le32(uint32_t crc, uint32_t value)
{
    const unsigned char bytes[4] = {value, value >> 8, value >> 16, value >> 24};
    return crc_update(crc, bytes, 4);
}

static uint32_t crc_le16(uint32_t crc, uint16_t value)
{
    const unsigned char bytes[2] = {value, value >> 8};
    return crc_update(crc, bytes, 2);
}

/* Strict "key value" lines; every key required exactly once, nothing else allowed. */
static int parse_manifest(const char *text, struct manifest *out)
{
    static const char *const keys[] = {"tag_file", "bsp_file", "tag_bytes", "tag_crc32", "bsp_bytes",
        "bsp_crc32", "map_bytes", "bsp_ordinal", "material", "surfaces", "vertices", "index_crc32",
        "position_crc32"};
    enum { KEYS = sizeof(keys) / sizeof(keys[0]) };
    uint32_t *numbers[KEYS] = {NULL, NULL, &out->tag_bytes, &out->tag_crc32, &out->bsp_bytes,
        &out->bsp_crc32, &out->map_bytes, &out->bsp_ordinal, &out->material, &out->surfaces,
        &out->vertices, &out->index_crc32, &out->position_crc32};
    unsigned seen = 0;
    memset(out, 0, sizeof(*out));
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        char line[160], key[32], value[128];
        if (length >= sizeof(line))
            return 0;
        memcpy(line, text, length);
        line[length] = 0;
        if (length && line[length - 1] == '\r')
            line[length - 1] = 0;
        text += length + (end != NULL);
        if (!line[0])
            continue;
        char tail;
        if (sscanf(line, "%31s %127s %c", key, value, &tail) != 2)
            return 0;
        unsigned k = 0;
        while (k < KEYS && strcmp(key, keys[k]))
            ++k;
        if (k == KEYS || seen & (1u << k))
            return 0;
        seen |= 1u << k;
        if (k < 2) {
            if (strncmp(value, "sd:/", 4) || strlen(value) >= sizeof(out->tag_file))
                return 0;
            strcpy(k ? out->bsp_file : out->tag_file, value);
        } else {
            char *stop;
            errno = 0;
            unsigned long parsed = strtoul(value, &stop, 0);
            if (errno || *stop || parsed > UINT32_MAX || value[0] == '-')
                return 0;
            *numbers[k] = (uint32_t)parsed;
        }
    }
    return seen == (1u << KEYS) - 1 && out->tag_bytes && out->tag_bytes <= CACHE_BSP_TAG_LIMIT &&
           out->bsp_bytes && out->vertices && out->vertices <= 64000 && out->surfaces &&
           out->surfaces <= 131072;
}

static int read_text(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    size_t used = fread(buffer, 1, capacity - 1, file);
    int ok = !ferror(file) && fgetc(file) == EOF;
    if (fclose(file) != 0)
        ok = 0;
    buffer[used] = 0;
    return ok && memchr(buffer, 0, used) == NULL;
}

static int read_runs(unsigned *previous)
{
    char text[64];
    *previous = 0;
    FILE *probe = fopen(RUNS_PATH, "rb");
    if (probe == NULL)
        return errno == ENOENT;
    fclose(probe);
    if (!read_text(RUNS_PATH, text, sizeof(text)) || strncmp(text, RUNS_PREFIX, sizeof(RUNS_PREFIX) - 1))
        return 0;
    char *stop;
    const char *digits = text + sizeof(RUNS_PREFIX) - 1;
    errno = 0;
    unsigned long count = strtoul(digits, &stop, 10);
    if (errno || stop == digits || strcmp(stop, "\n") || count >= UINT32_MAX)
        return 0;
    *previous = (unsigned)count;
    return 1;
}

static int init_storage(unsigned *previous)
{
    if (!fatInitDefault())
        return 0;
    if (mkdir(VIEW_DIR, 0777) != 0 && errno != EEXIST)
        return -1;
    if (!read_runs(previous))
        return -1;
    FILE *file = fopen(RUNS_PATH, "w");
    if (file == NULL)
        return -1;
    int written = fprintf(file, RUNS_PREFIX "%u\n", *previous + 1);
    if (fclose(file) != 0 || written < 0)
        return -1;
    unsigned readback;
    if (!read_runs(&readback) || readback != *previous + 1)
        return -1;
    record = fopen(LOG_PATH, "a");
    return record == NULL ? -1 : 1;
}

static unsigned char *slot(const struct cache_arena_owner *owner, size_t index, size_t bytes)
{
    struct cache_arena_handle handle;
    struct cache_arena_result status;
    unsigned char *view = NULL;
    if (!cache_arena_owner_handle(owner, index, &handle, &status) ||
        !cache_arena_owner_resolve(owner, &handle, 0, bytes, &view, &status))
        return NULL;
    return view;
}

static size_t round32(size_t value)
{
    return (value + 31) & ~(size_t)31;
}

static float bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

enum fault { FAULT_NONE, FAULT_BSP_CRC, FAULT_TAG_LENGTH };

/* Releases the owner and restores MEM2 arena low; safe after any partial load. */
static int release_section(struct section *s, uintptr_t arena_lo)
{
    struct cache_arena_result status;
    int ok = cache_arena_owner_release(&s->owner, &status);
    if ((uintptr_t)SYS_GetArena2Lo() != s->begin + s->plan.required)
        ok = 0;
    SYS_SetArena2Lo((void *)arena_lo);
    return ok;
}

static int load_section(const struct manifest *m, struct section *s, unsigned placement, enum fault fault,
                        uintptr_t lo, uintptr_t hi, const char **failure)
{
    sample_stack();
    u64 started = gettime();
    const struct cache_arena_request requests[SLOT_COUNT] = {
        {CACHE_BSP_TAG_LIMIT, 64}, {HALO_PORT_GAME_STATE_SIZE, 64}, {0x400000, 64},
        {CACHE_STREAM_IO_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64},
        {sizeof(struct cache_bsp_control), 64}, {sizeof(struct cache_material_control), 64},
        {WORKSPACE_BYTES, 64}, {round32((size_t)m->vertices * 12), 32},
        {round32((size_t)m->surfaces * 6), 32}, {round32((size_t)m->surfaces * 4), 32}};
    struct cache_arena_result arena;
    s->begin = lo + (placement & 1 ? 4096u : 0u);
    if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)s->begin, hi - s->begin, RESERVE_BYTES,
                                &s->plan, &arena)) {
        *failure = "arena_plan";
        return 0;
    }
    SYS_SetArena2Lo((void *)(s->begin + s->plan.required));
    if (!cache_arena_owner_bind(&s->owner, &s->plan, (void *)s->begin, hi - s->begin, &arena)) {
        SYS_SetArena2Lo((void *)lo);
        *failure = "owner_bind";
        return 0;
    }
    memset((void *)s->begin, 0xa5, s->plan.required);
    struct cache_arena_handle io0, io1;
    struct cache_stream_result stream = {0};
    if (!cache_arena_owner_handle(&s->owner, TAG_SLOT, &s->tag, &arena) ||
        !cache_arena_owner_handle(&s->owner, IO0_SLOT, &io0, &arena) ||
        !cache_arena_owner_handle(&s->owner, IO1_SLOT, &io1, &arena)) {
        *failure = "handles";
        goto fail;
    }
    FILE *input = fopen(m->tag_file, "rb");
    if (input == NULL) {
        *failure = "tag_open";
        goto fail;
    }
    size_t tag_bytes = m->tag_bytes - (fault == FAULT_TAG_LENGTH);
    int read = cache_stream_read(input, &s->owner, &s->tag, &io0, &io1, tag_bytes, m->tag_crc32, &stream);
    if (fclose(input) != 0 || !read) {
        *failure = read ? "tag_close" : cache_stream_error_name(stream.error);
        goto fail;
    }
    struct cache_bsp_reference reference;
    struct cache_bsp_result bsp;
    if (!cache_bsp_select(&s->owner, &s->tag, m->tag_bytes, m->map_bytes, m->bsp_ordinal, &reference, &bsp) ||
        (uint32_t)reference.file_size != m->bsp_bytes) {
        *failure = "bsp_select";
        goto fail;
    }
    input = fopen(m->bsp_file, "rb");
    if (input == NULL) {
        *failure = "bsp_open";
        goto fail;
    }
    stream = (struct cache_stream_result){0};
    read = cache_stream_read_at(input, &s->owner, &s->tag, &io0, &io1, reference.slot_offset, m->bsp_bytes,
                                m->bsp_crc32 ^ (fault == FAULT_BSP_CRC), &stream);
    if (fclose(input) != 0 || !read) {
        *failure = read ? "bsp_close" : cache_stream_error_name(stream.error);
        goto fail;
    }
    s->bsp_control = (void *)slot(&s->owner, BSP_CONTROL_SLOT, sizeof(*s->bsp_control));
    s->material_control = (void *)slot(&s->owner, MATERIAL_CONTROL_SLOT, sizeof(*s->material_control));
    void *workspace = slot(&s->owner, MATERIAL_WORKSPACE_SLOT, WORKSPACE_BYTES);
    s->positions = (void *)slot(&s->owner, POSITION_SLOT, (size_t)m->vertices * 12);
    s->indices = (void *)slot(&s->owner, INDEX_SLOT, (size_t)m->surfaces * 6);
    s->shades = (void *)slot(&s->owner, SHADE_SLOT, (size_t)m->surfaces * 4);
    if (!s->bsp_control || !s->material_control || !workspace || !s->positions || !s->indices || !s->shades) {
        *failure = "slot_views";
        goto fail;
    }
    memset(s->bsp_control, 0, sizeof(*s->bsp_control));
    memset(s->material_control, 0, sizeof(*s->material_control));
    struct cache_material_requirements requirements;
    struct cache_material_result material;
    if (!cache_bsp_bind(s->bsp_control, &s->owner, &s->tag, m->tag_bytes, m->map_bytes, m->bsp_ordinal,
                        &s->bsp_view, &bsp) ||
        !cache_material_measure(&s->bsp_view, &requirements, &material) ||
        requirements.workspace_bytes > WORKSPACE_BYTES ||
        !cache_material_bind(s->material_control, &s->bsp_view, workspace, requirements.workspace_bytes,
                             &s->material_view, &material)) {
        *failure = "bind";
        goto fail;
    }
    struct cache_material_projection projection;
    if (!cache_material_get_material(&s->material_view, m->material, &projection, &material) ||
        projection.vertex_buffers[0].type != 1 ||
        (uint32_t)projection.vertex_buffers[0].count != m->vertices ||
        (uint32_t)projection.surface_count != m->surfaces) {
        *failure = "material_identity";
        goto fail;
    }
    /* Positions: raw bits with integer stores; the GP reads them as F32. */
    float lower[3] = {INFINITY, INFINITY, INFINITY}, upper[3] = {-INFINITY, -INFINITY, -INFINITY};
    uint32_t crc = 0;
    for (uint32_t v = 0; v < m->vertices; ++v) {
        struct cache_material_compressed_vertex_projection vertex;
        if (!cache_material_get_compressed_vertex(&s->material_view, m->material, v, &vertex, &material)) {
            *failure = "vertex_get";
            goto fail;
        }
        for (unsigned axis = 0; axis < 3; ++axis) {
            uint32_t bits = vertex.position_bits[axis];
            if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) {
                *failure = "vertex_nonfinite";
                goto fail;
            }
            s->positions[v * 3 + axis] = bits;
            crc = crc_le32(crc, bits);
            float value = bits_float(bits);
            lower[axis] = fminf(lower[axis], value);
            upper[axis] = fmaxf(upper[axis], value);
        }
    }
    s->position_crc32 = crc;
    crc = 0;
    s->degenerate = s->floors = 0;
    const float light[3] = {0.40f, 0.30f, 0.866f};
    for (uint32_t t = 0; t < m->surfaces; ++t) {
        struct cache_material_surface_projection surface;
        struct cache_material_surface_positions_projection corners;
        if (!cache_material_get_material_surface(&s->material_view, m->material, t, &surface, &material) ||
            !cache_material_get_surface_positions(&s->material_view, m->material, t, &corners, &material)) {
            *failure = "surface_get";
            goto fail;
        }
        float p[3][3];
        for (unsigned c = 0; c < 3; ++c) {
            uint16_t index = surface.vertex_indices[c];
            s->indices[t * 3 + c] = index;
            crc = crc_le16(crc, index);
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (corners.position_bits[c][axis] != s->positions[index * 3u + axis]) {
                    *failure = "boundary_array_mismatch";
                    goto fail;
                }
                p[c][axis] = bits_float(corners.position_bits[c][axis]);
            }
        }
        float e1[3], e2[3], n[3];
        for (unsigned a = 0; a < 3; ++a) {
            e1[a] = p[1][a] - p[0][a];
            e2[a] = p[2][a] - p[0][a];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(length > 0)) {
            ++s->degenerate;
            s->shades[t] = (GXColor){255, 0, 255, 255};
            continue;
        }
        float lambert = fabsf(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / length;
        float intensity = 0.28f + 0.72f * lambert;
        int is_floor = fabsf(n[2]) / length > 0.7f;
        s->floors += is_floor;
        const float base[3] = {is_floor ? 150 : 178, is_floor ? 172 : 166, is_floor ? 128 : 150};
        s->shades[t] = (GXColor){(u8)(base[0] * intensity), (u8)(base[1] * intensity),
                                 (u8)(base[2] * intensity), 255};
    }
    s->index_crc32 = crc;
    if (s->index_crc32 != m->index_crc32 || s->position_crc32 != m->position_crc32) {
        *failure = "section_crc";
        goto fail;
    }
    float extent = 0;
    for (unsigned a = 0; a < 3; ++a) {
        s->centre[a] = (lower[a] + upper[a]) * 0.5f;
        extent += (upper[a] - lower[a]) * (upper[a] - lower[a]);
    }
    s->radius = sqrtf(extent) * 0.5f;
    DCFlushRange(s->positions, m->vertices * 12);
    GX_SetArray(GX_VA_POS, s->positions, 3 * sizeof(uint32_t));
    GX_InvVtxCache();
    s->load_us = ticks_to_microsecs(gettime() - started);
    return 1;
fail:
    release_section(s, lo);
    return 0;
}

static void set_desc(int indexed)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, indexed ? GX_INDEX16 : GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

static void draw_section(const struct section *s, const struct manifest *m, const Mtx view)
{
    sample_stack();
    GX_LoadPosMtxImm((MtxP)view, GX_PNMTX0);
    set_desc(1);
    /* Winding is not yet qualified for this content, so both faces draw. */
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    for (uint32_t first = 0; first < m->surfaces; first += 20000) {
        uint32_t count = m->surfaces - first < 20000 ? m->surfaces - first : 20000;
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, count * 3);
        for (uint32_t t = first; t < first + count; ++t)
            for (unsigned c = 0; c < 3; ++c) {
                GX_Position1x16(s->indices[t * 3 + c]);
                GX_Color4u8(s->shades[t].r, s->shades[t].g, s->shades[t].b, 255);
            }
        GX_End();
    }
}

static u16 glyph(char c)
{
    static const u16 digits[16] = {075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111,
                                   075757, 075717, 075755, 065656, 074447, 065556, 074747, 074744};
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    if (c >= 'a' && c <= 'f')
        return digits[c - 'a' + 10];
    return c == '-' ? 000700 : 0;
}

static void draw_text(const char *text, f32 x, f32 y, f32 cell, GXColor colour)
{
    u32 lit = 0;
    for (const char *c = text; *c; ++c)
        lit += __builtin_popcount(glyph(*c));
    if (!lit)
        return;
    GX_Begin(GX_QUADS, GX_VTXFMT0, lit * 4);
    for (const char *c = text; *c; ++c, x += cell * 4)
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column) {
                if (!(glyph(*c) & (1u << (14 - row * 3 - column))))
                    continue;
                f32 l = x + column * cell, t = y + row * cell;
                const f32 q[4][2] = {{l, t}, {l + cell, t}, {l + cell, t + cell}, {l, t + cell}};
                for (int i = 0; i < 4; ++i) {
                    GX_Position3f32(q[i][0], q[i][1], -0.5f);
                    GX_Color4u8(colour.r, colour.g, colour.b, 255);
                }
            }
    GX_End();
}

static void camera(const struct section *s, f32 yaw, f32 pitch, f32 distance, Mtx view)
{
    guVector eye = {s->centre[0] + distance * cosf(pitch) * cosf(yaw),
                    s->centre[1] + distance * cosf(pitch) * sinf(yaw),
                    s->centre[2] + distance * sinf(pitch)};
    guVector up = {0, 0, 1}, target = {s->centre[0], s->centre[1], s->centre[2]};
    guLookAt(view, &eye, &up, &target);
}

static void projection(const struct section *s, const GXRModeObj *mode)
{
    Mtx44 perspective;
    guPerspective(perspective, 60, (f32)mode->fbWidth / mode->efbHeight, s->radius * 0.02f, s->radius * 30.0f);
    GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);
}

/* Fixed pose, EFB sampled on a grid: count of non-clear samples and their CRC. */
static void verify_frame(const struct section *s, const struct manifest *m, const GXRModeObj *mode,
                         void *scratch, unsigned *visible, uint32_t *grid_crc)
{
    Mtx view;
    projection(s, mode);
    camera(s, 0.8f, 0.55f, s->radius * 1.6f, view);
    draw_section(s, m, view);
    GX_DrawDone();
    *visible = 0;
    *grid_crc = 0;
    for (int gy = 0; gy < GRID_Y; ++gy)
        for (int gx = 0; gx < GRID_X; ++gx) {
            GXColor c;
            GX_PeekARGB((u16)((gx * 2 + 1) * mode->fbWidth / (GRID_X * 2)),
                        (u16)((gy * 2 + 1) * mode->efbHeight / (GRID_Y * 2)), &c);
            *visible += c.r != clear_colour.r || c.g != clear_colour.g || c.b != clear_colour.b;
            const unsigned char bytes[3] = {c.r, c.g, c.b};
            *grid_crc = crc_update(*grid_crc, bytes, 3);
        }
    GX_CopyDisp(scratch, GX_TRUE);
    GX_DrawDone();
}

struct frame_state {
    void *xfb[2];
    int fb;
    unsigned frames;
    u64 render_max_us;
    u32 fifo_peak;
};

#define PI_FIFO_WRITE_POINTER (*(volatile u32 *)0xCC003014)
#define PI_FIFO_ADDRESS_MASK 0x03FFFFE0u

static void present(const struct section *s, const struct manifest *m, const GXRModeObj *mode,
                    struct frame_state *f, const Mtx view, const char *line1, const char *line2)
{
    u64 start = gettime();
    u32 before = PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK;
    projection(s, mode);
    draw_section(s, m, view);
    Mtx44 ortho;
    Mtx identity;
    guOrtho(ortho, 0, mode->efbHeight, 0, mode->fbWidth, 0, 1);
    guMtxIdentity(identity);
    GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    set_desc(0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    draw_text(WII_BUILD_ID, 24, 24, 4, (GXColor){255, 220, 64, 255});
    draw_text(line1, 24, 52, 4, (GXColor){240, 240, 240, 255});
    draw_text(line2, 24, 80, 4, (GXColor){64, 220, 255, 255});
    GX_DrawDone();
    u32 submitted = ((PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK) - before + FIFO_BYTES) % FIFO_BYTES;
    if (submitted > f->fifo_peak)
        f->fifo_peak = submitted;
    u64 elapsed = ticks_to_microsecs(gettime() - start);
    if (elapsed > f->render_max_us)
        f->render_max_us = elapsed;
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_CopyDisp(f->xfb[f->fb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(f->xfb[f->fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    f->fb ^= 1;
    ++f->frames;
}

/* After release, every retained view and handle must reject. */
static int stale_rejects(const struct section *s)
{
    struct cache_material_root_projection root;
    struct cache_material_result material;
    struct cache_address_span span;
    struct cache_bsp_result bsp;
    struct cache_arena_result arena;
    unsigned char *view = NULL;
    return !cache_material_get_root(&s->material_view, &root, &material) && material.error == CACHE_MATERIAL_STATE &&
           !cache_bsp_get_root(&s->bsp_view, &span, &bsp) && bsp.error == CACHE_BSP_STATE &&
           !cache_arena_owner_resolve(&s->owner, &s->tag, 0, 1, &view, &arena) && arena.error == CACHE_ARENA_STATE;
}

int main(void)
{
    stack_entry = stack_lowest = (uintptr_t)__builtin_frame_address(0);
    VIDEO_Init();
    PAD_Init();
    WPAD_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL)
        return 1;
    unsigned previous_runs = 0;
    int storage = init_storage(&previous_runs);
    view_log("BEGIN target=geometry_view build=%s previous_runs=%u storage=%d video=%ux%u efb_height=%u\n",
             WII_BUILD_ID, previous_runs, storage, mode->fbWidth, mode->xfbHeight, mode->efbHeight);
    struct mallinfo heap_start = mallinfo();
    uintptr_t arena_lo = (uintptr_t)SYS_GetArena2Lo(), arena_hi = (uintptr_t)SYS_GetArena2Hi();
    view_log("ARENAS mem1=%p..%p bytes=%u mem2=%p..%p bytes=%u\n", SYS_GetArena1Lo(), SYS_GetArena1Hi(),
             SYS_GetArena1Size(), (void *)arena_lo, (void *)arena_hi, SYS_GetArena2Size());

    void *fifo = memalign(32, FIFO_BYTES);
    u32 xfb_bytes = VIDEO_GetFrameBufferSize(mode);
    void *xfb0 = SYS_AllocateFramebuffer(mode), *xfb1 = SYS_AllocateFramebuffer(mode);
    if (fifo == NULL || xfb0 == NULL || xfb1 == NULL) {
        view_log("ALLOC_FAIL fifo=%p xfb0=%p xfb1=%p\n", fifo, xfb0, xfb1);
        return 1;
    }
    view_log("OWN gx_fifo=%p bytes=%u cache=zeroed_then_DCFlushRange xfb0=%p xfb1=%p bytes_each=%u cache=K1_uncached\n",
             fifo, FIFO_BYTES, xfb0, xfb1, xfb_bytes);
    memset(fifo, 0, FIFO_BYTES);
    DCFlushRange(fifo, FIFO_BYTES);
    struct frame_state frame = {{MEM_K0_TO_K1(xfb0), MEM_K0_TO_K1(xfb1)}, 0, 0, 0, 0};
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(frame.xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    GX_Init(fifo, FIFO_BYTES);
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
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_CopyDisp(frame.xfb[1], GX_TRUE);
    GX_DrawDone();

    int failures = 0;
    const unsigned char check[] = "123456789";
    if (crc_update(0, check, 9) != UINT32_C(0xCBF43926) || cache_probe_crc32(check, 9) != UINT32_C(0xCBF43926)) {
        view_log("FAIL crc_self_test\n");
        ++failures;
    }
    struct manifest manifest, rejected;
    memset(&manifest, 0, sizeof(manifest));
    static char text[2048];
    int manifest_ok = storage > 0 && read_text(MANIFEST_PATH, text, sizeof(text)) && parse_manifest(text, &manifest);
    view_log("MANIFEST ok=%d material=%" PRIu32 " surfaces=%" PRIu32 " vertices=%" PRIu32 " tag_bytes=%" PRIu32
             " bsp_bytes=%" PRIu32 "\n", manifest_ok, manifest.material, manifest.surfaces, manifest.vertices,
             manifest.tag_bytes, manifest.bsp_bytes);
    /* Malformed manifests must reject: a duplicate key, a missing key and an unknown key. */
    if (manifest_ok) {
        char altered[2112];
        int rejects = 0;
        snprintf(altered, sizeof(altered), "%smaterial 1\n", text);
        rejects += !parse_manifest(altered, &rejected);
        const char *cut = strstr(text, "vertices ");
        snprintf(altered, sizeof(altered), "%.*s", cut ? (int)(cut - text) : 0, text);
        rejects += !parse_manifest(altered, &rejected);
        snprintf(altered, sizeof(altered), "%sextra 1\n", text);
        rejects += !parse_manifest(altered, &rejected);
        view_log("MALFORMED manifest_rejections=%d/3\n", rejects);
        failures += rejects != 3;
    } else {
        ++failures;
    }
    if (record != NULL && fflush(record) != 0)
        log_failed = 1;

    unsigned cycles = 0, stale = 0;
    unsigned visible0 = 0;
    uint32_t grid0 = 0;
    size_t max_required = 0;
    struct mallinfo heap_loaded = heap_start;
    for (unsigned cycle = 0; manifest_ok && !failures && cycle <= CHECK_CYCLES; ++cycle) {
        struct section section;
        memset(&section, 0, sizeof(section));
        const char *failure = "none";
        if (!load_section(&manifest, &section, cycle, FAULT_NONE, arena_lo, arena_hi, &failure)) {
            view_log("FAIL load cycle=%u reason=%s\n", cycle, failure);
            ++failures;
            break;
        }
        if (section.plan.required > max_required)
            max_required = section.plan.required;
        unsigned visible;
        uint32_t grid;
        verify_frame(&section, &manifest, mode, frame.xfb[frame.fb], &visible, &grid);
        if (cycle == 0) {
            visible0 = visible;
            grid0 = grid;
        }
        int same = visible == visible0 && grid == grid0 && visible >= 16;
        view_log("CYCLE n=%u placement=%u required=%lu charge=%lu remaining=%lu load_us=%" PRIu64
                 " index_crc=%08" PRIx32 " position_crc=%08" PRIx32 " degenerate=%u floor_tris=%u radius=%.2f"
                 " visible=%u/%d grid_crc=%08" PRIx32 " same_as_first=%d\n",
                 cycle, cycle & 1, (unsigned long)section.plan.required,
                 (unsigned long)(section.begin - arena_lo + section.plan.required),
                 (unsigned long)(arena_hi - section.begin - section.plan.required), section.load_us,
                 section.index_crc32, section.position_crc32, section.degenerate, section.floors, section.radius,
                 visible, GRID_X * GRID_Y, grid, same);
        failures += !same;
        /* Baseline after the first float log, which allocates newlib dtoa state once. */
        if (cycle == 0)
            heap_loaded = mallinfo();
        char line1[48], line2[48];
        if (cycle < CHECK_CYCLES) {
            for (unsigned f = 0; f < 90; ++f) {
                Mtx view;
                camera(&section, 0.8f + f * 0.02f, 0.55f, section.radius * 1.6f, view);
                snprintf(line1, sizeof(line1), "%u-%u-%" PRIu32, cycle, f, manifest.surfaces);
                snprintf(line2, sizeof(line2), "%u-%08" PRIx32, visible, grid);
                present(&section, &manifest, mode, &frame, view, line1, line2);
            }
        } else {
            /* Interactive: stick orbits, C-stick zooms, B resets, START or HOME exits. */
            f32 yaw = 0.8f, pitch = 0.55f, distance = section.radius * 1.6f, input_yaw = 0, input_zoom = 0;
            const char *exit_reason = "auto_exit";
            unsigned activity = 0, resets = 0, stick_seen = 0;
            for (unsigned f = 0; SYS_MainLoop(); ++f) {
                PAD_ScanPads();
                WPAD_ScanPads();
                s8 sx = PAD_StickX(0), sy = PAD_StickY(0), cy = PAD_SubStickY(0);
                u32 down = PAD_ButtonsDown(0), wdown = WPAD_ButtonsDown(0), wheld = WPAD_ButtonsHeld(0);
                f32 dyaw = (abs(sx) > 12 ? sx : 0) * 0.0006f + (wheld & WPAD_BUTTON_RIGHT ? 0.02f : 0) -
                           (wheld & WPAD_BUTTON_LEFT ? 0.02f : 0);
                f32 dpitch = (abs(sy) > 12 ? sy : 0) * 0.0004f;
                f32 dzoom = (abs(cy) > 12 ? -cy : 0) * section.radius * 0.0002f;
                activity |= sx || sy || cy || down || wdown;
                stick_seen |= abs(sx) > 12 || abs(cy) > 12;
                yaw += dyaw;
                pitch = fminf(1.45f, fmaxf(-0.2f, pitch + dpitch));
                f32 next = fminf(section.radius * 8, fmaxf(section.radius * 0.6f, distance + dzoom));
                input_yaw += fabsf(dyaw);
                input_zoom += fabsf(next - distance);
                distance = next;
                if ((down & PAD_BUTTON_B) || (wdown & WPAD_BUTTON_B)) {
                    yaw = 0.8f;
                    pitch = 0.55f;
                    distance = section.radius * 1.6f;
                    ++resets;
                }
                /* START counts only after the camera has been driven, so the
                 * repeating input script cannot end the scene before it is used. */
                if ((down & PAD_BUTTON_START) && stick_seen && f >= 240) { exit_reason = "pad_start"; break; }
                if (wdown & WPAD_BUTTON_HOME) { exit_reason = "remote_home"; break; }
#if WII_PROBE_AUTO_EXIT_FRAMES > 0
                if (f >= WII_PROBE_AUTO_EXIT_FRAMES) break;
#endif
                Mtx view;
                camera(&section, yaw, pitch, distance, view);
                snprintf(line1, sizeof(line1), "%u-%u-%" PRIu32, cycle, f, manifest.surfaces);
                snprintf(line2, sizeof(line2), "%x-%d-%d", (unsigned)PAD_ButtonsHeld(0),
                         (int)lroundf(fmodf(yaw * 57.29578f, 360.0f)), (int)lroundf(distance));
                present(&section, &manifest, mode, &frame, view, line1, line2);
            }
            view_log("INTERACTIVE activity=%u stick_seen=%u input_yaw=%.3f input_zoom=%.2f resets=%u exit=%s\n",
                     activity, stick_seen, input_yaw, input_zoom, resets, exit_reason);
        }
        /* The GP has finished with the arrays before their backing is released. */
        GX_DrawDone();
        if (!release_section(&section, arena_lo)) {
            view_log("FAIL release cycle=%u\n", cycle);
            ++failures;
            break;
        }
        int rejected_stale = stale_rejects(&section);
        stale += rejected_stale;
        failures += !rejected_stale;
        ++cycles;
        if (record != NULL && fflush(record) != 0)
            log_failed = 1;
    }
    /* Faulted inputs must reject, release cleanly and leave MEM2 as found. */
    unsigned faults_rejected = 0;
    if (manifest_ok && !failures) {
        const enum fault faults[] = {FAULT_BSP_CRC, FAULT_TAG_LENGTH};
        const char *expected[] = {"crc", "trailing"};
        for (unsigned i = 0; i < 2; ++i) {
            struct section section;
            memset(&section, 0, sizeof(section));
            const char *failure = "none";
            int loaded = load_section(&manifest, &section, 0, faults[i], arena_lo, arena_hi, &failure);
            int clean = !loaded && strcmp(failure, expected[i]) == 0 &&
                        (uintptr_t)SYS_GetArena2Lo() == arena_lo && !section.owner.live;
            if (loaded)
                release_section(&section, arena_lo);
            faults_rejected += clean;
            view_log("FAULT kind=%s reason=%s clean=%d\n", i ? "tag_length_minus_one" : "bsp_crc_flip", failure, clean);
        }
        failures += faults_rejected != 2;
    }
    struct mallinfo heap_end = mallinfo();
    view_log("MEMORY mem2_bytes=%lu plan_required_max=%lu mem2_unplanned=%lu heap_start=%d heap_loaded=%d heap_end=%d"
             " mem2_restored=%d state_sound=placeholder_reservations\n",
             (unsigned long)(arena_hi - arena_lo), (unsigned long)max_required,
             (unsigned long)(arena_hi - arena_lo - max_required), heap_start.uordblks, heap_loaded.uordblks,
             heap_end.uordblks, (uintptr_t)SYS_GetArena2Lo() == arena_lo);
    view_log("GX fifo_bytes=%u fifo_submitted_peak=%u render_max_us=%" PRIu64 " frames=%u\n",
             FIFO_BYTES, (unsigned)frame.fifo_peak, frame.render_max_us, frame.frames);
    view_log("STACK depth_sampled=%u scope=frame_address_samples_not_peak\n", (unsigned)(stack_entry - stack_lowest));
    int success = !failures && manifest_ok && heap_end.uordblks == heap_loaded.uordblks && cycles == CHECK_CYCLES + 1 && stale == cycles &&
                  faults_rejected == 2 && (uintptr_t)SYS_GetArena2Lo() == arena_lo;
    view_log("END target=geometry_view build=%s cycles=%u stale_rejected=%u faults_rejected=%u failures=%d result=%s\n",
             WII_BUILD_ID, cycles, stale, faults_rejected, failures, success ? "pass" : "fail");
    if (record != NULL && (fclose(record) != 0 || log_failed))
        return 1;
    return success ? 0 : 1;
}
