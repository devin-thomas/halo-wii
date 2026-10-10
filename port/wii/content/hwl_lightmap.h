/* HWL1 structure-BSP lightmap geometry loader (HWI-008E).
 *
 * Container (tools/wii/content_geometry.py): sections 1 BSP (16), 2
 * LIGHTMAPS (12), 3 MATERIALS (188, with their render lighting), 4 VERTICES
 * (the 32-byte compressed environment vertex), 5 LIGHTMAP_VERTICES (8), 6
 * SURFACES (three u16 indices local to the surface's material). One
 * scenario structure BSP per file.
 *
 * Decoded form, in one owned 32-byte aligned arena:
 *   bsp, lightmaps, materials  native structs, every field as stored;
 *   vertices        struct hwl_vertex, a GX indexed-array layout (stride 56):
 *                   position F32, normal, binormal and tangent expanded from
 *                   11:11:10 with the engine's uncompress_int32_to_real_vector3d
 *                   (GX_NRM_NBT F32), texcoord F32 as stored;
 *   lightmap_vertices  struct hwl_lightmap_vertex (stride 16): the incident
 *                   radiosity direction expanded the same way, and the
 *                   lightmap texcoord as stored s16 (GX_TEX_ST S16; the
 *                   engine's (2s + 1) / 65535 belongs in the texture matrix);
 *   surfaces        u16 triangles (GX_INDEX16 lists), indices local to the
 *                   material: a material's first_vertex locates them, and its
 *                   n-th lightmap vertex pairs with its n-th vertex. */
#ifndef HALO_WII_HWL_LIGHTMAP_H
#define HALO_WII_HWL_LIGHTMAP_H

#include "content_sections.h"

#define HWL_BSP_FORMAT "IIII"
#define HWL_LIGHTMAP_FORMAT "hHII"
#define HWL_MATERIAL_FORMAT                                                                                            \
    "IIhHii"                                                                                                           \
    "fff"                                                                                                              \
    "fffhHffffffffffffhHiiffffffffff"                                                                                  \
    "ffffhHhhIIII"
#define HWL_VERTEX_FORMAT "ffffffffffffff"
#define HWL_LIGHTMAP_VERTEX_FORMAT "fffhh"
#define HWL_SURFACE_FORMAT "HHH"
#define HWL_ENVIRONMENT_COMPRESSED 1
#define HWL_LIGHTMAP_COMPRESSED 3
#define HWL_MAX_MATERIAL_VERTICES 65535u

struct hwl_bsp {
    uint32_t lightmap_bitmap;   /* datum of the lightmap bitmap tag, or 0xFFFFFFFF */
    uint32_t lightmap_count, material_count, surface_count;
};

struct hwl_lightmap {
    int16_t bitmap_index;
    uint16_t pad;
    uint32_t first_material, material_count;
};

struct hwl_distant_light {
    float color[3], direction[3];
};

struct hwl_material {
    uint32_t shader_group, shader;
    int16_t permutation;
    uint16_t flags;
    int32_t first_surface, surface_count;
    float centroid[3];
    float ambient_color[3];
    int16_t distant_light_count;
    uint16_t pad0;
    struct hwl_distant_light distant_lights[2];
    int16_t point_light_count;
    uint16_t pad1;
    int32_t point_lights[2];
    float reflection_tint[4];
    float shadow_vector[3];
    float shadow_color[3];
    float plane[4];
    int16_t breakable_surface;
    uint16_t pad2;
    int16_t vertex_type, lightmap_vertex_type;
    uint32_t vertex_count, first_vertex, lightmap_vertex_count, first_lightmap_vertex;
};

struct hwl_vertex {
    float position[3];
    float normal[3], binormal[3], tangent[3];
    float texcoord[2];
};

struct hwl_lightmap_vertex {
    float incident_radiosity[3];
    int16_t texcoord[2];
};

struct hwl_surface {
    uint16_t vertex[3];
};

struct hwl_geometry {
    struct hwl_bsp bsp;
    uint32_t lightmap_count, material_count, vertex_count, lightmap_vertex_count, surface_count;
    struct hwl_lightmap *lightmaps;
    struct hwl_material *materials;
    struct hwl_vertex *vertices;
    struct hwl_lightmap_vertex *lightmap_vertices;
    struct hwl_surface *surfaces;
    struct content_arena arena;
};

/* Validates the container, the BSP record against the sections, lightmap
 * material ranges, every material (vertex types, counts, contiguous vertex
 * ranges, surfaces inside the surface array, every surface index below the
 * material's vertex count) and decodes it into owned memory. */
enum content_error hwl_load(const unsigned char *data, uint32_t bytes, struct hwl_geometry *out);
void hwl_release(struct hwl_geometry *geometry);

/* SHA-256 of the decoded geometry, every field little-endian in order: BSP,
 * lightmaps, materials, vertices, lightmap vertices, surfaces. */
void hwl_digest(const struct hwl_geometry *geometry, unsigned char digest[32]);

#endif
