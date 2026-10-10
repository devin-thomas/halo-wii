/* HWC1 collision BSP loader (HWI-008E).
 *
 * Container (tools/wii/content_geometry.py): section 1 BSPS (68: node s16,
 * -1 for a structure BSP; bsp s16; then (first, count) u32 pairs for the eight
 * arrays) and sections 2-9, the eight collision arrays of every BSP in order:
 * 3D nodes, planes, leaves, 2D references, 2D nodes, surfaces, edges and
 * vertices. One collision model tag (all its BSPs) or one structure BSP per
 * file.
 *
 * Decoded form: native arrays with the engine's record layouts
 * (source/physics/collision_bsp_definitions.c), one per array kind in one
 * owned 32-byte aligned arena; a BSP's elements are [first, first + count) of
 * each array, and every index inside a BSP is relative to that BSP's arrays,
 * so a BSP's arrays can back the engine's tag blocks directly.
 *
 * Every index the engine follows is checked against its BSP: 3D node planes
 * and children (a node, NONE, or a leaf with the sign bit), leaf references,
 * 2D reference planes (sign bit: flipped) and nodes, 2D node children (a node,
 * or a surface with the sign bit), surface planes (sign bit: flipped) and
 * first edges, edge vertices, edges and surfaces (-1: none), vertex first
 * edges. */
#ifndef HALO_WII_HWC_COLLISION_H
#define HALO_WII_HWC_COLLISION_H

#include "content_sections.h"

#define HWC_BSP_FORMAT "hhIIIIIIIIIIIIIIII"
#define HWC_ARRAYS 8u
enum {
    HWC_BSP3D_NODES,
    HWC_PLANES,
    HWC_LEAVES,
    HWC_BSP2D_REFERENCES,
    HWC_BSP2D_NODES,
    HWC_SURFACES,
    HWC_EDGES,
    HWC_VERTICES
};

struct hwc_range {
    uint32_t first, count;
};

struct hwc_bsp {
    int16_t node, bsp;
    struct hwc_range arrays[HWC_ARRAYS];
};

struct hwc_bsp3d_node {
    int32_t plane, back_child, front_child;
};

struct hwc_plane {
    float i, j, k, d;
};

struct hwc_leaf {
    uint16_t flags;
    int16_t bsp2d_reference_count;
    int32_t first_bsp2d_reference;
};

struct hwc_bsp2d_reference {
    int32_t plane, bsp2d_node;
};

struct hwc_bsp2d_node {
    float i, j, d;
    int32_t left_child, right_child;
};

struct hwc_surface {
    int32_t plane, first_edge;
    uint8_t flags;
    int8_t breakable_surface;
    int16_t material;
};

struct hwc_edge {
    int32_t start_vertex, end_vertex, forward_edge, reverse_edge, left_surface, right_surface;
};

struct hwc_vertex {
    float point[3];
    int32_t first_edge;
};

struct hwc_collision {
    uint32_t bsp_count;
    struct hwc_bsp *bsps;
    uint32_t counts[HWC_ARRAYS];
    struct hwc_bsp3d_node *bsp3d_nodes;
    struct hwc_plane *planes;
    struct hwc_leaf *leaves;
    struct hwc_bsp2d_reference *bsp2d_references;
    struct hwc_bsp2d_node *bsp2d_nodes;
    struct hwc_surface *surfaces;
    struct hwc_edge *edges;
    struct hwc_vertex *vertices;
    struct content_arena arena;
};

/* Validates the container, the per-BSP ranges (contiguous, tiling every
 * array, within the engine's per-BSP maxima) and every index above, and
 * decodes into owned memory. */
enum content_error hwc_load(const unsigned char *data, uint32_t bytes, struct hwc_collision *out);
void hwc_release(struct hwc_collision *collision);

/* SHA-256 of the decoded collision, every field little-endian in order: BSP
 * records, then the eight arrays. */
void hwc_digest(const struct hwc_collision *collision, unsigned char digest[32]);

#endif
