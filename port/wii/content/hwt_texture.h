/* HWT1 texture container loader (HWI-008C).
 *
 * Layout (tools/wii/gx_texture.py pack_hwt, docs/wii/CONTENT-PIPELINE.md):
 * a 64-byte big-endian header, then GX-tiled images in face, level, slice
 * order. Each image is one 2D GX texture level; sizes follow the GX tile
 * rules, so every image starts 32-byte aligned and the levels of one face of
 * a 2D or cube texture are a contiguous GX mipmap chain. */
#ifndef HALO_WII_HWT_TEXTURE_H
#define HALO_WII_HWT_TEXTURE_H

#include "content_common.h"

#define HWT_HEADER_BYTES 64u
#define HWT_MAX_DIMENSION 4096u
#define HWT_MAX_DEPTH 512u
#define HWT_GX_MAX_DIMENSION 1024u

/* libogc GX_TF_* numbering */
enum { HWT_GX_I8 = 1, HWT_GX_IA8 = 3, HWT_GX_RGB565 = 4, HWT_GX_RGB5A3 = 5, HWT_GX_RGBA8 = 6, HWT_GX_CMPR = 14 };
enum { HWT_TYPE_2D = 0, HWT_TYPE_3D = 1, HWT_TYPE_CUBE = 2 };

struct hwt_header {
    uint32_t version, gx_format, width, height, depth, faces, levels, images, source_format, type;
    uint32_t data_offset, data_bytes;
};

struct hwt_image {
    uint32_t face, level, slice, width, height;
    uint32_t offset;    /* from the start of the file */
    uint32_t bytes;
    int gx_loadable;    /* both dimensions within GX's 1024 */
};

/* Bytes of one GX-tiled level; 0 for an unknown format. */
uint32_t hwt_level_bytes(uint32_t gx_format, uint32_t width, uint32_t height);

/* Validates the whole container: header fields, enums, the source-to-GX
 * profile mapping, dimensions, level and image counts, reserved bytes and
 * that the images exactly fill the data. */
enum content_error hwt_parse(const unsigned char *data, uint32_t bytes, struct hwt_header *out);

/* Image index (0 <= index < images) of a parsed header. */
enum content_error hwt_image_at(const struct hwt_header *header, uint32_t index, struct hwt_image *out);

/* RGBA the GX sampler returns for texel (x, y) of one tiled image (the GX
 * decode rules, including CMPR's 5/8 + 3/8 interpolation). */
void hwt_decode_texel(uint32_t gx_format, const unsigned char *image, uint32_t width, uint32_t x, uint32_t y,
                      unsigned char rgba[4]);

#endif
