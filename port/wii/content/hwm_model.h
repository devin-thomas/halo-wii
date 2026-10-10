/* HWM1 model geometry loader (HWI-008E).
 *
 * Container (tools/wii/content_geometry.py): sections 1 PARTS (56-byte
 * records), 2 VERTICES (the 32-byte Xbox compressed model vertex), 3 INDICES
 * (u16, part-local). One model tag per file.
 *
 * Decoded form, in one owned 32-byte aligned arena:
 *   parts     native struct hwm_part, every field as stored;
 *   vertices  struct hwm_vertex, a GX indexed-array layout (stride 56): the
 *             position (GX_POS_XYZ F32) and the normal, binormal and tangent
 *             expanded from 11:11:10 with the engine's own
 *             uncompress_int32_to_real_vector3d (GX_NRM_NBT F32, the same bits
 *             as the engine), the texcoord as stored s16 (GX_TEX_ST S16; the
 *             engine's (2s + 1) / 65535 is affine and belongs in the texture
 *             matrix), the two node bytes and the node weight as stored;
 *   indices   u16 (GX_INDEX16), strips as stored: the parts are precompiled
 *             strips whose degenerate joins GX_TRIANGLESTRIP draws as the
 *             Xbox does, or triangle lists.
 * GX blends at most one matrix per vertex, so two-node weighting is the
 * renderer's (HWI-032): node bytes and weights stay exactly as stored. */
#ifndef HALO_WII_HWM_MODEL_H
#define HALO_WII_HWM_MODEL_H

#include "content_sections.h"

#define HWM_PART_FORMAT "HHIhbbhhfffffhhIIII"
#define HWM_SOURCE_VERTEX_FORMAT "fffIIIhhBBh"
#define HWM_VERTEX_FORMAT "ffffffffffffhhBBh"
#define HWM_COMPRESSED_VERTEX_TYPE 5
#define HWM_STRIP 1
#define HWM_TRIANGLES 0
#define HWM_MAX_GEOMETRIES 256u
#define HWM_MAX_PARTS_PER_GEOMETRY 32u
#define HWM_MAX_PART_VERTICES 65535u

struct hwm_part {
    uint16_t geometry, part;
    uint32_t flags;
    int16_t shader;
    int8_t previous_part, next_part;
    int16_t centroid_primary_node, centroid_secondary_node;
    float centroid_primary_weight, centroid_secondary_weight;
    float centroid[3];
    int16_t vertex_type, strip_type;
    uint32_t vertex_count, first_vertex, index_count, first_index;
};

struct hwm_vertex {
    float position[3];
    float normal[3], binormal[3], tangent[3];
    int16_t texcoord[2];
    uint8_t node[2];
    int16_t node_weight;
};

struct hwm_model {
    uint32_t part_count, vertex_count, index_count;
    struct hwm_part *parts;
    struct hwm_vertex *vertices;
    uint16_t *indices;
    struct content_arena arena;
};

/* Validates the container and every part (vertex type, strip type, counts,
 * geometry/part order, contiguous ranges that tile the arrays, every index
 * below its part's vertex count) and decodes it into owned memory. The input
 * may be released afterwards. */
enum content_error hwm_load(const unsigned char *data, uint32_t bytes, struct hwm_model *out);
void hwm_release(struct hwm_model *model);

/* SHA-256 of the decoded model, every field little-endian in order: parts
 * (HWM_PART_FORMAT), vertices (HWM_VERTEX_FORMAT), indices. */
void hwm_digest(const struct hwm_model *model, unsigned char digest[32]);

#endif
