/* Asset-free GX materials, texture and resource self-test (HWI-014).
 *
 * Every texture, palette and vertex is authored here; no game data is read.
 * Each check renders a fixed configuration into the EFB and reads pixels back
 * from the CPU, so the guest itself decides pass or fail. Controls use a
 * deliberately wrong configuration (depth off, culling off, untiled texel
 * order, unconverted DXT1, alpha test off, one TEV stage...) and must change
 * the answer. This is a diagnostic, not a Halo renderer or a hardware result. */
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

_Static_assert(sizeof(void *) == 4, "The self-test requires PPC32 pointers");

#define FIFO_BYTES (256u * 1024u)
#define GX_ALIGN 32u
#define MAX_OWNED 48
#define POOL_BYTES (64u * 1024u)
#define GUARD_BYTES 32u
#define GUARD_FILL 0xA5
#define CYCLES 4
#define TILE 64
#define SCENE_DIR "sd:/halo-wii-gxm"
#define RUNS_PATH SCENE_DIR "/runs.txt"
#define LOG_PATH SCENE_DIR "/materials.log"
#define RUNS_PREFIX "halo-wii-gxm-v1 "
/* Processor-interface CPU FIFO write pointer (physical address bits). */
#define PI_FIFO_WRITE_POINTER (*(volatile u32 *)0xCC003014)
#define PI_FIFO_ADDRESS_MASK 0x03FFFFE0u

static FILE *record;
static int log_failed;
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

__attribute__((noinline)) static void sample_stack(void)
{
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (here < stack_lowest)
        stack_lowest = here;
}

/* ------------------------------------------------------------------------
 * Registry of every CPU-visible buffer that GX or VI reads.
 * ------------------------------------------------------------------------ */

enum reject { REJ_NONE, REJ_NULL, REJ_ZERO, REJ_WRAP, REJ_ALIGN, REJ_REGION, REJ_OVERLAP, REJ_FULL,
              REJ_UNOWNED, REJ_RELEASE };
static const char *const reject_names[] = {"none", "null", "zero_bytes", "range_wraps", "misaligned",
                                           "outside_mem1_mem2", "overlap", "registry_full",
                                           "unowned_gx_read", "bad_release"};

struct owned_range {
    const char *name;
    uintptr_t base;
    u32 bytes;
    const char *cache;
    int active;
};

static struct owned_range owned[MAX_OWNED];
static int expect_rejection;      /* set only while a deliberate negative case runs */
static enum reject last_reject;
static unsigned rejections, unexpected_failures, owned_active, owned_peak;

static void reject(enum reject why, const char *name, const void *base, u32 bytes)
{
    last_reject = why;
    ++rejections;
    if (!expect_rejection)
        ++unexpected_failures;
    scene_log("REJECT reason=%s name=%s base=%p bytes=%u expected=%d\n", reject_names[why], name, base,
              (unsigned)bytes, expect_rejection);
}

static const char *region_of(uintptr_t physical, u32 bytes)
{
    if (physical + bytes <= 0x01800000u)
        return "MEM1";
    if (physical >= 0x10000000u && physical + bytes <= 0x14000000u)
        return "MEM2";
    return NULL;
}

/* Returns a handle, or -1 after logging why the range was refused. */
static int own(const char *name, const void *pointer, u32 bytes, u32 align, const char *cache)
{
    uintptr_t base = (uintptr_t)pointer;
    uintptr_t physical = (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(pointer);
    last_reject = REJ_NONE;
    if (pointer == NULL)
        reject(REJ_NULL, name, pointer, bytes);
    else if (bytes == 0)
        reject(REJ_ZERO, name, pointer, bytes);
    else if (physical + bytes < physical || base + bytes < base)
        reject(REJ_WRAP, name, pointer, bytes);
    else if (base % align != 0 || bytes % GX_ALIGN != 0)
        reject(REJ_ALIGN, name, pointer, bytes);
    else if (region_of(physical, bytes) == NULL)
        reject(REJ_REGION, name, pointer, bytes);
    if (last_reject != REJ_NONE)
        return -1;
    int slot = -1;
    for (int i = 0; i < MAX_OWNED; ++i) {
        if (!owned[i].active) {
            if (slot < 0)
                slot = i;
            continue;
        }
        uintptr_t other = (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL((void *)owned[i].base);
        if (physical < other + owned[i].bytes && other < physical + bytes) {
            reject(REJ_OVERLAP, name, pointer, bytes);
            scene_log("REJECT_DETAIL name=%s overlaps=%s\n", name, owned[i].name);
            return -1;
        }
    }
    if (slot < 0) {
        reject(REJ_FULL, name, pointer, bytes);
        return -1;
    }
    owned[slot] = (struct owned_range){name, base, bytes, cache, 1};
    if (++owned_active > owned_peak)
        owned_peak = owned_active;
    if (!expect_rejection)
        scene_log("OWN name=%s base=%p physical=0x%08x bytes=%u align=%u region=%s cache=%s\n", name, pointer,
                  (unsigned)physical, (unsigned)bytes, (unsigned)align, region_of(physical, bytes), cache);
    return slot;
}

static int release(int handle, int quiet)
{
    if (handle < 0 || handle >= MAX_OWNED || !owned[handle].active) {
        reject(REJ_RELEASE, "release", NULL, (u32)handle);
        return 0;
    }
    owned[handle].active = 0;
    --owned_active;
    if (!quiet && !expect_rejection)
        scene_log("RELEASE name=%s\n", owned[handle].name);
    return 1;
}

/* A GX read must lie entirely inside one active owned range. */
static int owned_contains(const char *name, const void *pointer, u32 bytes)
{
    uintptr_t base = (uintptr_t)pointer;
    for (int i = 0; i < MAX_OWNED; ++i)
        if (owned[i].active && base >= owned[i].base && base + bytes >= base &&
            base + bytes <= owned[i].base + owned[i].bytes)
            return 1;
    reject(REJ_UNOWNED, name, pointer, bytes);
    return 0;
}

/* ------------------------------------------------------------------------
 * MEM2 texture pool: a bump allocator carved from the MEM2 arena, with a
 * guard line after every allocation so an overrun is detected on release.
 * ------------------------------------------------------------------------ */

static uintptr_t pool_base, pool_top, pool_end, pool_peak;
static unsigned pool_failures;

static u32 round32(u32 bytes) { return (bytes + 31u) & ~31u; }

static u8 *pool_alloc(const char *name, u32 bytes)
{
    u32 need = round32(bytes) + GUARD_BYTES;
    if (bytes == 0 || need < bytes || need > pool_end - pool_top) {
        ++pool_failures;
        scene_log("ALLOC_FAIL pool name=%s bytes=%u free=%u expected=%d\n", name, (unsigned)bytes,
                  (unsigned)(pool_end - pool_top), expect_rejection);
        if (!expect_rejection)
            ++unexpected_failures;
        return NULL;
    }
    u8 *pointer = (u8 *)pool_top;
    pool_top += need;
    if (pool_top - pool_base > pool_peak)
        pool_peak = pool_top - pool_base;
    memset(pointer + round32(bytes), GUARD_FILL, GUARD_BYTES);
    return pointer;
}

static int guard_intact(const u8 *pointer, u32 bytes)
{
    const u8 *guard = pointer + round32(bytes);
    for (u32 i = 0; i < GUARD_BYTES; ++i)
        if (guard[i] != GUARD_FILL)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
 * Authored texel content and GX tiled encodings.
 * ------------------------------------------------------------------------ */

static GXColor rgba(int r, int g, int b, int a) { return (GXColor){(u8)r, (u8)g, (u8)b, (u8)a}; }
static u8 e5(u32 v) { return (u8)((v << 3) | (v >> 2)); }
static u8 e6(u32 v) { return (u8)((v << 2) | (v >> 4)); }
static u8 e4(u32 v) { return (u8)(v * 17); }
static u8 e3(u32 v) { return (u8)((v << 5) | (v << 2) | (v >> 1)); }

static u16 pack565(GXColor c) { return (u16)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3)); }
static GXColor unpack565(u16 v) { return rgba(e5(v >> 11), e6((v >> 5) & 63), e5(v & 31), 255); }

static u16 pack5a3(GXColor c, int opaque)
{
    if (opaque)
        return (u16)(0x8000 | ((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3));
    return (u16)(((c.a >> 5) << 12) | ((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4));
}

static GXColor unpack5a3(u16 v)
{
    if (v & 0x8000)
        return rgba(e5((v >> 10) & 31), e5((v >> 5) & 31), e5(v & 31), 255);
    return rgba(e4((v >> 8) & 15), e4((v >> 4) & 15), e4(v & 15), e3((v >> 12) & 7));
}

/* A hash-like pattern: neighbouring texels and swapped blocks differ widely. */
static GXColor authored(unsigned x, unsigned y, unsigned seed)
{
    return rgba(x * 53 + y * 17 + 30 + seed, x * 29 + y * 71 + 90 + seed * 3,
                x * 97 + y * 43 + 150 + seed * 7, x * 67 + y * 23 + 40 + seed * 11);
}

enum { T_I8, T_IA8, T_565, T_5A3, T_RGBA8, T_CMPR, T_CI8, T_MIP, T_ADD, T_DYN,
       T_I8_LINEAR, T_565_LINEAR, T_RGBA8_LINEAR, T_CMPR_RAW, T_CMPR_ORDER, T_COUNT };

struct texture_def {
    const char *name;
    u8 fmt;
    u16 w, h;
    u8 levels;
    u8 linear;
};

static const struct texture_def texture_defs[T_COUNT] = {
    [T_I8] = {"tex_i8", GX_TF_I8, 16, 8, 1, 0},
    [T_IA8] = {"tex_ia8", GX_TF_IA8, 8, 8, 1, 0},
    [T_565] = {"tex_rgb565", GX_TF_RGB565, 8, 8, 1, 0},
    [T_5A3] = {"tex_rgb5a3", GX_TF_RGB5A3, 8, 8, 1, 0},
    [T_RGBA8] = {"tex_rgba8", GX_TF_RGBA8, 8, 8, 1, 0},
    [T_CMPR] = {"tex_cmpr", GX_TF_CMPR, 16, 16, 1, 0},
    [T_CI8] = {"tex_ci8", GX_TF_CI8, 8, 8, 1, 0},
    [T_MIP] = {"tex_mip_rgb565", GX_TF_RGB565, 32, 32, 6, 0},
    [T_ADD] = {"tex_tev_add_i8", GX_TF_I8, 8, 8, 1, 0},
    [T_DYN] = {"tex_dynamic_rgb565", GX_TF_RGB565, 8, 8, 1, 0},
    [T_I8_LINEAR] = {"ctl_i8_untiled", GX_TF_I8, 16, 8, 1, 1},
    [T_565_LINEAR] = {"ctl_rgb565_untiled", GX_TF_RGB565, 8, 8, 1, 1},
    [T_RGBA8_LINEAR] = {"ctl_rgba8_untiled", GX_TF_RGBA8, 8, 8, 1, 1},
    [T_CMPR_RAW] = {"ctl_cmpr_raw_dxt1", GX_TF_CMPR, 16, 16, 1, 1},
    [T_CMPR_ORDER] = {"ctl_cmpr_dxt1_block_order", GX_TF_CMPR, 16, 16, 1, 2},
};

struct texture {
    u8 *data;
    u32 bytes;         /* GX tiled layout size: what the sampler reads */
    u32 owned_bytes;   /* allocation: max(layout, libogc GX_GetTexBufferSize) */
    int handle;
};

/* Per-load material state; allocated from the heap and freed on unload so
 * heap growth across repeated loads is measurable. */
struct materials {
    struct texture tex[T_COUNT];
    u8 *dxt1;          /* authored source in Xbox/PC DXT1 layout (CPU only) */
    u16 *tlut;         /* CI8 palette, RGB565 big-endian, 16 entries */
    int tlut_handle;
    GXTlutObj tlut_obj;
    u32 texture_bytes, owned_texture_bytes;
};

static struct materials *mat;

static void tile_shape(u8 fmt, u32 *tw, u32 *th, u32 *tile_bytes)
{
    *tile_bytes = 32;
    switch (fmt) {
    case GX_TF_I8: case GX_TF_CI8: *tw = 8; *th = 4; break;
    case GX_TF_CMPR: case GX_TF_I4: case GX_TF_CI4: *tw = 8; *th = 8; break;
    case GX_TF_RGBA8: *tw = 4; *th = 4; *tile_bytes = 64; break;
    default: *tw = 4; *th = 4; break;
    }
}

static u32 level_bytes(u8 fmt, u32 w, u32 h)
{
    u32 tw, th, tb;
    tile_shape(fmt, &tw, &th, &tb);
    return ((w + tw - 1) / tw) * ((h + th - 1) / th) * tb;
}

static u32 libogc_bytes(const struct texture_def *d)
{
    return GX_GetTexBufferSize(d->w, d->h, d->fmt, d->levels > 1 ? GX_TRUE : GX_FALSE, d->levels);
}

static u32 texture_bytes(const struct texture_def *d)
{
    u32 total = 0;
    for (u32 level = 0; level < d->levels; ++level) {
        u32 w = d->w >> level, h = d->h >> level;
        total += level_bytes(d->fmt, w ? w : 1, h ? h : 1);
    }
    return total;
}

/* Byte offset of texel (x, y) for 8- and 16-bit formats, tiled or untiled. */
static u32 texel_offset(u8 fmt, u32 w, u32 x, u32 y, int linear)
{
    u32 tw, th, tb;
    tile_shape(fmt, &tw, &th, &tb);
    u32 bpp = (fmt == GX_TF_I8 || fmt == GX_TF_CI8) ? 1 : 2;
    if (linear)
        return (y * w + x) * bpp;
    return ((y / th) * (w / tw) + x / tw) * tb + ((y % th) * tw + x % tw) * bpp;
}

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static u8 add_intensity(unsigned x, unsigned y) { return (u8)(64 + (authored(x, y, 6).r & 127)); }
static int ci8_index(unsigned x, unsigned y) { return (int)((x * 5 + y * 3) & 15); }
static GXColor ci8_palette(int i) { return authored((unsigned)i, (unsigned)(i * 3) & 7, 5); }
static int rgb5a3_opaque(unsigned x, unsigned y) { return (x + y) % 3 != 0; }

static const GXColor mip_colours[6] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
                                       {255, 255, 0, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}};
static const GXColor dyn_before = {255, 0, 255, 255}, dyn_after = {0, 255, 0, 255};

/* Authored DXT1 in the Xbox/PC layout: 4x4 blocks in row-major order, colours
 * little-endian, 2-bit indices with the leftmost texel in the low bits. Every
 * fourth block uses the three-colour mode with a transparent index. */
static void author_dxt1(u8 *src, u32 w, u32 h)
{
    u32 blocks_x = w / 4;
    for (u32 by = 0; by < h / 4; ++by)
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            u32 b = by * blocks_x + bx;
            u8 *block = src + b * 8;
            u16 c0 = pack565(authored(bx * 4, by * 4, 7)), c1 = pack565(authored(bx * 4 + 3, by * 4 + 3, 9));
            if (c0 == c1)
                c1 ^= 0x8410;
            int three_colour = b % 4 == 3;
            if (three_colour ? c0 > c1 : c0 < c1) {
                u16 t = c0;
                c0 = c1;
                c1 = t;
            }
            block[0] = (u8)c0; block[1] = (u8)(c0 >> 8);
            block[2] = (u8)c1; block[3] = (u8)(c1 >> 8);
            for (u32 row = 0; row < 4; ++row) {
                u8 bits = 0;
                for (u32 column = 0; column < 4; ++column)
                    bits |= (u8)(((column + 2 * row + b) & 3) << (2 * column));
                block[4 + row] = bits;
            }
        }
}

/* Reference decode of the logical DXT1 image with GX/Dolphin interpolation:
 * 5/8 + 3/8 in four-colour mode (standard DXT1 uses 2/3 + 1/3), half-way and
 * transparent black in three-colour mode. *interpolated is set for c2/c3. */
static GXColor dxt1_texel(const u8 *src, u32 w, u32 x, u32 y, int *interpolated)
{
    const u8 *block = src + ((y / 4) * (w / 4) + x / 4) * 8;
    u16 c0 = (u16)(block[0] | block[1] << 8), c1 = (u16)(block[2] | block[3] << 8);
    u32 index = (block[4 + y % 4] >> (2 * (x % 4))) & 3;
    GXColor a = unpack565(c0), b = unpack565(c1);
    *interpolated = index >= 2;
    if (index == 0)
        return a;
    if (index == 1)
        return b;
    if (c0 > c1) {
        if (index == 2)
            return rgba((a.r * 5 + b.r * 3) >> 3, (a.g * 5 + b.g * 3) >> 3, (a.b * 5 + b.b * 3) >> 3, 255);
        return rgba((a.r * 3 + b.r * 5) >> 3, (a.g * 3 + b.g * 5) >> 3, (a.b * 3 + b.b * 5) >> 3, 255);
    }
    if (index == 2)
        return rgba((a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2, 255);
    return rgba(0, 0, 0, 0);
}

static void convert_dxt1_block(const u8 *s, u8 *d)
{
    d[0] = s[1]; d[1] = s[0]; d[2] = s[3]; d[3] = s[2];
    for (int row = 0; row < 4; ++row) {
        u8 v = s[4 + row];
        d[4 + row] = (u8)(((v & 3) << 6) | (((v >> 2) & 3) << 4) | (((v >> 4) & 3) << 2) | ((v >> 6) & 3));
    }
}

/* DXT1 -> GX CMPR: big-endian colours, MSB-first indices, and 2x2 groups of
 * 4x4 blocks per 8x8 tile. mode 1 copies the DXT1 bytes unchanged and mode 2
 * converts each block but keeps DXT1 row-major block order (both controls). */
static void dxt1_to_cmpr(const u8 *src, u8 *dst, u32 w, u32 h, int mode)
{
    u32 blocks_x = w / 4;
    if (mode == 1) {
        memcpy(dst, src, w * h / 2);
        return;
    }
    if (mode == 2) {
        for (u32 b = 0; b < (w / 4) * (h / 4); ++b)
            convert_dxt1_block(src + b * 8, dst + b * 8);
        return;
    }
    u8 *out = dst;
    for (u32 ty = 0; ty < h / 8; ++ty)
        for (u32 tx = 0; tx < w / 8; ++tx)
            for (u32 sub = 0; sub < 4; ++sub) {
                u32 bx = tx * 2 + (sub & 1), by = ty * 2 + (sub >> 1);
                convert_dxt1_block(src + (by * blocks_x + bx) * 8, out);
                out += 8;
            }
}

/* The colour the sampler should return for texel (x, y) of texture id. */
static GXColor expect_texel(int id, u32 x, u32 y, int *loose, int *transparent)
{
    int interpolated = 0;
    GXColor c;
    *loose = 0;
    *transparent = 0;
    switch (id) {
    case T_I8: case T_I8_LINEAR: {
        u8 i = authored(x, y, 1).r;
        return rgba(i, i, i, i);
    }
    case T_IA8: {
        GXColor s = authored(x, y, 2);
        return rgba(s.r, s.r, s.r, s.a);
    }
    case T_565: case T_565_LINEAR: return unpack565(pack565(authored(x, y, 3)));
    case T_5A3: return unpack5a3(pack5a3(authored(x, y, 4), rgb5a3_opaque(x, y)));
    case T_RGBA8: case T_RGBA8_LINEAR: return authored(x, y, 5);
    case T_CI8: return unpack565(pack565(ci8_palette(ci8_index(x, y))));
    case T_ADD: {
        u8 i = add_intensity(x, y);
        return rgba(i, i, i, i);
    }
    case T_CMPR: case T_CMPR_RAW: case T_CMPR_ORDER:
        c = dxt1_texel(mat->dxt1, 16, x, y, &interpolated);
        *loose = interpolated;
        *transparent = c.a == 0;
        return c;
    default: return rgba(0, 0, 0, 255);
    }
}

static void encode_texture(int id)
{
    const struct texture_def *d = &texture_defs[id];
    struct texture *t = &mat->tex[id];
    memset(t->data, 0, t->bytes);
    if (d->fmt == GX_TF_CMPR) {
        dxt1_to_cmpr(mat->dxt1, t->data, d->w, d->h, d->linear);
        return;
    }
    if (id == T_MIP) {
        u8 *level = t->data;
        for (u32 n = 0; n < d->levels; ++n) {
            u32 w = d->w >> n, h = d->h >> n, bytes = level_bytes(d->fmt, w ? w : 1, h ? h : 1);
            for (u32 i = 0; i < bytes; i += 2)
                put16(level + i, pack565(mip_colours[n]));
            level += bytes;
        }
        return;
    }
    if (id == T_DYN) {
        for (u32 i = 0; i < t->bytes; i += 2)
            put16(t->data + i, pack565(dyn_before));
        return;
    }
    for (u32 y = 0; y < d->h; ++y)
        for (u32 x = 0; x < d->w; ++x) {
            if (d->fmt == GX_TF_RGBA8) {
                GXColor c = authored(x, y, 5);
                u32 base, within = (y % 4) * 4 + x % 4;
                if (d->linear) {
                    base = (y * d->w + x) * 4;
                    t->data[base] = c.a; t->data[base + 1] = c.r;
                    t->data[base + 2] = c.g; t->data[base + 3] = c.b;
                } else {
                    base = ((y / 4) * (d->w / 4) + x / 4) * 64;
                    t->data[base + within * 2] = c.a; t->data[base + within * 2 + 1] = c.r;
                    t->data[base + 32 + within * 2] = c.g; t->data[base + 32 + within * 2 + 1] = c.b;
                }
                continue;
            }
            u8 *p = t->data + texel_offset(d->fmt, d->w, x, y, d->linear);
            switch (id) {
            case T_I8: case T_I8_LINEAR: p[0] = authored(x, y, 1).r; break;
            case T_ADD: p[0] = add_intensity(x, y); break;
            case T_CI8: p[0] = (u8)ci8_index(x, y); break;
            case T_IA8: { GXColor s = authored(x, y, 2); p[0] = s.a; p[1] = s.r; break; }
            case T_565: case T_565_LINEAR: put16(p, pack565(authored(x, y, 3))); break;
            case T_5A3: put16(p, pack5a3(authored(x, y, 4), rgb5a3_opaque(x, y))); break;
            default: break;
            }
        }
}

/* Creates a texture object only after confirming GX will read owned memory. */
static int texture_object(GXTexObj *obj, int id, u8 wrap, u8 filter)
{
    const struct texture_def *d = &texture_defs[id];
    struct texture *t = &mat->tex[id];
    if (!owned_contains(d->name, t->data, t->bytes))
        return 0;
    if (d->fmt == GX_TF_CI8) {
        if (!owned_contains("tlut", mat->tlut, 32))
            return 0;
        GX_InitTexObjCI(obj, t->data, d->w, d->h, d->fmt, wrap, wrap, GX_FALSE, GX_TLUT0);
    } else {
        GX_InitTexObj(obj, t->data, d->w, d->h, d->fmt, wrap, wrap, d->levels > 1 ? GX_TRUE : GX_FALSE);
    }
    if (d->levels > 1)
        GX_InitTexObjLOD(obj, GX_NEAR_MIP_NEAR, GX_NEAR, 0.0f, (f32)(d->levels - 1), 0.0f, GX_FALSE, GX_FALSE,
                         GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(obj, filter, filter);
    return 1;
}

/* ------------------------------------------------------------------------
 * GX state and drawing helpers (screen-space orthographic by default).
 * ------------------------------------------------------------------------ */

static GXRModeObj *mode;
static Mtx44 ortho;
static Mtx identity;
static void *scratch_xfb;
static const GXColor clear_colour = {24, 24, 32, 255};
static const GXColor background = {40, 80, 200, 255};
static const GXColor white = {255, 255, 255, 255};

static void vtx(f32 x, f32 y, f32 z, GXColor c, f32 s, f32 t)
{
    GX_Position3f32(x, y, z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(s, t);
}

/* Clockwise on screen (front-facing) unless ccw. */
static void rect_st(f32 x0, f32 y0, f32 x1, f32 y1, f32 z, GXColor c, f32 s0, f32 t0, f32 s1, f32 t1, int ccw)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    if (!ccw) {
        vtx(x0, y0, z, c, s0, t0); vtx(x1, y0, z, c, s1, t0);
        vtx(x1, y1, z, c, s1, t1); vtx(x0, y1, z, c, s0, t1);
    } else {
        vtx(x0, y0, z, c, s0, t0); vtx(x0, y1, z, c, s0, t1);
        vtx(x1, y1, z, c, s1, t1); vtx(x1, y0, z, c, s1, t0);
    }
    GX_End();
}

static void rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 z, GXColor c)
{
    rect_st(x0, y0, x1, y1, z, c, 0, 0, 1, 1, 0);
}

static void state_reset(void)
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
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
}

static void use_texture(GXTexObj *obj, u8 map, u8 tevop)
{
    GX_LoadTexObj(obj, map);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, map, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, tevop);
}

/* Shows texture alpha as grey so the RGB8 EFB can verify the alpha channel. */
static void show_texture_alpha(void)
{
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXA);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/* ------------------------------------------------------------------------
 * Check machinery: probes read the EFB through the CPU peek path.
 * ------------------------------------------------------------------------ */

enum kind { POSITIVE, CONTROL_VALUE, CONTROL_DIFF };
static const char *const kind_names[] = {"positive", "control_value", "control_must_differ"};

static int gallery;   /* drawing for display only: no probes, no clears */
static unsigned cycle_number;
static u32 cycle_crc;
static unsigned peek_count;
static u64 peek_ticks;

static struct {
    const char *name;
    int kind;
    unsigned probes, matched, logged;
    int max_err;
    int synced;
    u32 write_before;
    u64 started;
} current;

static struct {
    unsigned passed, total;
    u32 fifo_check_peak;
    u64 fifo_total;
    u64 check_ticks;
} stats;

static u32 fifo_physical;

static u32 crc32_update(u32 crc, const u8 *data, u32 bytes)
{
    crc = ~crc;
    for (u32 i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static u32 fifo_delta(u32 before)
{
    u32 after = PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK;
    if (before < fifo_physical || before >= fifo_physical + FIFO_BYTES) {
        ++unexpected_failures;
        scene_log("REJECT reason=fifo_write_pointer_outside_owned_fifo value=0x%08x\n", (unsigned)before);
    }
    return (after - before + FIFO_BYTES) % FIFO_BYTES;
}

static void check_begin(const char *name, int kind)
{
    current.name = name;
    current.kind = kind;
    current.probes = current.matched = current.logged = 0;
    current.max_err = 0;
    current.synced = 0;
    current.started = gettime();
    current.write_before = PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK;
}

static void sync_draws(void)
{
    if (current.synced)
        return;
    GX_DrawDone();
    u32 submitted = fifo_delta(current.write_before);
    if (submitted > stats.fifo_check_peak)
        stats.fifo_check_peak = submitted;
    stats.fifo_total += submitted;
    current.synced = 1;
}

static void probe(u32 x, u32 y, GXColor expected, int tolerance)
{
    if (gallery)
        return;
    sync_draws();
    GXColor seen;
    u64 start = gettime();
    GX_PeekARGB((u16)x, (u16)y, &seen);
    peek_ticks += gettime() - start;
    ++peek_count;
    int err = abs(seen.r - expected.r);
    if (abs(seen.g - expected.g) > err) err = abs(seen.g - expected.g);
    if (abs(seen.b - expected.b) > err) err = abs(seen.b - expected.b);
    ++current.probes;
    if (err > current.max_err)
        current.max_err = err;
    if (err <= tolerance)
        ++current.matched;
    else if (current.logged < 3 && current.kind != CONTROL_DIFF) {
        ++current.logged;
        scene_log("MISS check=%s pixel=%u,%u expected=%02x%02x%02x observed=%02x%02x%02x tol=%d\n", current.name,
                  (unsigned)x, (unsigned)y, expected.r, expected.g, expected.b, seen.r, seen.g, seen.b, tolerance);
    }
    u8 rgb[3] = {seen.r, seen.g, seen.b};
    cycle_crc = crc32_update(cycle_crc, rgb, 3);
}

/* A non-colour check (for example a depth-order comparison). */
static void probe_bool(int ok)
{
    if (gallery)
        return;
    sync_draws();
    ++current.probes;
    current.matched += ok ? 1 : 0;
}

static int check_end(void)
{
    sync_draws();
    int pass;
    if (current.kind == CONTROL_DIFF)
        pass = current.probes > 0 && (current.probes - current.matched) * 4 >= current.probes;
    else
        pass = current.probes > 0 && current.matched == current.probes;
    u64 elapsed = gettime() - current.started;
    stats.check_ticks += elapsed;
    scene_log("CHECK cycle=%u name=%s kind=%s probes=%u matched=%u max_err=%d result=%s\n", cycle_number,
              current.name, kind_names[current.kind], current.probes, current.matched, current.max_err,
              pass ? "pass" : "fail");
    /* Clear colour and depth for the next check. */
    GX_CopyDisp(scratch_xfb, GX_TRUE);
    GX_DrawDone();
    ++stats.total;
    stats.passed += pass ? 1u : 0u;
    return pass;
}

/* Centre pixel of texel i when n texels span size pixels starting at origin. */
static u32 texel_px(u32 origin, u32 size, u32 n, u32 i) { return origin + (i * size) / n + size / (2 * n); }

/* Pass the texture through REPLACE and probe every texel (or its alpha). */
static void probe_texture(int id, u32 X, u32 Y, u32 sw, u32 sh, int alpha, int tolerance)
{
    const struct texture_def *d = &texture_defs[id];
    for (u32 y = 0; y < d->h; ++y)
        for (u32 x = 0; x < d->w; ++x) {
            int loose, transparent;
            GXColor e = expect_texel(id, x, y, &loose, &transparent);
            if (alpha)
                e = rgba(e.a, e.a, e.a, 255);
            else if (transparent)
                continue;   /* colour of a transparent CMPR texel is unspecified */
            probe(texel_px(X, sw, d->w, x), texel_px(Y, sh, d->h, y), e, loose && !alpha ? tolerance + 6 : tolerance);
        }
}

/* ------------------------------------------------------------------------
 * The checks. Each draws into its own 64x64 slot.
 * ------------------------------------------------------------------------ */

struct check_def;
typedef void (*check_fn)(const struct check_def *, u32 X, u32 Y);
struct check_def {
    const char *name;
    u8 kind;
    u8 slot;
    u8 no_gallery;
    check_fn run;
    int arg;
};

static const GXColor near_colour = {220, 40, 40, 255}, far_colour = {40, 200, 80, 255};

static void ck_depth(const struct check_def *c, u32 X, u32 Y)
{
    /* arg 0: near then far; 1: far then near; 2: control, depth off. */
    if (c->arg == 2)
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    if (c->arg == 1)
        rect(X, Y, X + TILE, Y + TILE, -0.75f, far_colour);
    rect(X, Y, X + TILE, Y + TILE, -0.25f, near_colour);
    if (c->arg != 1)
        rect(X, Y, X + TILE, Y + TILE, -0.75f, far_colour);
    probe(X + 32, Y + 32, c->arg == 2 ? far_colour : near_colour, 6);
}

static void ck_depth_values(const struct check_def *c, u32 X, u32 Y)
{
    (void)c;
    rect(X, Y, X + 30, Y + TILE, -0.25f, near_colour);
    rect(X + 34, Y, X + TILE, Y + TILE, -0.75f, far_colour);
    if (gallery)
        return;
    sync_draws();
    u32 z_near = 0, z_far = 0;
    GX_PeekZ((u16)(X + 15), (u16)(Y + 32), &z_near);
    GX_PeekZ((u16)(X + 49), (u16)(Y + 32), &z_far);
    scene_log("ZPEEK cycle=%u near=%06x far=%06x clear=%06x\n", cycle_number, (unsigned)z_near, (unsigned)z_far,
              (unsigned)GX_MAX_Z24);
    probe_bool(z_near < z_far && z_far < GX_MAX_Z24);
}

static const GXColor cw_colour = {230, 120, 40, 255}, ccw_colour = {60, 200, 220, 255};

static void ck_cull(const struct check_def *c, u32 X, u32 Y)
{
    static const u8 modes[] = {GX_CULL_BACK, GX_CULL_FRONT, GX_CULL_NONE, GX_CULL_ALL};
    GX_SetCullMode(modes[c->arg]);
    rect_st(X, Y, X + 30, Y + TILE, -0.5f, cw_colour, 0, 0, 1, 1, 0);
    rect_st(X + 34, Y, X + TILE, Y + TILE, -0.5f, ccw_colour, 0, 0, 1, 1, 1);
    int cw_visible = c->arg == 0 || c->arg == 2, ccw_visible = c->arg == 1 || c->arg == 2;
    probe(X + 15, Y + 32, cw_visible ? cw_colour : clear_colour, 6);
    probe(X + 49, Y + 32, ccw_visible ? ccw_colour : clear_colour, 6);
}

/* Two planes crossing at the tile centre in perspective: the left one is
 * nearer on the left, the right one nearer on the right. */
static void ck_perspective(const struct check_def *c, u32 X, u32 Y)
{
    Mtx44 perspective;
    guPerspective(perspective, 60, 1.0f, 0.1f, 100.0f);
    GX_SetViewport(X, Y, TILE, TILE, 0, 1);
    GX_SetScissor(X, Y, TILE, TILE);
    GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);
    if (c->arg)
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 8);
    vtx(-3, 1.5f, -2.5f, near_colour, 0, 0); vtx(3, 1.5f, -5.5f, near_colour, 0, 0);
    vtx(3, -1.5f, -5.5f, near_colour, 0, 0); vtx(-3, -1.5f, -2.5f, near_colour, 0, 0);
    vtx(-3, 1.5f, -5.5f, far_colour, 0, 0); vtx(3, 1.5f, -2.5f, far_colour, 0, 0);
    vtx(3, -1.5f, -2.5f, far_colour, 0, 0); vtx(-3, -1.5f, -5.5f, far_colour, 0, 0);
    GX_End();
    probe(X + 16, Y + 32, c->arg ? far_colour : near_colour, 6);
    probe(X + 48, Y + 32, far_colour, 6);
}

static void ck_texture(const struct check_def *c, u32 X, u32 Y)
{
    /* arg: texture id in the low byte, 0x100 = show alpha. */
    int id = c->arg & 0xff, alpha = c->arg & 0x100;
    const struct texture_def *d = &texture_defs[id];
    GXTexObj obj;
    if (!texture_object(&obj, id, GX_CLAMP, GX_NEAR))
        return;
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    if (alpha)
        show_texture_alpha();
    u32 sh = d->h * TILE / d->w;
    rect(X, Y, X + TILE, Y + sh, -0.5f, white);
    probe_texture(id, X, Y, TILE, sh, alpha, 6);
}

/* Wrong-order controls render the misencoded data but expect the authored
 * image; they must fail most probes. */
static void ck_texture_control(const struct check_def *c, u32 X, u32 Y)
{
    int wrong = c->arg, right;
    switch (wrong) {
    case T_I8_LINEAR: right = T_I8; break;
    case T_565_LINEAR: right = T_565; break;
    case T_RGBA8_LINEAR: right = T_RGBA8; break;
    default: right = T_CMPR; break;
    }
    const struct texture_def *d = &texture_defs[wrong];
    GXTexObj obj;
    if (!texture_object(&obj, wrong, GX_CLAMP, GX_NEAR))
        return;
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    u32 sh = d->h * TILE / d->w;
    rect(X, Y, X + TILE, Y + sh, -0.5f, white);
    probe_texture(right, X, Y, TILE, sh, 0, 6);
}

static void ck_wrap(const struct check_def *c, u32 X, u32 Y)
{
    static const u8 wraps[] = {GX_REPEAT, GX_CLAMP, GX_MIRROR};
    GXTexObj obj;
    if (!texture_object(&obj, T_565, wraps[c->arg], GX_NEAR))
        return;
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    rect_st(X, Y, X + TILE, Y + TILE, -0.5f, white, 0, 0, 2, 2, 0);
    for (u32 i = 0; i < 8; ++i) {
        u32 wrapped = c->arg == 0 ? i : c->arg == 1 ? 7 : 7 - i;
        int loose, transparent;
        /* Second repetition along s (row t=1), then along t (column s=1). */
        probe(X + 32 + 4 * i + 2, Y + 4 + 2, expect_texel(T_565, wrapped, 1, &loose, &transparent), 6);
        probe(X + 4 + 2, Y + 32 + 4 * i + 2, expect_texel(T_565, 1, wrapped, &loose, &transparent), 6);
    }
}

static GXColor blend(GXColor src, GXColor dst, int a)
{
    return rgba((src.r * a + dst.r * (255 - a) + 127) / 255, (src.g * a + dst.g * (255 - a) + 127) / 255,
                (src.b * a + dst.b * (255 - a) + 127) / 255, 255);
}

static const GXColor overlay = {240, 200, 40, 255};

static void ck_blend(const struct check_def *c, u32 X, u32 Y)
{
    /* arg: overlay alpha; negative = control with blending off. */
    int a = abs(c->arg);
    rect(X, Y, X + TILE, Y + TILE, -0.6f, background);
    if (c->arg > 0)
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    rect(X, Y, X + TILE, Y + 32, -0.4f, rgba(overlay.r, overlay.g, overlay.b, a));
    probe(X + 32, Y + 16, c->arg > 0 ? blend(overlay, background, a) : overlay, 6);
    probe(X + 32, Y + 48, background, 6);
}

static void ck_alpha_test(const struct check_def *c, u32 X, u32 Y)
{
    rect(X, Y, X + TILE, Y + TILE, -0.6f, background);
    if (c->arg == 0) {
        GX_SetZCompLoc(GX_FALSE);
        GX_SetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
    }
    rect(X, Y, X + 30, Y + TILE, -0.4f, rgba(near_colour.r, near_colour.g, near_colour.b, 64));
    rect(X + 34, Y, X + TILE, Y + TILE, -0.4f, rgba(near_colour.r, near_colour.g, near_colour.b, 200));
    probe(X + 15, Y + 32, c->arg == 0 ? background : near_colour, 6);
    probe(X + 49, Y + 32, near_colour, 6);
}

/* RGB5A3 translucent texels blended over the background. */
static void ck_texture_blend(const struct check_def *c, u32 X, u32 Y)
{
    (void)c;
    GXTexObj obj;
    rect(X, Y, X + TILE, Y + TILE, -0.6f, background);
    if (!texture_object(&obj, T_5A3, GX_CLAMP, GX_NEAR))
        return;
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    rect(X, Y, X + TILE, Y + TILE, -0.4f, white);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            int loose, transparent;
            GXColor e = expect_texel(T_5A3, x, y, &loose, &transparent);
            probe(texel_px(X, TILE, 8, x), texel_px(Y, TILE, 8, y), blend(e, background, e.a), 8);
        }
}

/* CMPR cut-out: the three-colour transparent texels must show the background
 * under an alpha test; with the test off (control) they must not. */
static void ck_cutout(const struct check_def *c, u32 X, u32 Y)
{
    GXTexObj obj;
    rect(X, Y, X + TILE, Y + TILE, -0.6f, background);
    if (!texture_object(&obj, T_CMPR, GX_CLAMP, GX_NEAR))
        return;
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    if (c->arg == 0) {
        GX_SetZCompLoc(GX_FALSE);
        GX_SetAlphaCompare(GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0);
    }
    rect(X, Y, X + TILE, Y + TILE, -0.4f, white);
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            int loose, transparent;
            GXColor e = expect_texel(T_CMPR, x, y, &loose, &transparent);
            if (c->arg && !transparent)
                continue;
            probe(texel_px(X, TILE, 16, x), texel_px(Y, TILE, 16, y), transparent ? background : e,
                  loose ? 12 : 6);
        }
}

static void mip_object(GXTexObj *obj, int mipmapped, f32 min_lod, f32 max_lod, f32 bias)
{
    struct texture *t = &mat->tex[T_MIP];
    GX_InitTexObj(obj, t->data, 32, 32, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, mipmapped ? GX_TRUE : GX_FALSE);
    if (mipmapped)
        GX_InitTexObjLOD(obj, GX_NEAR_MIP_NEAR, GX_NEAR, min_lod, max_lod, bias, GX_FALSE, GX_FALSE, GX_ANISO_1);
    else
        GX_InitTexObjFilterMode(obj, GX_NEAR, GX_NEAR);
}

static void ck_mip_select(const struct check_def *c, u32 X, u32 Y)
{
    (void)c;
    GXTexObj obj;
    if (!owned_contains("tex_mip_rgb565", mat->tex[T_MIP].data, mat->tex[T_MIP].bytes))
        return;
    mip_object(&obj, 1, 0.0f, 5.0f, 0.0f);
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    /* 32, 16, 8, 4 and 2 pixel squares of a 32x32 texture: LOD 0..4. */
    static const u8 size[5] = {32, 16, 8, 4, 2}, ox[5] = {0, 36, 54, 36, 44}, oy[5] = {0, 0, 0, 20, 20};
    for (int i = 0; i < 5; ++i)
        rect(X + ox[i], Y + oy[i], X + ox[i] + size[i], Y + oy[i] + size[i], -0.5f, white);
    for (int i = 0; i < 5; ++i)
        probe(X + ox[i] + size[i] / 2, Y + oy[i] + size[i] / 2, mip_colours[i], 6);
}

static void ck_mip_param(const struct check_def *c, u32 X, u32 Y)
{
    /* arg 0: bias +2 at 32 px -> level 2; 1: max LOD 1 at 8 px -> level 1;
     * 2: min LOD 2 at 32 px -> level 2; 3: control, mipmapping off at 8 px -> level 0. */
    GXTexObj obj;
    if (!owned_contains("tex_mip_rgb565", mat->tex[T_MIP].data, mat->tex[T_MIP].bytes))
        return;
    static const f32 min_lod[4] = {0, 0, 2, 0}, max_lod[4] = {5, 1, 5, 5}, bias[4] = {2, 0, 0, 0};
    static const u8 size[4] = {32, 8, 32, 8}, level[4] = {2, 1, 2, 0};
    mip_object(&obj, c->arg != 3, min_lod[c->arg], max_lod[c->arg], bias[c->arg]);
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    rect(X, Y, X + size[c->arg], Y + size[c->arg], -0.5f, white);
    probe(X + size[c->arg] / 2, Y + size[c->arg] / 2, mip_colours[level[c->arg]], 6);
}

static u8 tev_mul(u8 a, u8 c) { return (u8)((a * (c + (c >> 7))) >> 8); }
static const GXColor vertex_tint = {160, 255, 96, 255};

/* arg 0: texture x vertex colour; 1: control, REPLACE; 2: two stages adding
 * 0.5 x a second texture; 3: control, second stage disabled. */
static void ck_tev(const struct check_def *c, u32 X, u32 Y)
{
    GXTexObj base, add;
    if (!texture_object(&base, T_565, GX_CLAMP, GX_NEAR) || !texture_object(&add, T_ADD, GX_CLAMP, GX_NEAR))
        return;
    use_texture(&base, GX_TEXMAP0, c->arg == 1 ? GX_REPLACE : GX_MODULATE);
    if (c->arg >= 2) {
        GX_LoadTexObj(&add, GX_TEXMAP1);
        GX_SetNumTevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
        GX_SetTevKColor(GX_KCOLOR0, (GXColor){128, 128, 128, 128});
        GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        if (c->arg == 3)
            GX_SetNumTevStages(1);   /* stage 1 configured but not enabled */
    }
    rect(X, Y, X + TILE, Y + TILE, -0.5f, vertex_tint);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            int loose, transparent;
            GXColor t = expect_texel(T_565, x, y, &loose, &transparent), e = t;
            if (c->arg != 1)
                e = rgba(tev_mul(t.r, vertex_tint.r), tev_mul(t.g, vertex_tint.g), tev_mul(t.b, vertex_tint.b), 255);
            if (c->arg == 2) {
                int k = tev_mul(add_intensity(x, y), 128);
                e = rgba(e.r + k > 255 ? 255 : e.r + k, e.g + k > 255 ? 255 : e.g + k,
                         e.b + k > 255 ? 255 : e.b + k, 255);
            }
            probe(texel_px(X, TILE, 8, x), texel_px(Y, TILE, 8, y), e, 6);
        }
}

/* In-place texture update: CPU rewrite, DCFlushRange, GX_InvalidateTexAll. */
static void ck_dynamic(const struct check_def *c, u32 X, u32 Y)
{
    struct texture *t = &mat->tex[T_DYN];
    GXColor colour = c->arg ? dyn_after : dyn_before;
    if (!gallery) {
        for (u32 i = 0; i < t->bytes; i += 2)
            put16(t->data + i, pack565(colour));
        DCFlushRange(t->data, t->bytes);
        GX_InvalidateTexAll();
    }
    GXTexObj obj;
    if (!texture_object(&obj, T_DYN, GX_CLAMP, GX_NEAR))
        return;
    use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
    rect(X, Y, X + TILE, Y + TILE, -0.5f, white);
    probe(X + 20, Y + 20, unpack565(pack565(colour)), 6);
    probe(X + 44, Y + 44, unpack565(pack565(colour)), 6);
}

#define TEX(id) (id)
#define ALPHA(id) ((id) | 0x100)
static const struct check_def checks[] = {
    {"depth_near_then_far", POSITIVE, 0, 0, ck_depth, 0},
    {"depth_far_then_near", POSITIVE, 1, 0, ck_depth, 1},
    {"control_depth_off", CONTROL_VALUE, 0, 0, ck_depth, 2},
    {"depth_z_order", POSITIVE, 2, 0, ck_depth_values, 0},
    {"cull_back", POSITIVE, 3, 0, ck_cull, 0},
    {"cull_front", POSITIVE, 4, 0, ck_cull, 1},
    {"control_cull_none", CONTROL_VALUE, 3, 0, ck_cull, 2},
    {"control_cull_all", CONTROL_VALUE, 3, 0, ck_cull, 3},
    {"perspective_intersection", POSITIVE, 5, 0, ck_perspective, 0},
    {"control_perspective_depth_off", CONTROL_VALUE, 5, 0, ck_perspective, 1},
    {"tex_i8", POSITIVE, 6, 0, ck_texture, TEX(T_I8)},
    {"tex_i8_alpha", POSITIVE, 7, 0, ck_texture, ALPHA(T_I8)},
    {"tex_ia8", POSITIVE, 8, 0, ck_texture, TEX(T_IA8)},
    {"tex_ia8_alpha", POSITIVE, 9, 0, ck_texture, ALPHA(T_IA8)},
    {"tex_rgb565", POSITIVE, 10, 0, ck_texture, TEX(T_565)},
    {"tex_rgb5a3", POSITIVE, 11, 0, ck_texture, TEX(T_5A3)},
    {"tex_rgb5a3_alpha", POSITIVE, 12, 0, ck_texture, ALPHA(T_5A3)},
    {"tex_rgba8", POSITIVE, 13, 0, ck_texture, TEX(T_RGBA8)},
    {"tex_rgba8_alpha", POSITIVE, 14, 0, ck_texture, ALPHA(T_RGBA8)},
    {"tex_cmpr_from_dxt1", POSITIVE, 15, 0, ck_texture, TEX(T_CMPR)},
    {"tex_cmpr_alpha", POSITIVE, 16, 0, ck_texture, ALPHA(T_CMPR)},
    {"tex_ci8_tlut_rgb565", POSITIVE, 17, 0, ck_texture, TEX(T_CI8)},
    {"control_i8_untiled_order", CONTROL_DIFF, 6, 0, ck_texture_control, T_I8_LINEAR},
    {"control_rgb565_untiled_order", CONTROL_DIFF, 10, 0, ck_texture_control, T_565_LINEAR},
    {"control_rgba8_untiled_order", CONTROL_DIFF, 13, 0, ck_texture_control, T_RGBA8_LINEAR},
    {"control_cmpr_unconverted_dxt1", CONTROL_DIFF, 15, 0, ck_texture_control, T_CMPR_RAW},
    {"control_cmpr_dxt1_block_order", CONTROL_DIFF, 15, 0, ck_texture_control, T_CMPR_ORDER},
    {"wrap_repeat", POSITIVE, 18, 0, ck_wrap, 0},
    {"wrap_clamp", POSITIVE, 19, 0, ck_wrap, 1},
    {"wrap_mirror", POSITIVE, 20, 0, ck_wrap, 2},
    {"blend_src_alpha_128", POSITIVE, 21, 0, ck_blend, 128},
    {"blend_src_alpha_64", POSITIVE, 22, 0, ck_blend, 64},
    {"control_blend_off", CONTROL_VALUE, 21, 0, ck_blend, -128},
    {"alpha_test_greater_128", POSITIVE, 23, 0, ck_alpha_test, 0},
    {"control_alpha_test_always", CONTROL_VALUE, 23, 0, ck_alpha_test, 1},
    {"tex_rgb5a3_blend", POSITIVE, 24, 0, ck_texture_blend, 0},
    {"tex_cmpr_cutout_alpha_test", POSITIVE, 25, 0, ck_cutout, 0},
    {"control_cmpr_cutout_test_off", CONTROL_DIFF, 25, 0, ck_cutout, 1},
    {"mip_level_by_size", POSITIVE, 26, 0, ck_mip_select, 0},
    {"mip_lod_bias_plus2", POSITIVE, 27, 0, ck_mip_param, 0},
    {"mip_max_lod_1", POSITIVE, 28, 0, ck_mip_param, 1},
    {"mip_min_lod_2", POSITIVE, 29, 0, ck_mip_param, 2},
    {"control_mip_disabled", CONTROL_VALUE, 28, 0, ck_mip_param, 3},
    {"tev_modulate_texture_vertex", POSITIVE, 30, 0, ck_tev, 0},
    {"control_tev_replace", CONTROL_VALUE, 30, 0, ck_tev, 1},
    {"tev_two_stage_add_konst", POSITIVE, 31, 0, ck_tev, 2},
    {"control_tev_second_stage_disabled", CONTROL_VALUE, 31, 0, ck_tev, 3},
    {"dynamic_texture_before", POSITIVE, 32, 1, ck_dynamic, 0},
    {"dynamic_texture_after_flush_invalidate", POSITIVE, 32, 1, ck_dynamic, 1},
};
#define CHECK_COUNT (sizeof(checks) / sizeof(checks[0]))

static void slot_origin(unsigned slot, u32 *X, u32 *Y)
{
    *X = 24 + (slot % 8) * 76;
    *Y = 56 + (slot / 8) * 76;
}

static void run_suite(int display_only)
{
    gallery = display_only;
    for (unsigned i = 0; i < CHECK_COUNT; ++i) {
        const struct check_def *c = &checks[i];
        if (display_only && (c->kind != POSITIVE || c->no_gallery))
            continue;
        u32 X, Y;
        slot_origin(c->slot, &X, &Y);
        if (!display_only)
            check_begin(c->name, c->kind);
        state_reset();
        sample_stack();
        c->run(c, X, Y);
        state_reset();
        if (!display_only)
            check_end();
    }
    gallery = 0;
}

/* ------------------------------------------------------------------------
 * Load / unload with full ownership accounting.
 * ------------------------------------------------------------------------ */

static u64 upload_ticks;

static int load_materials(void)
{
    u64 start = gettime();
    mat = calloc(1, sizeof(*mat));
    if (mat == NULL) {
        ++unexpected_failures;
        scene_log("ALLOC_FAIL heap name=materials bytes=%u\n", (unsigned)sizeof(*mat));
        return 0;
    }
    mat->tlut_handle = -1;
    for (int i = 0; i < T_COUNT; ++i)
        mat->tex[i].handle = -1;
    mat->dxt1 = malloc(16 * 16 / 2);
    mat->tlut = memalign(GX_ALIGN, 32);
    if (mat->dxt1 == NULL || mat->tlut == NULL) {
        ++unexpected_failures;
        scene_log("ALLOC_FAIL heap name=dxt1_or_tlut\n");
        return 0;
    }
    author_dxt1(mat->dxt1, 16, 16);
    for (int i = 0; i < 16; ++i)
        put16((u8 *)&mat->tlut[i], pack565(ci8_palette(i)));
    DCFlushRange(mat->tlut, 32);
    mat->tlut_handle = own("tlut_ci8_rgb565", mat->tlut, 32, GX_ALIGN, "MEM1_heap_CPU_write_DCFlushRange_then_GX_LoadTlut");
    if (mat->tlut_handle < 0)
        return 0;
    for (int i = 0; i < T_COUNT; ++i) {
        const struct texture_def *d = &texture_defs[i];
        struct texture *t = &mat->tex[i];
        t->bytes = texture_bytes(d);
        t->owned_bytes = libogc_bytes(d) > t->bytes ? libogc_bytes(d) : t->bytes;
        t->data = pool_alloc(d->name, t->owned_bytes);
        if (t->data == NULL)
            return 0;
        t->handle = own(d->name, t->data, t->owned_bytes, GX_ALIGN,
                        "MEM2_pool_CPU_write_DCFlushRange_GX_InvalidateTexAll");
        if (t->handle < 0)
            return 0;
        memset(t->data, 0, t->owned_bytes);
        encode_texture(i);
        DCFlushRange(t->data, t->owned_bytes);
        mat->texture_bytes += t->bytes;
        mat->owned_texture_bytes += t->owned_bytes;
    }
    GX_InitTlutObj(&mat->tlut_obj, mat->tlut, GX_TL_RGB565, 16);
    GX_LoadTlut(&mat->tlut_obj, GX_TLUT0);
    GX_InvalidateTexAll();
    upload_ticks = gettime() - start;
    return 1;
}

/* GX must be idle before any buffer it reads is released or reused. */
static unsigned unload_materials(void)
{
    unsigned guard_failures = 0;
    GX_DrawDone();
    if (mat == NULL)
        return 0;
    for (int i = 0; i < T_COUNT; ++i) {
        struct texture *t = &mat->tex[i];
        if (t->data != NULL && !guard_intact(t->data, t->owned_bytes)) {
            ++guard_failures;
            ++unexpected_failures;
            scene_log("GUARD_FAIL name=%s\n", texture_defs[i].name);
        }
        if (t->handle >= 0)
            release(t->handle, 1);
    }
    if (mat->tlut_handle >= 0)
        release(mat->tlut_handle, 1);
    GX_InvalidateTexAll();
    free(mat->tlut);
    free(mat->dxt1);
    free(mat);
    mat = NULL;
    pool_top = pool_base;
    return guard_failures;
}

/* ------------------------------------------------------------------------
 * Deliberate invalid cases: each must be detected for the stated reason.
 * ------------------------------------------------------------------------ */

static unsigned negatives_expected, negatives_detected;

static void negative(const char *name, int detected)
{
    ++negatives_expected;
    negatives_detected += detected ? 1u : 0u;
    scene_log("NEGATIVE case=%s detected=%d\n", name, detected);
}

static int rejected_for(enum reject why, int handle) { return handle < 0 && last_reject == why; }

static void run_negatives(unsigned persistent)
{
    unsigned peak_before = owned_peak;   /* the registry-full case would inflate it */
    expect_rejection = 1;
    u8 *scratch = pool_alloc("negative_scratch", MAX_OWNED * 32);
    u8 *canary = pool_alloc("negative_canary", 64);
    if (scratch == NULL || canary == NULL) {
        ++unexpected_failures;
        expect_rejection = 0;
        return;
    }
    u8 *texels = mat->tex[T_565].data;
    negative("misaligned_base", rejected_for(REJ_ALIGN, own("neg_misaligned", scratch + 4, 64, GX_ALIGN, "-")));
    negative("unaligned_length", rejected_for(REJ_ALIGN, own("neg_length", scratch, 40, GX_ALIGN, "-")));
    negative("overlaps_owned_texture", rejected_for(REJ_OVERLAP, own("neg_overlap", texels + 32, 32, GX_ALIGN, "-")));
    negative("overlaps_gx_fifo", rejected_for(REJ_OVERLAP, own("neg_fifo", (void *)owned[0].base, 32, GX_ALIGN, "-")));
    negative("outside_mem1_mem2", rejected_for(REJ_REGION, own("neg_region", (void *)0x81800000, 32, GX_ALIGN, "-")));
    negative("range_wraps", rejected_for(REJ_WRAP, own("neg_wrap", scratch, 0xFFFFFFE0u, GX_ALIGN, "-")));
    negative("zero_length", rejected_for(REJ_ZERO, own("neg_zero", scratch, 0, GX_ALIGN, "-")));
    negative("null_base", rejected_for(REJ_NULL, own("neg_null", NULL, 32, GX_ALIGN, "-")));
    negative("texture_from_unowned_memory",
             !owned_contains("neg_unowned_texture", scratch, 128) && last_reject == REJ_UNOWNED);
    negative("read_past_owned_end", !owned_contains("neg_past_end", texels, mat->tex[T_565].bytes + 32) &&
                                        last_reject == REJ_UNOWNED);
    int handle = own("neg_release_twice", scratch, 32, GX_ALIGN, "-");
    int first = release(handle, 1);
    negative("double_release", handle >= 0 && first && !release(handle, 1) && last_reject == REJ_RELEASE);
    negative("release_unknown_handle", !release(MAX_OWNED + 3, 1) && last_reject == REJ_RELEASE);
    /* Leak detector: a registered range left behind must be counted. */
    handle = own("neg_leak", scratch, 32, GX_ALIGN, "-");
    unsigned leaked = owned_active - persistent - (T_COUNT + 1);   /* textures + TLUT are legitimately live */
    negative("leaked_range_counted", handle >= 0 && leaked == 1);
    release(handle, 1);
    /* Overrun detector: one byte past the payload corrupts the guard. */
    canary[64] = 0;
    negative("overrun_guard", !guard_intact(canary, 64));
    unsigned failures_before = pool_failures;
    negative("pool_exhaustion", pool_alloc("neg_pool_exhaustion", POOL_BYTES * 2) == NULL &&
                                    pool_failures == failures_before + 1);
    void *huge = memalign(GX_ALIGN, 0x7FFFFFE0u);
    negative("heap_allocation_failure", huge == NULL);
    free(huge);
    /* Registry capacity: fill every free slot, then one more must fail. */
    int handles[MAX_OWNED], count = 0;
    while (count < MAX_OWNED) {
        int h = own("neg_fill", scratch + 32 * count, 32, GX_ALIGN, "-");
        if (h < 0)
            break;
        handles[count++] = h;
    }
    negative("registry_full", count < MAX_OWNED && last_reject == REJ_FULL);
    for (int i = 0; i < count; ++i)
        release(handles[i], 1);
    owned_peak = peak_before;
    expect_rejection = 0;
    scene_log("NEGATIVES detected=%u expected=%u rejections_logged=%u\n", negatives_detected, negatives_expected,
              rejections);
}

/* ------------------------------------------------------------------------
 * HUD (3x5 hex glyphs, as in the GX scene) and storage.
 * ------------------------------------------------------------------------ */

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
                vtx(left, top, -0.1f, colour, 0, 0);
                vtx(left + cell, top, -0.1f, colour, 0, 0);
                vtx(left + cell, top + cell, -0.1f, colour, 0, 0);
                vtx(left, top + cell, -0.1f, colour, 0, 0);
            }
    }
    GX_End();
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

/* 1 with persistent logging, 0 when SD is absent, -1 on a storage error. An
 * invalid run counter is preserved rather than overwritten. */
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

static u32 us(u64 ticks) { return (u32)ticks_to_microsecs(ticks); }

int main(void)
{
    stack_entry = stack_lowest = (uintptr_t)__builtin_frame_address(0);
    VIDEO_Init();
    PAD_Init();
    WPAD_Init();
    mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL)
        return 1;
    struct mallinfo heap_start = mallinfo();

    unsigned previous_runs = 0;
    int storage = init_storage(&previous_runs);
    scene_log("BEGIN target=gx_materials build=%s previous_runs=%u storage=%d video=%ux%u efb_height=%u mode=%u aa=%u\n",
              WII_BUILD_ID, previous_runs, storage, mode->fbWidth, mode->xfbHeight, mode->efbHeight,
              mode->viTVMode, mode->aa);
    scene_log("ARENAS mem1=%p..%p bytes=%u mem2=%p..%p bytes=%u\n", SYS_GetArena1Lo(), SYS_GetArena1Hi(),
              SYS_GetArena1Size(), SYS_GetArena2Lo(), SYS_GetArena2Hi(), SYS_GetArena2Size());

    void *fifo = memalign(GX_ALIGN, FIFO_BYTES);
    u32 xfb_bytes = VIDEO_GetFrameBufferSize(mode);
    void *xfb_cached[2] = {SYS_AllocateFramebuffer(mode), SYS_AllocateFramebuffer(mode)};
    if (fifo == NULL || xfb_cached[0] == NULL || xfb_cached[1] == NULL) {
        scene_log("ALLOC_FAIL fifo=%p xfb0=%p xfb1=%p\n", fifo, xfb_cached[0], xfb_cached[1]);
        if (record != NULL)
            fclose(record);
        return 1;
    }
    own("gx_fifo", fifo, FIFO_BYTES, GX_ALIGN, "zeroed_then_DCFlushRange_GP_reads");
    own("xfb0", xfb_cached[0], xfb_bytes, GX_ALIGN, "K1_uncached_CPU_never_writes_GX_copy_target");
    own("xfb1", xfb_cached[1], xfb_bytes, GX_ALIGN, "K1_uncached_CPU_never_writes_GX_copy_target");
    const unsigned persistent = owned_active;
    void *xfb[2] = {MEM_K0_TO_K1(xfb_cached[0]), MEM_K0_TO_K1(xfb_cached[1])};
    fifo_physical = (u32)MEM_VIRTUAL_TO_PHYSICAL(fifo);
    memset(fifo, 0, FIFO_BYTES);
    DCFlushRange(fifo, FIFO_BYTES);

    /* MEM2 texture pool carved from the arena; restored at exit. */
    void *arena2_saved = SYS_GetArena2Lo();
    pool_base = ((uintptr_t)arena2_saved + 31u) & ~(uintptr_t)31u;
    pool_end = pool_base + POOL_BYTES;
    if (pool_end > (uintptr_t)SYS_GetArena2Hi()) {
        scene_log("ALLOC_FAIL mem2_pool bytes=%u\n", POOL_BYTES);
        if (record != NULL)
            fclose(record);
        return 1;
    }
    SYS_SetArena2Lo((void *)pool_end);
    pool_top = pool_base;
    scene_log("POOL mem2=%p..%p bytes=%u guard=%u\n", (void *)pool_base, (void *)pool_end, POOL_BYTES, GUARD_BYTES);

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
    GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
    GX_SetDispCopyDst(mode->fbWidth, copy_lines);
    GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
    GX_SetFieldMode(mode->field_rendering, mode->viHeight == 2 * mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(mode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDither(GX_FALSE);
    GX_SetAlphaUpdate(GX_TRUE);
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
    scratch_xfb = xfb[1];
    state_reset();
    GX_CopyDisp(scratch_xfb, GX_TRUE);
    GX_DrawDone();

    /* Texture size accounting against libogc's helper. A disagreement is a
     * reported finding; the allocation takes the larger value so neither
     * interpretation can make GX read outside the owned range. */
    u32 size_disagreements = 0;
    for (int i = 0; i < T_COUNT; ++i) {
        const struct texture_def *d = &texture_defs[i];
        u32 mine = texture_bytes(d), libogc = libogc_bytes(d);
        if (mine != libogc)
            ++size_disagreements;
        scene_log("TEXBYTES name=%s fmt=%u size=%ux%u levels=%u computed=%u libogc=%u match=%d\n", d->name,
                  d->fmt, d->w, d->h, d->levels, (unsigned)mine, (unsigned)libogc, mine == libogc);
    }
    /* The first %f allocates newlib's dtoa state; take it before the baseline. */
    scene_log("EFB format=%s width=%u height=%u colour_bytes=%u z_bytes=%u location=GPU_embedded_not_main_memory "
              "scale=%.3f\n", mode->aa ? "RGB565_Z16" : "RGB8_Z24", mode->fbWidth, mode->efbHeight,
              (unsigned)(mode->fbWidth * mode->efbHeight * 3), (unsigned)(mode->fbWidth * mode->efbHeight * 3),
              y_scale);
    scene_log("XFB count=2 bytes_each=%u total=%u disp_copy_yscale_return=%u\n", (unsigned)xfb_bytes, (unsigned)(2 * xfb_bytes),
              (unsigned)copy_lines);
    if (record != NULL && fflush(record) != 0)
        log_failed = 1;

    /* Repeated load / check / unload cycles. */
    const int baseline = mallinfo().uordblks;
    int heap_peak = baseline, max_growth = 0;
    u32 crcs[CYCLES] = {0};
    unsigned cycle_passed[CYCLES] = {0}, guard_failures = 0, leaks = 0;
    for (unsigned cycle = 1; cycle <= CYCLES; ++cycle) {
        cycle_number = cycle;
        cycle_crc = 0;
        unsigned passed_before = stats.passed, total_before = stats.total;
        stats.fifo_check_peak = 0;
        stats.fifo_total = 0;
        stats.check_ticks = 0;
        int heap_before = mallinfo().uordblks;
        int loaded = load_materials();
        int heap_loaded = mallinfo().uordblks;
        if (heap_loaded > heap_peak)
            heap_peak = heap_loaded;
        u32 texture_total = mat != NULL ? mat->texture_bytes : 0;
        u32 texture_owned = mat != NULL ? mat->owned_texture_bytes : 0;
        if (loaded) {
            run_suite(0);
            if (cycle == 1)
                run_negatives(persistent);
        } else {
            ++unexpected_failures;
            scene_log("LOAD_FAIL cycle=%u\n", cycle);
        }
        u32 pool_used = (u32)(pool_top - pool_base);
        guard_failures += unload_materials();
        unsigned leaked = owned_active - persistent;
        if (leaked) {
            ++leaks;
            ++unexpected_failures;
            scene_log("LEAK cycle=%u ranges=%u\n", cycle, leaked);
        }
        int heap_after = mallinfo().uordblks;
        if (heap_after - baseline > max_growth)
            max_growth = heap_after - baseline;
        crcs[cycle - 1] = cycle_crc;
        cycle_passed[cycle - 1] = stats.passed - passed_before;
        scene_log("CYCLE n=%u checks_passed=%u total=%u crc=%08x heap_before=%d heap_loaded=%d heap_after=%d "
                  "texture_bytes=%u texture_owned_bytes=%u pool_used=%u pool_peak=%u owned_peak=%u leaked_ranges=%u upload_us=%u "
                  "checks_us=%u fifo_check_peak=%u fifo_cycle_total=%u\n",
                  cycle, cycle_passed[cycle - 1], stats.total - total_before, (unsigned)cycle_crc, heap_before,
                  heap_loaded, heap_after, (unsigned)texture_total, (unsigned)texture_owned, (unsigned)pool_used, (unsigned)pool_peak,
                  owned_peak, leaked, us(upload_ticks), us(stats.check_ticks), (unsigned)stats.fifo_check_peak,
                  (unsigned)stats.fifo_total);
    }
    int crc_stable = 1;
    for (unsigned i = 1; i < CYCLES; ++i)
        crc_stable &= crcs[i] == crcs[0];
    scene_log("PEEK count=%u avg_ns=%u scope=CPU_EFB_peek_emulated_timebase_not_hardware_timing\n", peek_count,
              peek_count ? (unsigned)(ticks_to_nanosecs(peek_ticks) / peek_count) : 0);
    scene_log("SUMMARY checks_passed=%u total=%u per_cycle=%u cycles=%u crc_stable=%d heap_growth_max=%d "
              "heap_peak=%d negatives=%u/%u guard_failures=%u leaks=%u libogc_size_disagreements=%u unexpected_failures=%u\n",
              stats.passed, stats.total, (unsigned)CHECK_COUNT, CYCLES, crc_stable, max_growth, heap_peak,
              negatives_detected, negatives_expected, guard_failures, leaks, (unsigned)size_disagreements,
              unexpected_failures);
    if (record != NULL && fflush(record) != 0)
        log_failed = 1;

    /* Interactive gallery: every positive configuration drawn each frame. */
    int display_loaded = load_materials();
    if (!display_loaded)
        ++unexpected_failures;
    int heap_display = mallinfo().uordblks;
    unsigned frames = 0, connected = 0, activity = 0, overlay_on = 0, spin = 0, toggles = 0;
    u32 fifo_peak = 0;
    u64 fifo_sum = 0, copy_sum = 0, submit_sum = 0, copy_max = 0, submit_max = 0;
    const char *exit_reason = "system_event";
    char line[48];
    int fb = 0;
    f32 angle = 0;
    while (SYS_MainLoop()) {
        u32 present = PAD_ScanPads();
        WPAD_ScanPads();
        connected |= present;
        u32 down = PAD_ButtonsDown(0), held = PAD_ButtonsHeld(0), wdown = WPAD_ButtonsDown(0);
        if (held || PAD_StickX(0) || PAD_StickY(0) || WPAD_ButtonsHeld(0))
            activity |= 1;
        if ((down & PAD_BUTTON_A) || (wdown & WPAD_BUTTON_A)) {
            spin = !spin;
            ++toggles;
            scene_log("INPUT frame=%u spin=%u\n", frames, spin);
        }
        if ((down & PAD_BUTTON_B) || (wdown & WPAD_BUTTON_B)) {
            overlay_on = !overlay_on;
            ++toggles;
            scene_log("INPUT frame=%u resource_overlay=%u\n", frames, overlay_on);
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
        u64 start = gettime();
        u32 write_before = PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK;
        if (display_loaded)
            run_suite(1);
        state_reset();
        if (spin && display_loaded &&
            owned_contains("tex_mip_rgb565", mat->tex[T_MIP].data, mat->tex[T_MIP].bytes)) {
            /* A trilinear, mipmapped floor receding in perspective below the
             * gallery, with scrolling texture coordinates. Same conventions as
             * the perspective check: identity view, own viewport. */
            angle += 0.02f;
            const u32 top = 360, height = mode->efbHeight > top + 8 ? mode->efbHeight - top - 8 : 8;
            Mtx44 perspective;
            guPerspective(perspective, 60, (f32)(mode->fbWidth - 48) / height, 0.1f, 100.0f);
            GX_SetViewport(24, top, mode->fbWidth - 48, height, 0, 1);
            GX_SetScissor(24, top, mode->fbWidth - 48, height);
            GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);
            GXTexObj obj;
            GX_InitTexObj(&obj, mat->tex[T_MIP].data, 32, 32, GX_TF_RGB565, GX_REPEAT, GX_REPEAT, GX_TRUE);
            GX_InitTexObjLOD(&obj, GX_LIN_MIP_LIN, GX_LINEAR, 0, 5, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
            use_texture(&obj, GX_TEXMAP0, GX_REPLACE);
            f32 scroll = fmodf(angle, 1.0f);
            GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            vtx(-3, -0.5f, -12, white, 0, scroll); vtx(3, -0.5f, -12, white, 4, scroll);
            vtx(3, -0.5f, -1.2f, white, 4, 12 + scroll); vtx(-3, -0.5f, -1.2f, white, 0, 12 + scroll);
            GX_End();
            state_reset();
        }
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        draw_text(WII_BUILD_ID, 24, 12, 4, (GXColor){255, 220, 64, 255});
        snprintf(line, sizeof(line), "%u-%u-%u", stats.passed, stats.total, frames);
        draw_text(line, 24, 34, 4, (GXColor){240, 240, 240, 255});
        if (overlay_on) {
            /* Resource view: pool use, FIFO this frame and heap, as bars. */
            f32 pool = 200.0f * (f32)(pool_top - pool_base) / POOL_BYTES;
            f32 fifo_bar = 200.0f * (f32)(fifo_peak > 65536 ? 65536 : fifo_peak) / 65536.0f;
            rect(400, 12, 400 + pool, 18, -0.1f, (GXColor){64, 220, 255, 255});
            rect(400, 22, 400 + fifo_bar, 28, -0.1f, (GXColor){255, 160, 64, 255});
            rect(400, 32, 400 + 200.0f * owned_active / MAX_OWNED, 38, -0.1f, (GXColor){160, 255, 96, 255});
        }
        GX_DrawDone();
        u32 submitted = (PI_FIFO_WRITE_POINTER & PI_FIFO_ADDRESS_MASK);
        submitted = (submitted - write_before + FIFO_BYTES) % FIFO_BYTES;
        if (submitted > fifo_peak)
            fifo_peak = submitted;
        fifo_sum += submitted;
        u64 rendered = gettime();
        GX_CopyDisp(xfb[fb], GX_TRUE);
        GX_DrawDone();
        u64 copied = gettime();
        VIDEO_SetNextFramebuffer(xfb[fb]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        fb ^= 1;
        u64 submit = rendered - start, copy = copied - rendered;
        submit_sum += submit;
        copy_sum += copy;
        if (submit > submit_max) submit_max = submit;
        if (copy > copy_max) copy_max = copy;
        ++frames;
    }
    int heap_loop_end = mallinfo().uordblks;
    guard_failures += unload_materials();
    if (owned_active != persistent) {
        ++unexpected_failures;
        scene_log("LEAK display ranges=%u\n", owned_active - persistent);
    }
    int arena_intact = SYS_GetArena2Lo() == (void *)pool_end;
    SYS_SetArena2Lo(arena2_saved);
    if (!arena_intact)
        ++unexpected_failures;
    int heap_end = mallinfo().uordblks;
    int heap_growth = heap_end - baseline;
    if (heap_end - baseline > max_growth)
        max_growth = heap_end - baseline;
    scene_log("FIFO bytes=%u gallery_per_frame_peak=%u avg=%u check_pass_peak_last_cycle=%u "
              "scope=PI_write_pointer_delta_excluding_copy\n", FIFO_BYTES, (unsigned)fifo_peak,
              frames ? (unsigned)(fifo_sum / frames) : 0, (unsigned)stats.fifo_check_peak);
    scene_log("TIMING frames=%u gallery_submit_us_avg=%u max=%u efb_to_xfb_copy_us_avg=%u max=%u "
              "scope=CPU_submit_plus_DrawDone_in_Dolphin\n", frames,
              frames ? us(submit_sum) / frames : 0, us(submit_max), frames ? us(copy_sum) / frames : 0,
              us(copy_max));
    scene_log("STACK entry=0x%08x lowest_sampled=0x%08x depth_sampled=%u scope=frame_address_samples_not_peak\n",
              (unsigned)stack_entry, (unsigned)stack_lowest, (unsigned)(stack_entry - stack_lowest));
    scene_log("HEAP start=%d baseline=%d peak=%d display_loaded=%d loop_end=%d end=%d growth=%d "
              "loop_growth=%d max_cycle_growth=%d arena2_restored=%d\n", heap_start.uordblks, baseline, heap_peak,
              heap_display, heap_loop_end, heap_end, heap_growth, heap_loop_end - heap_display, max_growth,
              arena_intact);
    int success = stats.total == CYCLES * CHECK_COUNT && stats.passed == stats.total && crc_stable &&
                  negatives_detected == negatives_expected && negatives_expected > 0 && unexpected_failures == 0 &&
                  guard_failures == 0 && max_growth == 0 &&
                  heap_loop_end == heap_display && storage >= 0;
    scene_log("END target=gx_materials build=%s frames=%u connected=%x activity=%x toggles=%u exit=%s storage=%d "
              "checks=%u/%u negatives=%u/%u unexpected=%u heap_growth=%d crc_stable=%d result=%s\n",
              WII_BUILD_ID, frames, connected, activity, toggles, exit_reason, storage, stats.passed, stats.total,
              negatives_detected, negatives_expected, unexpected_failures, max_growth, crc_stable,
              success ? "pass" : "fail");
    if (record != NULL && (fclose(record) != 0 || log_failed))
        return 1;
    return success ? 0 : 1;
}
