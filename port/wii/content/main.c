/* Content loader self-test (HWI-008C, HWI-008E).
 *
 * Loads converted content files (HWT1 textures, HRA1 recorded animations,
 * HWS1 sounds; HWM1 models, HWL1 lightmap geometry, HWC1 collision, HMA1
 * model animations, HWF1 fonts and HUS1 text) from the SD card into memory
 * this program owns, validates them with the runtime loaders and checks the
 * decoded contents against SHA-256 digests the host tool wrote
 * (content_check.c). The decoded kinds also log their decoded memory, the
 * heap they held and their load times (MEMORY lines). Textures are
 * also uploaded to GX and read back from the EFB, texel by texel, against
 * the CPU decode. Malformed and truncated inputs must be rejected with the
 * stated reason. Every case runs in two load/check/release cycles, which
 * must agree and leave the heap where it started.
 *
 * The SD card supplies sd:/halo-wii-content/cases.txt and the files it
 * names; the repository holds no game data. This is a diagnostic, not a
 * Halo renderer, mixer or gameplay result. */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "build_id.h"
#include "content_check.h"
#include "hra_animation.h"
#include "hwt_texture.h"

_Static_assert(sizeof(void *) == 4, "The self-test requires PPC32 pointers");

#define CONTENT_DIR "sd:/halo-wii-content"
#define RUNS_PATH CONTENT_DIR "/runs.txt"
#define LOG_PATH CONTENT_DIR "/loaders.log"
#define CASES_PATH CONTENT_DIR "/cases.txt"
#define RUNS_PREFIX "halo-wii-content-v1 "
#define FIFO_BYTES (256u * 1024u)
#define CROP 32u
#define CYCLES 2
#define CMPR_TOLERANCE 8
#define POST_CHECK_FRAMES 3600u

static FILE *record;
static int log_failed;

static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
    va_list args;
    if (record == NULL)
        return;
    va_start(args, format);
    if (vfprintf(record, format, args) < 0)
        log_failed = 1;
    va_end(args);
}

static uint64_t wii_clock(void) { return gettime(); }
static u32 us(u64 ticks) { return (u32)ticks_to_microsecs(ticks); }

/* ---- GX ----------------------------------------------------------------------- */

static GXRModeObj *mode;
static void *xfb[2];
static int fb;
static Mtx44 ortho;
static Mtx identity;

static void gx_state(void)
{
    GX_SetViewport(0, 0, mode->fbWidth, mode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
    GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
}

static void clear_efb(void)
{
    GX_CopyDisp(xfb[fb], GX_TRUE);
    GX_DrawDone();
}

static void show_frame(void)
{
    GX_CopyDisp(xfb[fb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(xfb[fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fb ^= 1;
}

/* One quad of w x h pixels at the origin whose texels map 1:1. */
static void draw_quad(f32 w, f32 h, f32 s1, f32 t1)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(0, 0, 0); GX_Color4u8(255, 255, 255, 255); GX_TexCoord2f32(0, 0);
    GX_Position3f32(w, 0, 0); GX_Color4u8(255, 255, 255, 255); GX_TexCoord2f32(s1, 0);
    GX_Position3f32(w, h, 0); GX_Color4u8(255, 255, 255, 255); GX_TexCoord2f32(s1, t1);
    GX_Position3f32(0, h, 0); GX_Color4u8(255, 255, 255, 255); GX_TexCoord2f32(0, t1);
    GX_End();
}

/* Whether a CMPR texel is one of the two interpolated colours. */
static int cmpr_interpolated(const unsigned char *image, uint32_t width, uint32_t x, uint32_t y)
{
    const unsigned char *tile = image + ((y / 8) * ((width + 7) / 8) + (x / 8)) * 32;
    const unsigned char *block = tile + (((y % 8) / 4) * 2 + ((x % 8) / 4)) * 8;
    uint32_t index = (block[4 + y % 4] >> (6 - 2 * (x % 4))) & 3u;
    return index >= 2;
}

/* Draws the texture object over a crop of the level (lw x lh texels) and
 * compares every texel's colour, then its alpha (shown as grey through
 * GX_CC_TEXA, since the EFB is RGB8), with the CPU decode of image. */
static void gx_compare(GXTexObj *object, uint32_t gx_format, const unsigned char *image, uint32_t lw, uint32_t lh,
                       struct content_readback *rb)
{
    uint32_t w = lw < CROP ? lw : CROP, h = lh < CROP ? lh : CROP;
    for (int pass = 0; pass < 2; ++pass) {
        gx_state();
        GX_LoadTexObj(object, GX_TEXMAP0);
        if (pass == 1) {
            GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXA);
            GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        }
        draw_quad((f32)w, (f32)h, (f32)w / (f32)lw, (f32)h / (f32)lh);
        GX_DrawDone();
        ++rb->draws;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                unsigned char expected[4];
                hwt_decode_texel(gx_format, image, lw, x, y, expected);
                GXColor seen;
                GX_PeekARGB((u16)x, (u16)y, &seen);
                ++rb->peeks;
                unsigned char observed[3] = {seen.r, seen.g, seen.b};
                rb->crc = content_crc32(rb->crc, observed, 3);
                if (pass == 0 && gx_format == HWT_GX_CMPR && expected[3] == 0)
                    continue;   /* the colour of a transparent CMPR texel is unspecified */
                int loose = pass == 0 && gx_format == HWT_GX_CMPR && cmpr_interpolated(image, lw, x, y);
                int tolerance = loose ? CMPR_TOLERANCE : 0, bad = 0;
                for (int c = 0; c < 3; ++c) {
                    int want = pass == 0 ? expected[c] : expected[3];
                    int err = abs((int)observed[c] - want);
                    int channel = pass == 0 ? c : 3;
                    if (err > rb->max_err[channel])
                        rb->max_err[channel] = err;
                    bad |= err > tolerance;
                }
                rb->interpolated += loose ? 1u : 0u;
                if (bad) {
                    if (rb->mismatches < 3)
                        log_line("MISS pass=%d texel=%u,%u expected=%02x%02x%02x%02x observed=%02x%02x%02x\n", pass,
                                 (unsigned)x, (unsigned)y, expected[0], expected[1], expected[2], expected[3],
                                 observed[0], observed[1], observed[2]);
                    ++rb->mismatches;
                }
            }
        clear_efb();
    }
}

/* Control: image 0 drawn as a different GX format with the same bytes per
 * tile must not read back as the CPU decode, which shows the comparison is
 * sensitive. Returns 1 when a mismatch is seen, 0 when none is, -1 when no
 * control applies. A uniform image can legitimately read back the same. */
static int texture_control(const struct content_blob *blob, const struct hwt_header *h)
{
    struct hwt_image image;
    if (hwt_image_at(h, 0, &image) != CONTENT_OK || !image.gx_loadable)
        return -1;
    u8 wrong;
    uint32_t height = image.height;
    switch (h->gx_format) {
    case HWT_GX_CMPR: wrong = GX_TF_I4; break;
    case HWT_GX_I8: wrong = GX_TF_IA4; break;
    case HWT_GX_IA8: wrong = GX_TF_RGB565; break;
    case HWT_GX_RGB565: wrong = GX_TF_RGB5A3; break;
    case HWT_GX_RGB5A3: wrong = GX_TF_RGB565; break;
    case HWT_GX_RGBA8: wrong = GX_TF_RGB565; height = image.height * 2; break; /* same bytes */
    default: return -1;
    }
    if (height > HWT_GX_MAX_DIMENSION)
        return -1;
    GXTexObj object;
    GX_InitTexObj(&object, blob->data + image.offset, (u16)image.width, (u16)height, wrong, GX_CLAMP, GX_CLAMP,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&object, GX_NEAR, GX_NEAR);
    uint32_t w = image.width < CROP ? image.width : CROP, ch = image.height < CROP ? image.height : CROP;
    gx_state();
    GX_LoadTexObj(&object, GX_TEXMAP0);
    draw_quad((f32)w, (f32)ch, (f32)w / (f32)image.width, (f32)ch / (f32)height);
    GX_DrawDone();
    unsigned mismatches = 0;
    for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            unsigned char expected[4];
            hwt_decode_texel(h->gx_format, blob->data + image.offset, image.width, x, y, expected);
            GXColor seen;
            GX_PeekARGB((u16)x, (u16)y, &seen);
            mismatches += seen.r != expected[0] || seen.g != expected[1] || seen.b != expected[2];
        }
    clear_efb();
    return mismatches > 0;
}

/* Every GX-loadable image on its own, then each level of a 2D chain through
 * one mipmapped texture object with its LOD pinned to that level. */
static void texture_readback(const struct content_blob *blob, const struct hwt_header *h, struct content_outcome *o)
{
    DCFlushRange(blob->data, blob->capacity);
    GX_InvalidateTexAll();
    o->control = texture_control(blob, h);
    for (uint32_t i = 0; i < h->images; ++i) {
        struct hwt_image image;
        if (hwt_image_at(h, i, &image) != CONTENT_OK || !image.gx_loadable)
            continue;
        GXTexObj object;
        GX_InitTexObj(&object, blob->data + image.offset, (u16)image.width, (u16)image.height, (u8)h->gx_format,
                      GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&object, GX_NEAR, GX_NEAR);
        gx_compare(&object, h->gx_format, blob->data + image.offset, image.width, image.height, &o->readback);
        ++o->gx_images;
    }
    if (h->type != HWT_TYPE_2D || h->levels < 2 || h->width > HWT_GX_MAX_DIMENSION ||
        h->height > HWT_GX_MAX_DIMENSION)
        return;
    for (uint32_t level = 0; level < h->levels; ++level) {
        struct hwt_image image;
        if (hwt_image_at(h, level, &image) != CONTENT_OK)
            return;
        GXTexObj object;
        GX_InitTexObj(&object, blob->data + HWT_HEADER_BYTES, (u16)h->width, (u16)h->height, (u8)h->gx_format,
                      GX_CLAMP, GX_CLAMP, GX_TRUE);
        GX_InitTexObjLOD(&object, GX_NEAR_MIP_NEAR, GX_NEAR, (f32)level, (f32)level, 0.0f, GX_FALSE, GX_FALSE,
                         GX_ANISO_1);
        gx_compare(&object, h->gx_format, blob->data + image.offset, image.width, image.height, &o->readback);
        ++o->mip_levels;
    }
}

/* ---- cases and totals ----------------------------------------------------------- */

static struct content_case cases[CONTENT_CHECK_MAX_CASES];
static unsigned char *scratch;
static uint32_t scratch_bytes;

/* The largest decoded asset of a kind in one cycle (HWI-008E residency input). */
struct largest {
    char id[24];
    u32 file_bytes, decoded_bytes, units, items;
    int heap_peak, heap_resident;
    u64 load_ticks, decode_ticks, expanded_bytes;
};

struct totals {
    unsigned passed, failed, rejected_as_expected;
    unsigned kind_cases[CONTENT_KIND_COUNT], kind_passed[CONTENT_KIND_COUNT];
    u64 load_ticks[CONTENT_KIND_COUNT], decode_ticks[CONTENT_KIND_COUNT], bytes[CONTENT_KIND_COUNT];
    u64 decoded_bytes[CONTENT_KIND_COUNT], digest_ticks[CONTENT_KIND_COUNT];
    struct largest largest[CONTENT_KIND_COUNT];
    unsigned draws, peeks, mismatches, gx_images, mip_levels, controls, controls_detected;
    int max_err[4];
    u32 crc;
};

static int32_t wii_heap(void) { return (int32_t)mallinfo().uordblks; }

static void run_case(const struct content_case *c, unsigned cycle, struct totals *t)
{
    struct content_outcome o;
    content_run_case(c, scratch, scratch_bytes, texture_readback, wii_clock, wii_heap, &o);
    const struct content_readback *rb = &o.readback;
    log_line("CASE cycle=%u id=%s kind=%s expect=%s got=%s bytes=%u file_sha=%d decoded_match=%d units=%u "
             "items=%u decoded_bytes=%u expanded_bytes=%u heap_peak=%d heap_resident=%d "
             "gx_images=%u mip_levels=%u draws=%u peeks=%u mismatches=%u interpolated=%u max_err=%d,%d,%d,%d "
             "control=%d readback_crc=%08x load_us=%u decode_us=%u digest_us=%u result=%s\n",
             cycle, c->id, content_kind_name(c->kind), content_error_name(c->expect), content_error_name(o.error),
             (unsigned)c->bytes, o.file_ok, o.decoded_match, (unsigned)o.units, (unsigned)o.items,
             (unsigned)o.decoded_bytes, (unsigned)o.expanded_bytes, (int)o.heap_peak, (int)o.heap_resident,
             (unsigned)o.gx_images, (unsigned)o.mip_levels, rb->draws, rb->peeks, rb->mismatches, rb->interpolated,
             rb->max_err[0], rb->max_err[1], rb->max_err[2], rb->max_err[3], o.control, (unsigned)rb->crc,
             us(o.load_ticks), us(o.decode_ticks), us(o.digest_ticks), o.pass ? "pass" : "fail");
    if (c->expect == CONTENT_OK && o.pass && o.decoded_bytes > t->largest[c->kind].decoded_bytes) {
        struct largest *l = &t->largest[c->kind];
        memcpy(l->id, c->id, sizeof(l->id));   /* both char[24], NUL-terminated by the parser */
        l->file_bytes = c->bytes;
        l->decoded_bytes = o.decoded_bytes;
        l->units = o.units;
        l->items = o.items;
        l->heap_peak = o.heap_peak;
        l->heap_resident = o.heap_resident;
        l->load_ticks = o.load_ticks;
        l->decode_ticks = o.decode_ticks;
        l->expanded_bytes = o.expanded_bytes;
    }
    t->kind_cases[c->kind]++;
    if (o.pass) {
        t->passed++;
        t->kind_passed[c->kind]++;
        t->rejected_as_expected += c->expect != CONTENT_OK;
    } else {
        t->failed++;
    }
    if (c->expect == CONTENT_OK) {
        t->load_ticks[c->kind] += o.load_ticks;
        t->decode_ticks[c->kind] += o.decode_ticks;
        t->digest_ticks[c->kind] += o.digest_ticks;
        t->bytes[c->kind] += c->bytes;
        t->decoded_bytes[c->kind] += o.decoded_bytes;
    }
    t->draws += rb->draws;
    t->peeks += rb->peeks;
    t->mismatches += rb->mismatches;
    t->gx_images += o.gx_images;
    t->mip_levels += o.mip_levels;
    if (o.control >= 0) {
        t->controls++;
        t->controls_detected += (unsigned)o.control;
    }
    for (int i = 0; i < 4; ++i)
        if (rb->max_err[i] > t->max_err[i])
            t->max_err[i] = rb->max_err[i];
    unsigned char summary[8] = {(unsigned char)o.pass, (unsigned char)o.error, (unsigned char)(rb->crc >> 24),
                                (unsigned char)(rb->crc >> 16), (unsigned char)(rb->crc >> 8), (unsigned char)rb->crc,
                                (unsigned char)o.decoded_match, (unsigned char)o.file_ok};
    t->crc = content_crc32(t->crc, summary, sizeof(summary));
    t->crc = content_crc32(t->crc, (const unsigned char *)o.decoded, 64);
    if (record != NULL && fflush(record) != 0)
        log_failed = 1;
    show_frame();
}

/* ---- storage -------------------------------------------------------------------- */

static int read_runs(unsigned *count)
{
    FILE *file = fopen(RUNS_PATH, "r");
    if (file == NULL) {
        *count = 0;
        return errno == ENOENT;
    }
    char text[64] = {0};
    size_t got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    unsigned value;
    char tail;
    if (got == 0 || sscanf(text, RUNS_PREFIX "%u%c", &value, &tail) != 2 || tail != '\n')
        return 0;
    *count = value;
    return 1;
}

static int init_storage(unsigned *previous)
{
    if (!fatInitDefault())
        return 0;
    if (mkdir(CONTENT_DIR, 0777) != 0 && errno != EEXIST)
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

static void init_gx(void *fifo)
{
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    GX_Init(fifo, FIFO_BYTES);
    GX_SetCopyClear((GXColor){24, 24, 32, 255}, GX_MAX_Z24);
    f32 y_scale = GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight);
    u32 copy_lines = GX_SetDispCopyYScale(y_scale);
    GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
    GX_SetDispCopyDst(mode->fbWidth, copy_lines);
    GX_SetCopyFilter(GX_FALSE, mode->sample_pattern, GX_FALSE, mode->vfilter);
    GX_SetFieldMode(mode->field_rendering, mode->viHeight == 2 * mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDither(GX_FALSE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_InvVtxCache();
    guOrtho(ortho, 0, mode->efbHeight, 0, mode->fbWidth, 0, 1);
    guMtxIdentity(identity);
    gx_state();
    clear_efb();
}

int main(void)
{
    VIDEO_Init();
    PAD_Init();
    WPAD_Init();
    mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL)
        return 1;
    unsigned previous_runs = 0;
    int storage = init_storage(&previous_runs);
    log_line("BEGIN target=content_loaders build=%s previous_runs=%u storage=%d\n", WII_BUILD_ID, previous_runs,
             storage);
    void *fifo = memalign(32, FIFO_BYTES);
    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    scratch_bytes = HRA_MAX_STREAM_BYTES + 64u * 1024u;
    scratch = memalign(32, scratch_bytes);
    if (fifo == NULL || scratch == NULL) {
        log_line("ALLOC_FAIL fifo=%p scratch=%p\n", fifo, (void *)scratch);
        if (record != NULL)
            fclose(record);
        return 1;
    }
    memset(fifo, 0, FIFO_BYTES);
    DCFlushRange(fifo, FIFO_BYTES);
    init_gx(fifo);

    int count = storage > 0 ? content_read_cases(CASES_PATH, CONTENT_DIR "/", cases, CONTENT_CHECK_MAX_CASES) : -1;
    int cases_ok = count > 0;
    unsigned case_count = cases_ok ? (unsigned)count : 0u;
    log_line("CASES ok=%d count=%u efb=%ux%u crop=%u cycles=%u\n", cases_ok, case_count, mode->fbWidth,
             mode->efbHeight, CROP, CYCLES);
    struct totals totals[CYCLES];
    memset(totals, 0, sizeof(totals));
    int heap_baseline = mallinfo().uordblks, heap_growth = 0;
    for (unsigned cycle = 1; cases_ok && cycle <= CYCLES; ++cycle) {
        struct totals *t = &totals[cycle - 1];
        for (unsigned i = 0; i < case_count; ++i)
            run_case(&cases[i], cycle, t);
        int growth = mallinfo().uordblks - heap_baseline;
        if (growth > heap_growth)
            heap_growth = growth;
        log_line("CYCLE n=%u passed=%u failed=%u rejected_as_expected=%u textures=%u/%u animations=%u/%u "
                 "sounds=%u/%u models=%u/%u lightmaps=%u/%u collisions=%u/%u model_animations=%u/%u fonts=%u/%u "
                 "strings=%u/%u gx_images=%u mip_levels=%u draws=%u peeks=%u mismatches=%u max_err=%d,%d,%d,%d "
                 "controls_detected=%u/%u crc=%08x heap_growth=%d\n",
                 cycle, t->passed, t->failed, t->rejected_as_expected, t->kind_passed[0], t->kind_cases[0],
                 t->kind_passed[1], t->kind_cases[1], t->kind_passed[2], t->kind_cases[2], t->kind_passed[3],
                 t->kind_cases[3], t->kind_passed[4], t->kind_cases[4], t->kind_passed[5], t->kind_cases[5],
                 t->kind_passed[6], t->kind_cases[6], t->kind_passed[7], t->kind_cases[7], t->kind_passed[8],
                 t->kind_cases[8], t->gx_images, t->mip_levels, t->draws, t->peeks, t->mismatches, t->max_err[0],
                 t->max_err[1], t->max_err[2], t->max_err[3], t->controls_detected, t->controls, (unsigned)t->crc,
                 growth);
        for (int k = 0; k < CONTENT_KIND_COUNT; ++k)
            log_line("TIMING cycle=%u kind=%s valid_bytes=%u decoded_bytes=%u load_us=%u decode_us=%u digest_us=%u "
                     "scope=SD_read_and_CPU_decode_in_Dolphin_emulated_time\n",
                     cycle, content_kind_name((enum content_kind)k), (unsigned)t->bytes[k],
                     (unsigned)t->decoded_bytes[k], us(t->load_ticks[k]), us(t->decode_ticks[k]),
                     us(t->digest_ticks[k]));
        for (int k = CONTENT_KIND_MODEL; k < CONTENT_KIND_COUNT; ++k) {
            const struct largest *l = &t->largest[k];
            log_line("MEMORY cycle=%u kind=%s largest=%s file_bytes=%u decoded_bytes=%u units=%u items=%u "
                     "expanded_bytes=%u heap_peak=%d heap_resident=%d load_us=%u decode_us=%u\n",
                     cycle, content_kind_name((enum content_kind)k), l->id[0] ? l->id : "-", (unsigned)l->file_bytes,
                     (unsigned)l->decoded_bytes, (unsigned)l->units, (unsigned)l->items, (unsigned)l->expanded_bytes,
                     l->heap_peak, l->heap_resident, us(l->load_ticks), us(l->decode_ticks));
        }
    }
    int stable = cases_ok, all_passed = cases_ok;
    for (unsigned cycle = 0; cases_ok && cycle < CYCLES; ++cycle) {
        stable &= totals[cycle].crc == totals[0].crc && totals[cycle].passed == totals[0].passed;
        all_passed &= totals[cycle].failed == 0 && totals[cycle].passed == case_count &&
                      totals[cycle].controls_detected > 0;
    }
    log_line("SUMMARY cycles=%u cases=%u stable=%d all_passed=%d heap_growth_max=%d\n", CYCLES, case_count, stable,
             all_passed, heap_growth);
    if (record != NULL && fflush(record) != 0)
        log_failed = 1;

    /* Exit on START (the authored pad script holds it), or after a bounded wait. */
    const char *exit_reason = "auto_exit";
    for (unsigned frame = 0; frame < POST_CHECK_FRAMES; ++frame) {
        PAD_ScanPads();
        WPAD_ScanPads();
        if (PAD_ButtonsHeld(0) & PAD_BUTTON_START) {
            exit_reason = "pad_start";
            break;
        }
        if (WPAD_ButtonsHeld(0) & WPAD_BUTTON_HOME) {
            exit_reason = "remote_home";
            break;
        }
        show_frame();
    }
    free(scratch);
    int success = storage > 0 && all_passed && stable && heap_growth == 0 && !log_failed;
    log_line("END target=content_loaders build=%s exit=%s storage=%d cases=%u stable=%d heap_growth=%d result=%s\n",
             WII_BUILD_ID, exit_reason, storage, case_count, stable, heap_growth, success ? "pass" : "fail");
    if (record != NULL && (fclose(record) != 0 || log_failed))
        return 1;
    return success ? 0 : 1;
}
