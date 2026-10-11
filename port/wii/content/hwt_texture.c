/* HWT1 texture container loader (HWI-008C; version 2 split alpha,
 * HWI-008F). See hwt_texture.h. */
#include "hwt_texture.h"

#include <string.h>

static int tile_shape(uint32_t gx_format, uint32_t *tw, uint32_t *th, uint32_t *tile_bytes)
{
    switch (gx_format) {
    case HWT_GX_I4: *tw = 8; *th = 8; *tile_bytes = 32; return 1;
    case HWT_GX_I8: *tw = 8; *th = 4; *tile_bytes = 32; return 1;
    case HWT_GX_IA8: case HWT_GX_RGB565: case HWT_GX_RGB5A3: *tw = 4; *th = 4; *tile_bytes = 32; return 1;
    case HWT_GX_RGBA8: *tw = 4; *th = 4; *tile_bytes = 64; return 1;
    case HWT_GX_CMPR: *tw = 8; *th = 8; *tile_bytes = 32; return 1;
    default: return 0;
    }
}

uint32_t hwt_level_bytes(uint32_t gx_format, uint32_t width, uint32_t height)
{
    uint32_t tw, th, tb;
    if (!tile_shape(gx_format, &tw, &th, &tb))
        return 0;
    return ((width + tw - 1) / tw) * ((height + th - 1) / th) * tb;
}

/* gx-baseline-v1 (version 1): the GX format each Halo bitmap format
 * converts to. Version 2 (gx-splitalpha-v1) is DXT3/DXT5 to CMPR only. */
static uint32_t profile_target(uint32_t version, uint32_t source_format)
{
    if (version == 2)
        return source_format == 15 || source_format == 16 ? HWT_GX_CMPR : 0;
    switch (source_format) {
    case 0: case 1: case 3: return HWT_GX_IA8;           /* a8, y8, a8y8 */
    case 2: return HWT_GX_I8;                            /* ay8 */
    case 6: return HWT_GX_RGB565;                        /* r5g6b5 */
    case 8: case 9: return HWT_GX_RGB5A3;                /* a1r5g5b5, a4r4g4b4 */
    case 10: case 11: case 15: case 16: case 17: return HWT_GX_RGBA8; /* x8r8g8b8 a8r8g8b8 dxt3 dxt5 p8_bump */
    case 14: return HWT_GX_CMPR;                         /* dxt1 */
    default: return 0;
    }
}

static uint32_t floor_log2(uint32_t value)
{
    uint32_t result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

static uint32_t level_dim(uint32_t value, uint32_t level) { return (value >> level) ? (value >> level) : 1u; }

enum content_error hwt_parse(const unsigned char *data, uint32_t bytes, struct hwt_header *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    if (bytes < HWT_HEADER_BYTES)
        return CONTENT_TRUNCATED;
    if (memcmp(data, "HWT1", 4) != 0)
        return CONTENT_MAGIC;
    struct hwt_header h;
    h.version = content_be16(data + 4);
    h.gx_format = content_be16(data + 6);
    h.width = content_be16(data + 8);
    h.height = content_be16(data + 10);
    h.depth = content_be16(data + 12);
    h.faces = content_be16(data + 14);
    h.levels = content_be16(data + 16);
    h.images = content_be16(data + 18);
    h.source_format = content_be16(data + 20);
    h.type = content_be16(data + 22);
    h.data_offset = content_be32(data + 24);
    h.data_bytes = content_be32(data + 28);
    h.alpha_kind = HWT_ALPHA_NONE;
    h.alpha_gx_format = h.alpha_offset = h.alpha_bytes = 0;
    if (h.version != 1 && h.version != 2)
        return CONTENT_VERSION;
    uint32_t tw, th, tb;
    if (!tile_shape(h.gx_format, &tw, &th, &tb) || h.gx_format == HWT_GX_I4 || h.type > HWT_TYPE_CUBE ||
        profile_target(1, h.source_format) == 0)
        return CONTENT_ENUM;
    if (profile_target(h.version, h.source_format) != h.gx_format)
        return CONTENT_ENUM;
    uint32_t reserved_from = 32;
    if (h.version == 2) {
        uint32_t section = content_be16(data + 32), reserved = content_be16(data + 38);
        h.alpha_kind = content_be16(data + 34);
        h.alpha_gx_format = content_be16(data + 36);
        h.alpha_offset = content_be32(data + 40);
        h.alpha_bytes = content_be32(data + 44);
        if (section != 1)
            return CONTENT_VERSION;
        if (h.alpha_kind != HWT_ALPHA_OMITTED && h.alpha_kind != HWT_ALPHA_PLANE)
            return CONTENT_ENUM;
        if (reserved != 0)
            return CONTENT_RESERVED;
        if (h.alpha_kind == HWT_ALPHA_OMITTED) {
            if (h.alpha_gx_format != 0)
                return CONTENT_ENUM;
            if (h.alpha_offset != 0 || h.alpha_bytes != 0)
                return CONTENT_LENGTH;
        } else if ((h.alpha_gx_format != HWT_GX_I4 && h.alpha_gx_format != HWT_GX_I8) ||
                   (h.source_format == 15 && h.alpha_gx_format != HWT_GX_I4)) {
            return CONTENT_ENUM;   /* DXT3's explicit 4-bit alpha is I4 */
        }
        reserved_from = 48;
    }
    for (uint32_t i = reserved_from; i < HWT_HEADER_BYTES; ++i)
        if (data[i] != 0)
            return CONTENT_RESERVED;
    uint32_t depth_limit = h.type == HWT_TYPE_3D ? HWT_MAX_DEPTH : 1u;
    if (h.width == 0 || h.height == 0 || h.depth == 0 || h.width > HWT_MAX_DIMENSION || h.height > HWT_MAX_DIMENSION ||
        h.depth > depth_limit || (h.type == HWT_TYPE_CUBE && h.width != h.height) ||
        h.faces != (h.type == HWT_TYPE_CUBE ? 6u : 1u))
        return CONTENT_DIMENSIONS;
    uint32_t largest = h.width > h.height ? h.width : h.height;
    if (h.depth > largest)
        largest = h.depth;
    if (h.levels == 0 || h.levels > floor_log2(largest) + 1)
        return CONTENT_DIMENSIONS;
    if (h.data_offset != HWT_HEADER_BYTES)
        return CONTENT_LENGTH;
    if (h.data_bytes > bytes - HWT_HEADER_BYTES || h.data_bytes + h.alpha_bytes != bytes - HWT_HEADER_BYTES)
        return CONTENT_LENGTH;
    if (h.alpha_kind == HWT_ALPHA_PLANE && h.alpha_offset != HWT_HEADER_BYTES + h.data_bytes)
        return CONTENT_LENGTH;
    uint32_t images = 0;
    uint64_t total = 0, alpha_total = 0;
    for (uint32_t level = 0; level < h.levels; ++level) {
        uint32_t slices = level_dim(h.depth, level), w = level_dim(h.width, level), hh = level_dim(h.height, level);
        images += slices;
        total += (uint64_t)hwt_level_bytes(h.gx_format, w, hh) * slices;
        if (h.alpha_kind == HWT_ALPHA_PLANE)
            alpha_total += (uint64_t)hwt_level_bytes(h.alpha_gx_format, w, hh) * slices;
    }
    images *= h.faces;
    total *= h.faces;
    alpha_total *= h.faces;
    if (images != h.images)
        return CONTENT_COUNT;
    if (total != h.data_bytes || alpha_total != h.alpha_bytes)
        return CONTENT_LENGTH;
    if (h.version == 2) {
        /* every CMPR block in the four-colour form: c0 > c1, or c0 == c1 with
        every index 0 (tile padding blocks are zero, which is the latter) */
        const unsigned char *block = data + HWT_HEADER_BYTES;
        for (uint32_t i = 0; i < h.data_bytes / 8u; ++i, block += 8) {
            uint32_t c0 = content_be16(block), c1 = content_be16(block + 2);
            if (c0 < c1 || (c0 == c1 && content_be32(block + 4) != 0))
                return CONTENT_VALUE;
        }
    }
    *out = h;
    return CONTENT_OK;
}

static enum content_error image_in(const struct hwt_header *header, uint32_t gx_format, uint32_t first,
                                   uint32_t index, struct hwt_image *out)
{
    if (header == NULL || out == NULL || index >= header->images)
        return CONTENT_ARGUMENT;
    uint32_t offset = first, n = 0;
    for (uint32_t face = 0; face < header->faces; ++face)
        for (uint32_t level = 0; level < header->levels; ++level) {
            uint32_t w = level_dim(header->width, level), h = level_dim(header->height, level);
            uint32_t bytes = hwt_level_bytes(gx_format, w, h);
            for (uint32_t slice = 0; slice < level_dim(header->depth, level); ++slice, ++n) {
                if (n == index) {
                    out->face = face;
                    out->level = level;
                    out->slice = slice;
                    out->width = w;
                    out->height = h;
                    out->offset = offset;
                    out->bytes = bytes;
                    out->gx_loadable = w <= HWT_GX_MAX_DIMENSION && h <= HWT_GX_MAX_DIMENSION;
                    return CONTENT_OK;
                }
                offset += bytes;
            }
        }
    return CONTENT_ARGUMENT;
}

enum content_error hwt_image_at(const struct hwt_header *header, uint32_t index, struct hwt_image *out)
{
    if (header == NULL)
        return CONTENT_ARGUMENT;
    return image_in(header, header->gx_format, HWT_HEADER_BYTES, index, out);
}

enum content_error hwt_alpha_image_at(const struct hwt_header *header, uint32_t index, struct hwt_image *out)
{
    if (header == NULL || header->alpha_kind != HWT_ALPHA_PLANE)
        return CONTENT_ARGUMENT;
    return image_in(header, header->alpha_gx_format, header->alpha_offset, index, out);
}

static unsigned char e5(uint32_t v) { return (unsigned char)((v << 3) | (v >> 2)); }
static unsigned char e6(uint32_t v) { return (unsigned char)((v << 2) | (v >> 4)); }
static unsigned char e3(uint32_t v) { return (unsigned char)((v << 5) | (v << 2) | (v >> 1)); }

static void rgb565(uint32_t v, unsigned char rgba[4])
{
    rgba[0] = e5((v >> 11) & 31);
    rgba[1] = e6((v >> 5) & 63);
    rgba[2] = e5(v & 31);
    rgba[3] = 255;
}

void hwt_decode_texel(uint32_t gx_format, const unsigned char *image, uint32_t width, uint32_t x, uint32_t y,
                      unsigned char rgba[4])
{
    uint32_t tw, th, tb;
    if (!tile_shape(gx_format, &tw, &th, &tb)) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return;
    }
    uint32_t tiles_per_row = (width + tw - 1) / tw;
    const unsigned char *tile = image + ((y / th) * tiles_per_row + (x / tw)) * tb;
    uint32_t ix = x % tw, iy = y % th;
    if (gx_format == HWT_GX_CMPR) {
        const unsigned char *block = tile + ((iy / 4) * 2 + (ix / 4)) * 8;
        uint32_t c0 = content_be16(block), c1 = content_be16(block + 2);
        unsigned char a[4], b[4];
        rgb565(c0, a);
        rgb565(c1, b);
        uint32_t index = (block[4 + iy % 4] >> (6 - 2 * (ix % 4))) & 3u;
        for (int c = 0; c < 3; ++c) {
            if (index == 0)
                rgba[c] = a[c];
            else if (index == 1)
                rgba[c] = b[c];
            else if (c0 > c1)
                rgba[c] = (unsigned char)(index == 2 ? (5 * a[c] + 3 * b[c]) >> 3 : (3 * a[c] + 5 * b[c]) >> 3);
            else
                rgba[c] = (unsigned char)(index == 2 ? (a[c] + b[c]) >> 1 : 0);
        }
        rgba[3] = (c0 <= c1 && index == 3) ? 0 : 255;
        return;
    }
    uint32_t within = iy * tw + ix;
    if (gx_format == HWT_GX_I4) {
        uint32_t v = tile[within / 2];
        v = (within % 2 == 0) ? v >> 4 : v & 15u;
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = (unsigned char)(v | (v << 4));
        return;
    }
    if (gx_format == HWT_GX_I8) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = tile[within];
        return;
    }
    if (gx_format == HWT_GX_RGBA8) {
        rgba[3] = tile[2 * within];
        rgba[0] = tile[2 * within + 1];
        rgba[1] = tile[32 + 2 * within];
        rgba[2] = tile[32 + 2 * within + 1];
        return;
    }
    uint32_t v = content_be16(tile + 2 * within);
    if (gx_format == HWT_GX_IA8) {
        rgba[0] = rgba[1] = rgba[2] = (unsigned char)(v & 0xFF);
        rgba[3] = (unsigned char)(v >> 8);
    } else if (gx_format == HWT_GX_RGB565) {
        rgb565(v, rgba);
    } else if (v & 0x8000) {
        rgba[0] = e5((v >> 10) & 31);
        rgba[1] = e5((v >> 5) & 31);
        rgba[2] = e5(v & 31);
        rgba[3] = 255;
    } else {
        rgba[0] = (unsigned char)(((v >> 8) & 15) * 17);
        rgba[1] = (unsigned char)(((v >> 4) & 15) * 17);
        rgba[2] = (unsigned char)((v & 15) * 17);
        rgba[3] = e3((v >> 12) & 7);
    }
}

void hwt_decode_rgba(const struct hwt_header *header, const unsigned char *file, uint32_t index, uint32_t x,
                     uint32_t y, unsigned char rgba[4])
{
    struct hwt_image image;
    if (hwt_image_at(header, index, &image) != CONTENT_OK) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return;
    }
    hwt_decode_texel(header->gx_format, file + image.offset, image.width, x, y, rgba);
    if (header->alpha_kind == HWT_ALPHA_OMITTED) {
        rgba[3] = 255;
    } else if (header->alpha_kind == HWT_ALPHA_PLANE && hwt_alpha_image_at(header, index, &image) == CONTENT_OK) {
        unsigned char alpha[4];
        hwt_decode_texel(header->alpha_gx_format, file + image.offset, image.width, x, y, alpha);
        rgba[3] = alpha[3];
    }
}
