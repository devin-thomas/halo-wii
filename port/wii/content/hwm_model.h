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

/* ---------- HWI-082: the compressed model vertex as the renderer consumes it
 *
 * One profile, the Xbox cache's: a model part's vertex buffer of
 * _rasterizer_vertex_type_model_compressed (5), 32 bytes a vertex, read from
 * the tag slot where map_stage.py leaves the payload as the map holds it
 * (little-endian: the staging converts only what the game reads).
 *
 * The skinning fields are decoded as the Xbox's vertex declaration for this
 * vertex consumes them (rasterizer_xbox_vertex_shaders_initialize.c,
 * vertex_shader_declarations[0x78 / 4]: FLOAT3, NORMPACKED3 x3, NORMSHORT2,
 * PBYTE2, NORMSHORT1), with the node matrices as rasterizer_set_model_skinning
 * loads them (three constant registers a node, every node of the model):
 *   node byte    the model's node times 3 (the constant register offset), so
 *                node = byte / 3; a byte that is not a multiple of 3 is refused
 *   node weight  NORMSHORT1: the first node's weight is s16 / 32767, the
 *                second's 1 minus it (compress_real_to_int16_clamp writes it)
 * rasterizer_geometry_uncompress_vertices' (byte)weight / 255 is not what the
 * hardware reads, and is not used. A weight of 32767 gives the second node
 * weight 0 exactly: its byte is not consumed (retail maps leave other values
 * there) and is not checked. Anything else outside this profile is refused,
 * never mapped to another node. tools/wii/model_vertex_probe.py measures a
 * staged map against these rules (counts only).
 *
 * The node matrices are the renderer's (render_model's node matrix times the
 * node's runtime_default_inverse_matrix), each as the vertex shader's three
 * constants (node_matrix_constants): row r = scale * (forward, left, up)
 * component r, then position component r. */
#define HWM_SKIN_VERTEX_BYTES 32u
#define HWM_SKIN_FULL_WEIGHT 32767
#define HWM_SKIN_MAXIMUM_NODES 64u

enum hwm_skin_result {
    HWM_SKIN_OK = 0,
    HWM_SKIN_NODE_STRIDE,  /* a consumed node byte is not a multiple of 3 */
    HWM_SKIN_NODE_RANGE,   /* a consumed node is past the model's nodes */
    HWM_SKIN_WEIGHT_RANGE, /* the weight is negative */
    HWM_SKIN_POSITION,     /* the position is not finite */
    NUMBER_OF_HWM_SKIN_RESULTS
};

struct hwm_skin_vertex {
    float position[3];
    float normal[3];
    uint8_t node[2];   /* the model's nodes (byte / 3); node[1] == node[0] when unused */
    float weight[2];
};

/* Decodes one little-endian compressed model vertex (HWM_SKIN_VERTEX_BYTES)
 * against a model of node_count nodes (1..HWM_SKIN_MAXIMUM_NODES). */
enum hwm_skin_result hwm_skin_decode_le(const unsigned char *in, uint32_t node_count, struct hwm_skin_vertex *out);

/* The decoded vertex through the node matrices: the weighted sum of each
 * consumed node's transform of the position and of the normal (the normal then
 * normalised; left as the sum when its length is 0). */
void hwm_skin_apply(const struct hwm_skin_vertex *vertex, const float (*palette)[3][4], float position[3],
                    float normal[3]);

const char *hwm_skin_result_name(enum hwm_skin_result result);

#endif
