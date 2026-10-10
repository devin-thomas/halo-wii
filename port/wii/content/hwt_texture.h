/* HWT1 texture container loader (HWI-008C; version 2 split alpha, HWI-008F).
 *
 * Layout (tools/wii/gx_texture.py pack_hwt, docs/wii/CONTENT-PIPELINE.md):
 * a 64-byte big-endian header, then GX-tiled images in face, level, slice
 * order. Each image is one 2D GX texture level; sizes follow the GX tile
 * rules, so every image starts 32-byte aligned and the levels of one face of
 * a 2D or cube texture are a contiguous GX mipmap chain.
 *
 * Version 1 (gx-baseline-v1, and every non-DXT3/DXT5 texture of
 * gx-splitalpha-v1): one image set in the header's GX format; header bytes
 * 32..63 are zero.
 *
 * Version 2 (gx-splitalpha-v1, DXT3/DXT5 only): the images are CMPR colour,
 * and header bytes 32..47 are the alpha section:
 *   32 u16 section version (1)
 *   34 u16 kind: 1 = no plane, the alpha is 255 at every texel;
 *                2 = plane present
 *   36 u16 plane GX format: I4 (0) or I8 (1); DXT3 is always I4; 0 when kind 1
 *   38 u16 reserved (0)
 *   40 u32 plane offset from the file start: 64 + data bytes; 0 when kind 1
 *   44 u32 plane bytes; 0 when kind 1
 * The plane holds one image per colour image, in the same order and size,
 * each 32-byte aligned; bytes 48..63 are zero. Every CMPR block of a version
 * 2 texture decodes in the four-colour mode (c0 > c1, or c0 == c1 with all
 * indices 0), so the colour never selects CMPR's transparent entry. */
#ifndef HALO_WII_HWT_TEXTURE_H
#define HALO_WII_HWT_TEXTURE_H

#include "content_common.h"

#define HWT_HEADER_BYTES 64u
#define HWT_MAX_DIMENSION 4096u
#define HWT_MAX_DEPTH 512u
#define HWT_GX_MAX_DIMENSION 1024u

/* libogc GX_TF_* numbering (I4 is an alpha-plane format only) */
enum { HWT_GX_I4 = 0, HWT_GX_I8 = 1, HWT_GX_IA8 = 3, HWT_GX_RGB565 = 4, HWT_GX_RGB5A3 = 5, HWT_GX_RGBA8 = 6,
       HWT_GX_CMPR = 14 };
enum { HWT_TYPE_2D = 0, HWT_TYPE_3D = 1, HWT_TYPE_CUBE = 2 };
/* alpha section kinds (version 2); HWT_ALPHA_NONE for version 1 */
enum { HWT_ALPHA_NONE = 0, HWT_ALPHA_OMITTED = 1, HWT_ALPHA_PLANE = 2 };

struct hwt_header {
    uint32_t version, gx_format, width, height, depth, faces, levels, images, source_format, type;
    uint32_t data_offset, data_bytes;
    /* version 2 */
    uint32_t alpha_kind, alpha_gx_format, alpha_offset, alpha_bytes;
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
 * profile mapping (version 1: gx-baseline-v1; version 2: DXT3/DXT5 as CMPR),
 * dimensions, level and image counts, reserved bytes, the alpha section, that
 * the images (and the plane) exactly fill the data, and for version 2 the
 * four-colour form of every CMPR block. */
enum content_error hwt_parse(const unsigned char *data, uint32_t bytes, struct hwt_header *out);

/* Image index (0 <= index < images) of a parsed header. */
enum content_error hwt_image_at(const struct hwt_header *header, uint32_t index, struct hwt_image *out);

/* The alpha-plane image of colour image index (version 2 with a plane only). */
enum content_error hwt_alpha_image_at(const struct hwt_header *header, uint32_t index, struct hwt_image *out);

/* RGBA the GX sampler returns for texel (x, y) of one tiled image (the GX
 * decode rules, including CMPR's 5/8 + 3/8 interpolation; I4 and I8 give the
 * intensity in all four channels). */
void hwt_decode_texel(uint32_t gx_format, const unsigned char *image, uint32_t width, uint32_t x, uint32_t y,
                      unsigned char rgba[4]);

/* RGBA of texel (x, y) of image index of a parsed texture, as the renderer
 * combines it: the colour image's, with a version 2 texture's alpha taken
 * from its plane (255 when omitted). */
void hwt_decode_rgba(const struct hwt_header *header, const unsigned char *file, uint32_t index, uint32_t x,
                     uint32_t y, unsigned char rgba[4]);

#endif
