/* Asset-free native GX diagnostic scene (HWI-014A). Authored geometry only;
 * this is not a Halo renderer, engine integration or gameplay result. */
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

_Static_assert(sizeof(void *) == 4, "The scene requires PPC32 pointers");

#define FIFO_BYTES (256u * 1024u)
#define GX_ALIGN 32u
#define MAX_OWNED 8
#define SCENE_DIR "sd:/halo-wii-gx"
#define RUNS_PATH SCENE_DIR "/runs.txt"
#define LOG_PATH SCENE_DIR "/scene.log"
#define RUNS_PREFIX "halo-wii-gx-v1 "
/* Processor-interface CPU FIFO write pointer (physical address bits). */
#define PI_FIFO_WRITE_POINTER (*(volatile u32 *)0xCC003014)
#define PI_FIFO_ADDRESS_MASK 0x03FFFFE0u

/* Face colours double as the expected values for EFB self-checks. */
enum { FACE_FRONT, FACE_BACK, FACE_RIGHT, FACE_LEFT, FACE_TOP, FACE_BOTTOM, FACE_COUNT };
static const GXColor face_colours[FACE_COUNT] = {
    {224, 48, 48, 255}, {48, 200, 200, 255}, {240, 140, 32, 255},
    {200, 48, 200, 255}, {232, 224, 48, 255}, {96, 64, 200, 255},
};
static const GXColor clear_colour = {24, 24, 32, 255};
static const GXColor backdrop_colour = {32, 64, 224, 255};
static const GXColor cull_colour = {240, 240, 240, 255};

/* Corner i has x=bit0, y=bit1, z=bit2. Each face is clockwise when viewed
 * from outside, which libogc documents as front-facing. */
static const u8 face_corners[FACE_COUNT][4] = {
    {6, 7, 5, 4}, {3, 2, 0, 1}, {7, 3, 1, 5}, {2, 6, 4, 0}, {2, 3, 7, 6}, {4, 5, 1, 0},
};

struct owned_range {
    const char *name;
    uintptr_t base;
    u32 bytes;
    u32 align;
    const char *cache;
};

static struct owned_range owned[MAX_OWNED];
static unsigned owned_count;
static FILE *record;
static int log_failed;
static int range_failures;
static uintptr_t stack_entry, stack_lowest;

static void scene_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void scene_log(const char *format, ...)
{
    va_list args;
    if (record == NULL)
        return;
    va_start(args, format);
    if (vfprintf(record, format, args) < 0)
        log_failed = 1;
    va_end(args);
}

/* Not inlined, so each sample is the frame of a real call from that depth. */
__attribute__((noinline)) static void sample_stack(void)
{
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (here < stack_lowest)
        stack_lowest = here;
}

static const char *region_of(uintptr_t physical, u32 bytes)
{
    if (physical + bytes < physical)
        return NULL;
    if (physical + bytes <= 0x01800000u)
        return "MEM1";
    if (physical >= 0x10000000u && physical + bytes <= 0x14000000u)
        return "MEM2";
    return NULL;
}

/* Registers a CPU-visible buffer that GX or VI reads. Rejects null, misalignment,
 * ranges outside MEM1/MEM2 and physical overlap with an earlier owned range. */
static int own(const char *name, const void *pointer, u32 bytes, u32 align, const char *cache)
{
    uintptr_t base = (uintptr_t)pointer;
    uintptr_t physical = (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(pointer);
    const char *region = pointer == NULL ? NULL : region_of(physical, bytes);
    if (pointer == NULL || bytes == 0 || base % align != 0 || region == NULL || owned_count == MAX_OWNED) {
        ++range_failures;
        scene_log("INVALID_RANGE name=%s base=%p bytes=%u align=%u\n", name, pointer, bytes, align);
        return 0;
    }
    for (unsigned i = 0; i < owned_count; ++i) {
        uintptr_t other = (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL((void *)owned[i].base);
        if (physical < other + owned[i].bytes && other < physical + bytes) {
            ++range_failures;
            scene_log("INVALID_RANGE name=%s overlaps=%s\n", name, owned[i].name);
            return 0;
        }
    }
    owned[owned_count++] = (struct owned_range){name, base, bytes, align, cache};
    scene_log("OWN name=%s base=%p physical=0x%08x bytes=%u align=%u region=%s cache=%s\n",
              name, pointer, (unsigned)physical, bytes, align, region, cache);
    return 1;
}

/* GX array pointers must stay inside a registered range. */
static int owned_contains(const void *pointer, u32 bytes)
{
    uintptr_t base = (uintptr_t)pointer;
    for (unsigned i = 0; i < owned_count; ++i)
        if (base >= owned[i].base && base + bytes <= owned[i].base + owned[i].bytes)
            return 1;
    ++range_failures;
    scene_log("INVALID_RANGE unowned_array=%p bytes=%u\n", pointer, bytes);
    return 0;
}

static int read_runs(unsigned *previous)
{
    FILE *file = fopen(RUNS_PATH, "r");
    *previous = 0;
    if (file == NULL)
        return errno == ENOENT ? 1 : 0;
    char line[64];
    char *end;
    int valid = fgets(line, sizeof(line), file) != NULL &&
                strncmp(line, RUNS_PREFIX, sizeof(RUNS_PREFIX) - 1) == 0;
    unsigned long count = 0;
    if (valid) {
        const char *digits = line + sizeof(RUNS_PREFIX) - 1;
        valid = *digits >= '0' && *digits <= '9';
        if (valid) {
            errno = 0;
            count = strtoul(digits, &end, 10);
            valid = errno == 0 && count < UINT32_MAX && strcmp(end, "\n") == 0;
        }
    }
    if (fgetc(file) != EOF || ferror(file))
        valid = 0;
    if (fclose(file) != 0 || !valid)
        return 0;
    *previous = (unsigned)count;
    return 1;
}

/* Returns 1 with persistent logging, 0 when SD is absent, -1 on a storage error.
 * An invalid run counter is preserved rather than overwritten. */
static int init_storage(unsigned *previous)
{
    if (!fatInitDefault())
        return 0;
    if (mkdir(SCENE_DIR, 0777) != 0 && errno != EEXIST)
        return -1;
    if (!read_runs(previous))
        return -1;
    FILE *file = fopen(RUNS_PATH, "w");
    if (file == NULL)
        return -1;
    int write_result = fprintf(file, RUNS_PREFIX "%u\n", *previous + 1);
    if (fclose(file) != 0 || write_result < 0)
        return -1;
    unsigned readback;
    if (!read_runs(&readback) || readback != *previous + 1)
        return -1;
    record = fopen(LOG_PATH, "a");
    return record == NULL ? -1 : 1;
}

static void set_direct_desc(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

static void set_indexed_desc(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
}

static void quad(const f32 corners[4][3], GXColor colour)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; ++i) {
        GX_Position3f32(corners[i][0], corners[i][1], corners[i][2]);
        GX_Color4u8(colour.r, colour.g, colour.b, colour.a);
    }
    GX_End();
}

static void draw_cube(const Mtx view, const Mtx model)
{
    sample_stack();
    Mtx modelview;
    guMtxConcat(view, model, modelview);
    GX_LoadPosMtxImm(modelview, GX_PNMTX0);
    set_indexed_desc();
    GX_Begin(GX_QUADS, GX_VTXFMT0, FACE_COUNT * 4);
    for (int face = 0; face < FACE_COUNT; ++face)
        for (int corner = 0; corner < 4; ++corner) {
            GX_Position1x8(face_corners[face][corner]);
            GX_Color1x8(face);
        }
    GX_End();
}

static void load_identity_world(const Mtx view)
{
    GX_LoadPosMtxImm((MtxP)view, GX_PNMTX0);
    set_direct_desc();
}

/* A checkered floor below the cube; each tile is clockwise viewed from above. */
static void draw_floor(const Mtx view)
{
    load_identity_world(view);
    for (int row = 0; row < 8; ++row)
        for (int column = 0; column < 8; ++column) {
            f32 x0 = -4.0f + column, z0 = -4.0f + row;
            const f32 tile[4][3] = {{x0, -1.6f, z0}, {x0 + 1, -1.6f, z0},
                                    {x0 + 1, -1.6f, z0 + 1}, {x0, -1.6f, z0 + 1}};
            u8 shade = (row + column) % 2 ? 70 : 110;
            quad(tile, (GXColor){shade, shade, shade, 255});
        }
}

/* 3x5 glyphs, rows top to bottom with bit 2 as the left column. */
static u16 glyph(char c)
{
    static const u16 digits[16] = {
        075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111,
        075757, 075717, 075755, 065656, 074447, 065556, 074747, 074744,
    };
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    if (c >= 'a' && c <= 'f')
        return digits[c - 'a' + 10];
    if (c == '-')
        return 000700;
    return 0;
}

static void draw_text(const char *text, f32 x, f32 y, f32 cell, GXColor colour)
{
    sample_stack();
    u32 lit = 0;
    for (const char *c = text; *c; ++c)
        lit += __builtin_popcount(glyph(*c));
    if (lit == 0)
        return;
    GX_Begin(GX_QUADS, GX_VTXFMT0, lit * 4);
    for (const char *c = text; *c; ++c, x += cell * 4) {
        u16 bits = glyph(*c);
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column) {
                if (!(bits & (1u << (14 - row * 3 - column))))
                    continue;
                f32 left = x + column * cell, top = y + row * cell;
                const f32 cellq[4][2] = {{left, top}, {left + cell, top},
                                         {left + cell, top + cell}, {left, top + cell}};
                for (int i = 0; i < 4; ++i) {
                    GX_Position3f32(cellq[i][0], cellq[i][1], -0.5f);
                    GX_Color4u8(colour.r, colour.g, colour.b, colour.a);
                }
            }
    }
    GX_End();
}

static void make_model(Mtx model, f32 yaw, f32 pitch, f32 scale, f32 tx, f32 ty, f32 tz)
{
    Mtx yaw_m, pitch_m;
    guMtxRotRad(yaw_m, 'y', yaw);
    guMtxRotRad(pitch_m, 'x', pitch);
    guMtxConcat(yaw_m, pitch_m, model);
    guMtxScaleApply(model, model, scale, scale, scale);
    guMtxTransApply(model, model, tx, ty, tz);
}

struct calibration {
    const char *name;
    f32 yaw, pitch;
    u8 cull;
    u8 depth;
    u8 cull_quad;
    f32 probe_x, probe_y;
    GXColor expected;
};

static int colour_near(GXColor a, GXColor b)
{
    return abs(a.r - b.r) <= 12 && abs(a.g - b.g) <= 12 && abs(a.b - b.b) <= 12;
}

/* Renders one deterministic pose into the EFB, then reads pixels back from
 * the CPU. Controls disable culling or depth so each check must discriminate. */
static int run_calibration(const struct calibration *test, const Mtx view, const GXRModeObj *mode,
                           void *scratch_xfb)
{
    sample_stack();
    Mtx model;
    GX_SetCullMode(test->cull);
    GX_SetZMode(test->depth, GX_LEQUAL, test->depth);
    make_model(model, test->yaw, test->pitch, 1.0f, 0, 0, 0);
    draw_cube(view, model);
    load_identity_world(view);
    const f32 backdrop[4][3] = {{-4, 3, -3}, {4, 3, -3}, {4, -3, -3}, {-4, -3, -3}};
    quad(backdrop, backdrop_colour);
    if (test->cull_quad) {
        /* Counter-clockwise to the viewer: back-facing in front of the cube. */
        const f32 near_quad[4][3] = {{-0.3f, 0.3f, 2}, {-0.3f, -0.3f, 2}, {0.3f, -0.3f, 2}, {0.3f, 0.3f, 2}};
        quad(near_quad, cull_colour);
    }
    GX_DrawDone();
    u16 x = (u16)(test->probe_x * mode->fbWidth), y = (u16)(test->probe_y * mode->efbHeight);
    GXColor seen;
    u32 z = 0;
    GX_PeekARGB(x, y, &seen);
    GX_PeekZ(x, y, &z);
    int pass = colour_near(seen, test->expected);
    scene_log("CAL name=%s pixel=%u,%u expected=%02x%02x%02x observed=%02x%02x%02x%02x z=%06x pass=%d\n",
              test->name, x, y, test->expected.r, test->expected.g, test->expected.b,
              seen.r, seen.g, seen.b, seen.a, (unsigned)z, pass);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_CopyDisp(scratch_xfb, GX_TRUE);
    GX_DrawDone();
    return pass;
}

static int calibrate(const GXRModeObj *mode, void *scratch_xfb, u32 *z_center, u32 *z_backdrop)
{
    Mtx view;
    guVector camera = {0, 0, 6}, up = {0, 1, 0}, target = {0, 0, 0};
    guLookAt(view, &camera, &up, &target);
    const f32 deg30 = 0.52359878f;
    const struct calibration tests[] = {
        {"front_depth_cull", 0, 0, GX_CULL_BACK, GX_TRUE, 1, 0.5f, 0.5f, face_colours[FACE_FRONT]},
        {"backdrop_visible", 0, 0, GX_CULL_BACK, GX_TRUE, 1, 0.25f, 0.5f, backdrop_colour},
        {"clear_corner", 0, 0, GX_CULL_BACK, GX_TRUE, 1, 0.0125f, 0.0167f, clear_colour},
        {"control_cull_none", 0, 0, GX_CULL_NONE, GX_TRUE, 1, 0.5f, 0.5f, cull_colour},
        {"control_depth_off", 0, 0, GX_CULL_BACK, GX_FALSE, 0, 0.5f, 0.5f, backdrop_colour},
        {"pitch_top_visible", 0, deg30, GX_CULL_BACK, GX_TRUE, 0, 0.5f, 0.396f, face_colours[FACE_TOP]},
        {"pitch_front_lower", 0, deg30, GX_CULL_BACK, GX_TRUE, 0, 0.5f, 0.604f, face_colours[FACE_FRONT]},
        {"yaw_left_visible", deg30, 0, GX_CULL_BACK, GX_TRUE, 0, 0.405f, 0.5f, face_colours[FACE_LEFT]},
    };
    int passed = 0;
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        passed += run_calibration(&tests[i], view, mode, scratch_xfb);
        if (i == 0) {
            /* Re-render the first pose to compare cube and backdrop depth samples. */
            Mtx model;
            GX_SetCullMode(GX_CULL_BACK);
            make_model(model, 0, 0, 1.0f, 0, 0, 0);
            draw_cube(view, model);
            load_identity_world(view);
            const f32 backdrop[4][3] = {{-4, 3, -3}, {4, 3, -3}, {4, -3, -3}, {-4, -3, -3}};
            quad(backdrop, backdrop_colour);
            GX_DrawDone();
            GX_PeekZ(mode->fbWidth / 2, mode->efbHeight / 2, z_center);
            GX_PeekZ(mode->fbWidth / 4, mode->efbHeight / 2, z_backdrop);
            GX_CopyDisp(scratch_xfb, GX_TRUE);
            GX_DrawDone();
        }
    }
    int depth_order = *z_center < *z_backdrop;
    scene_log("CAL name=depth_order z_cube=%06x z_backdrop=%06x pass=%d\n",
              (unsigned)*z_center, (unsigned)*z_backdrop, depth_order);
    return passed + depth_order;
}

struct timing {
    u64 render_min, render_max, render_sum;
    u64 interval_min, interval_max;
    u32 samples;
};

int main(void)
{
    stack_entry = stack_lowest = (uintptr_t)__builtin_frame_address(0);
    VIDEO_Init();
    PAD_Init();
    WPAD_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL)
        return 1;
    struct mallinfo heap_start = mallinfo();
    void *arena1_lo = SYS_GetArena1Lo(), *arena2_lo = SYS_GetArena2Lo();

    unsigned previous_runs = 0;
    int storage = init_storage(&previous_runs);
    scene_log("BEGIN target=gx_scene build=%s previous_runs=%u storage=%d video=%ux%u efb_height=%u mode=%u aa=%u\n",
              WII_BUILD_ID, previous_runs, storage, mode->fbWidth, mode->xfbHeight, mode->efbHeight,
              mode->viTVMode, mode->aa);
    scene_log("ARENAS mem1=%p..%p bytes=%u mem2=%p..%p bytes=%u\n",
              SYS_GetArena1Lo(), SYS_GetArena1Hi(), SYS_GetArena1Size(),
              SYS_GetArena2Lo(), SYS_GetArena2Hi(), SYS_GetArena2Size());

    /* FIFO: zeroed through the cache, then flushed and invalidated so no dirty
     * line can later be written back over commands the GP is reading. */
    void *fifo = memalign(GX_ALIGN, FIFO_BYTES);
    u32 xfb_bytes = VIDEO_GetFrameBufferSize(mode);
    void *xfb_cached[2] = {SYS_AllocateFramebuffer(mode), SYS_AllocateFramebuffer(mode)};
    /* Cube arrays: 8 positions x 3 f32 and 6 RGBA8 colours, rounded to 32 bytes. */
    const u32 position_bytes = 128, colour_bytes = 32;
    f32 *positions = memalign(GX_ALIGN, position_bytes);
    GXColor *colours = memalign(GX_ALIGN, colour_bytes);
    if (fifo == NULL || xfb_cached[0] == NULL || xfb_cached[1] == NULL || positions == NULL || colours == NULL) {
        scene_log("ALLOC_FAIL fifo=%p xfb0=%p xfb1=%p positions=%p colours=%p\n",
                  fifo, xfb_cached[0], xfb_cached[1], positions, colours);
        if (record != NULL)
            fclose(record);
        return 1;
    }
    own("gx_fifo", fifo, FIFO_BYTES, GX_ALIGN, "zeroed_then_DCFlushRange_GP_reads");
    own("xfb0", xfb_cached[0], xfb_bytes, GX_ALIGN, "K1_uncached_CPU_never_writes_GX_copy_target");
    own("xfb1", xfb_cached[1], xfb_bytes, GX_ALIGN, "K1_uncached_CPU_never_writes_GX_copy_target");
    own("cube_positions", positions, position_bytes, GX_ALIGN, "CPU_filled_then_DCFlushRange_and_GX_InvVtxCache");
    own("cube_colours", colours, colour_bytes, GX_ALIGN, "CPU_filled_then_DCFlushRange_and_GX_InvVtxCache");
    void *xfb[2] = {MEM_K0_TO_K1(xfb_cached[0]), MEM_K0_TO_K1(xfb_cached[1])};

    memset(fifo, 0, FIFO_BYTES);
    DCFlushRange(fifo, FIFO_BYTES);
    memset(positions, 0, position_bytes);
    memset(colours, 0, colour_bytes);
    for (int corner = 0; corner < 8; ++corner) {
        positions[corner * 3 + 0] = corner & 1 ? 1.0f : -1.0f;
        positions[corner * 3 + 1] = corner & 2 ? 1.0f : -1.0f;
        positions[corner * 3 + 2] = corner & 4 ? 1.0f : -1.0f;
    }
    memcpy(colours, face_colours, sizeof(face_colours));
    DCFlushRange(positions, position_bytes);
    DCFlushRange(colours, colour_bytes);

    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    GX_Init(fifo, FIFO_BYTES);
    GX_SetCopyClear(clear_colour, GX_MAX_Z24);
    f32 y_scale = GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight);
    u32 copy_lines = GX_SetDispCopyYScale(y_scale);
    GX_SetViewport(0, 0, mode->fbWidth, mode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
    GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
    GX_SetDispCopyDst(mode->fbWidth, copy_lines);
    GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
    GX_SetFieldMode(mode->field_rendering, mode->viHeight == 2 * mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(mode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDither(GX_FALSE);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);

    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    int arrays_valid = owned_contains(positions, 8 * 3 * sizeof(f32)) &&
                       owned_contains(colours, FACE_COUNT * sizeof(GXColor));
    GX_SetArray(GX_VA_POS, positions, 3 * sizeof(f32));
    GX_SetArray(GX_VA_CLR0, colours, sizeof(GXColor));
    GX_InvVtxCache();
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

    Mtx44 perspective, ortho;
    f32 aspect = (f32)mode->fbWidth / mode->efbHeight;
    guPerspective(perspective, 60, aspect, 0.1f, 300.0f);
    guOrtho(ortho, 0, mode->efbHeight, 0, mode->fbWidth, 0, 1);
    GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);

    GX_CopyDisp(xfb[1], GX_TRUE);
    GX_DrawDone();
    u32 z_center = 0, z_backdrop = 0;
    const int calibration_total = 9;
    int calibration_passed = calibrate(mode, xfb[1], &z_center, &z_backdrop);
    /* The first %f allocates newlib's dtoa state; take it before the heap baseline. */
    scene_log("PROJECTION fov_y=60 aspect=%.4f near=0.1 far=300 camera_z=6\n", aspect);
    struct mallinfo heap_ready = mallinfo();
    scene_log("CALIBRATION passed=%d total=%d scope=EFB_readback_in_stock_Dolphin_not_hardware\n",
              calibration_passed, calibration_total);
    scene_log("HEAP before=%d after_init=%d owned_ranges=%u range_failures=%d arena1_lo_moved=%d arena2_lo_moved=%d\n",
              heap_start.uordblks, heap_ready.uordblks, owned_count, range_failures,
              (int)((char *)SYS_GetArena1Lo() - (char *)arena1_lo), (int)((char *)SYS_GetArena2Lo() - (char *)arena2_lo));
    if (record != NULL && fflush(record) != 0)
        log_failed = 1;

    f32 yaw = 0.35f, pitch = 0.3f, distance = 6.0f, orbit = 0;
    int auto_spin = 0;
    unsigned frames = 0, toggles = 0, resets = 0, first_input_frame = 0;
    unsigned connected = 0, activity = 0;
    f32 input_yaw = 0, input_pitch = 0, input_zoom = 0;
    u32 fifo_peak = 0;
    u64 fifo_total = 0;
    const u32 fifo_physical = (u32)MEM_VIRTUAL_TO_PHYSICAL(fifo);
    struct timing timing = {UINT64_MAX, 0, 0, UINT64_MAX, 0, 0};
    const char *exit_reason = "system_event";
    char line[40];
    int fb = 0;
    u64 previous_frame = gettime();
    const GXColor hud_build = {255, 220, 64, 255}, hud_counter = {240, 240, 240, 255},
                  hud_input = {64, 220, 255, 255};
    const f32 radians_per_unit = 0.0012f;

    while (SYS_MainLoop()) {
        u64 frame_start = gettime();
        u32 write_before = PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK;
        u32 present = PAD_ScanPads();
        WPAD_ScanPads();
        connected |= present;
        s8 stick_x = PAD_StickX(0), stick_y = PAD_StickY(0), c_y = PAD_SubStickY(0);
        u32 down = PAD_ButtonsDown(0), held = PAD_ButtonsHeld(0);
        u32 wdown = WPAD_ButtonsDown(0), wheld = WPAD_ButtonsHeld(0);
        f32 yaw_step = (abs(stick_x) > 12 ? stick_x : 0) * radians_per_unit;
        f32 pitch_step = (abs(stick_y) > 12 ? stick_y : 0) * radians_per_unit;
        f32 zoom_step = (abs(c_y) > 12 ? -c_y : 0) * 0.0008f;
        if (wheld & WPAD_BUTTON_RIGHT) yaw_step += 0.03f;
        if (wheld & WPAD_BUTTON_LEFT) yaw_step -= 0.03f;
        if (wheld & WPAD_BUTTON_UP) pitch_step += 0.03f;
        if (wheld & WPAD_BUTTON_DOWN) pitch_step -= 0.03f;
        if (wheld & WPAD_BUTTON_PLUS) zoom_step -= 0.05f;
        if (wheld & WPAD_BUTTON_MINUS) zoom_step += 0.05f;
        if (held || stick_x || stick_y || c_y || wheld) {
            activity |= 1;
            if (!first_input_frame)
                first_input_frame = frames;
        }
        yaw += yaw_step;
        pitch += pitch_step;
        f32 new_distance = fminf(14.0f, fmaxf(3.0f, distance + zoom_step));
        input_yaw += fabsf(yaw_step);
        input_pitch += fabsf(pitch_step);
        input_zoom += fabsf(new_distance - distance);
        distance = new_distance;
        if ((down & PAD_BUTTON_A) || (wdown & WPAD_BUTTON_A)) {
            auto_spin = !auto_spin;
            ++toggles;
            scene_log("INPUT frame=%u auto_spin=%d\n", frames, auto_spin);
        }
        if ((down & PAD_BUTTON_B) || (wdown & WPAD_BUTTON_B)) {
            yaw = 0.35f;
            pitch = 0.3f;
            distance = 6.0f;
            ++resets;
            scene_log("INPUT frame=%u reset_pose=1\n", frames);
        }
        if (down & PAD_BUTTON_START)
            exit_reason = "pad_start";
        if (wdown & WPAD_BUTTON_HOME)
            exit_reason = "remote_home";
        if (strcmp(exit_reason, "system_event") != 0)
            break;
#if WII_PROBE_AUTO_EXIT_FRAMES > 0
        if (frames >= WII_PROBE_AUTO_EXIT_FRAMES) {
            exit_reason = "auto_exit";
            break;
        }
#endif
        if (auto_spin)
            yaw += 0.02f;
        orbit += 0.025f;

        Mtx view, model;
        guVector camera = {0, 1.2f, distance}, up = {0, 1, 0}, target = {0, 0, 0};
        guLookAt(view, &camera, &up, &target);
        GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);
        GX_SetCullMode(GX_CULL_BACK);
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        draw_floor(view);
        make_model(model, yaw, pitch, 1.0f, 0, 0, 0);
        draw_cube(view, model);
        /* The satellite clips the main cube's corners so depth intersection is visible. */
        make_model(model, orbit * 2, orbit, 0.35f, cosf(orbit) * 1.25f, 0.5f, sinf(orbit) * 1.25f);
        draw_cube(view, model);

        Mtx identity;
        guMtxIdentity(identity);
        GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
        GX_LoadPosMtxImm(identity, GX_PNMTX0);
        set_direct_desc();
        GX_SetCullMode(GX_CULL_NONE);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        draw_text(WII_BUILD_ID, 24, 24, 4, hud_build);
        snprintf(line, sizeof(line), "%u-%d", frames, calibration_passed);
        draw_text(line, 24, 52, 4, hud_counter);
        snprintf(line, sizeof(line), "%04x-%x-%d", (unsigned)(held | (wheld & 0xffff)), (unsigned)present,
                 (int)lroundf(fmodf(yaw * 57.29578f, 360.0f)));
        draw_text(line, 24, 80, 4, hud_input);

        GX_DrawDone();
        /* Bytes the CPU submitted this frame, modulo the ring; valid while one
         * frame stays below the FIFO size (the GP drains concurrently). */
        u32 write_after = PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK;
        u32 submitted = (write_after - write_before + FIFO_BYTES) % FIFO_BYTES;
        if (write_before < fifo_physical || write_before >= fifo_physical + FIFO_BYTES) {
            ++range_failures;
            scene_log("INVALID_RANGE fifo_write_pointer=0x%08x\n", (unsigned)write_before);
        }
        if (submitted > fifo_peak)
            fifo_peak = submitted;
        fifo_total += submitted;
        u64 rendered = gettime();
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GX_SetColorUpdate(GX_TRUE);
        GX_CopyDisp(xfb[fb], GX_TRUE);
        GX_DrawDone();
        VIDEO_SetNextFramebuffer(xfb[fb]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        fb ^= 1;

        u64 render_us = ticks_to_microsecs(rendered - frame_start);
        u64 interval_us = ticks_to_microsecs(frame_start - previous_frame);
        previous_frame = frame_start;
        if (render_us < timing.render_min) timing.render_min = render_us;
        if (render_us > timing.render_max) timing.render_max = render_us;
        timing.render_sum += render_us;
        if (frames > 0) {
            if (interval_us < timing.interval_min) timing.interval_min = interval_us;
            if (interval_us > timing.interval_max) timing.interval_max = interval_us;
        }
        ++timing.samples;
        if (frames % 120 == 0)
            scene_log("STATE frame=%u yaw=%.3f pitch=%.3f distance=%.2f auto_spin=%d held=%04x stick=%d,%d cstick_y=%d\n",
                      frames, yaw, pitch, distance, auto_spin, (unsigned)held, stick_x, stick_y, c_y);
        ++frames;
    }

    struct mallinfo heap_end = mallinfo();
    scene_log("FIFO bytes=%u submitted_per_frame_peak=%u avg=%u scope=PI_write_pointer_delta_scene_and_HUD_excluding_copy\n",
              FIFO_BYTES, (unsigned)fifo_peak, timing.samples ? (unsigned)(fifo_total / timing.samples) : 0);
    scene_log("TIMING frames=%u render_us_min=%" PRIu64 " max=%" PRIu64 " avg=%" PRIu64
              " interval_us_min=%" PRIu64 " max=%" PRIu64 " scope=CPU_submit_plus_GX_DrawDone\n",
              timing.samples, timing.samples ? timing.render_min : 0, timing.render_max,
              timing.samples ? timing.render_sum / timing.samples : 0,
              timing.samples > 1 ? timing.interval_min : 0, timing.interval_max);
    scene_log("STACK entry=0x%08x lowest_sampled=0x%08x depth_sampled=%u scope=frame_address_samples_not_peak\n",
              (unsigned)stack_entry, (unsigned)stack_lowest, (unsigned)(stack_entry - stack_lowest));
    scene_log("HEAP end=%d after_init=%d leak_since_init=%d\n",
              heap_end.uordblks, heap_ready.uordblks, heap_end.uordblks - heap_ready.uordblks);
    int success = calibration_passed == calibration_total && range_failures == 0 && arrays_valid &&
                  storage >= 0 && heap_end.uordblks == heap_ready.uordblks;
    scene_log("END target=gx_scene build=%s frames=%u connected=%x activity=%x first_input_frame=%u "
              "input_yaw=%.3f input_pitch=%.3f input_zoom=%.3f toggles=%u resets=%u exit=%s storage=%d "
              "calibration=%d/%d range_failures=%d result=%s\n",
              WII_BUILD_ID, frames, connected, activity, first_input_frame, input_yaw, input_pitch, input_zoom,
              toggles, resets, exit_reason, storage, calibration_passed, calibration_total, range_failures,
              success ? "pass" : "fail");
    if (record != NULL && (fclose(record) != 0 || log_failed))
        return 1;
    return success ? 0 : 1;
}
