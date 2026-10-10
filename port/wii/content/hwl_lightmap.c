/* HWL1 structure-BSP lightmap geometry loader (HWI-008E). See hwl_lightmap.h. */
#include "hwl_lightmap.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(struct hwl_bsp) == 16, "hwl_bsp matches HWL_BSP_FORMAT");
_Static_assert(sizeof(struct hwl_lightmap) == 12, "hwl_lightmap matches HWL_LIGHTMAP_FORMAT");
_Static_assert(sizeof(struct hwl_material) == 188, "hwl_material matches HWL_MATERIAL_FORMAT");
_Static_assert(offsetof(struct hwl_material, ambient_color) == 32, "hwl_material layout");
_Static_assert(offsetof(struct hwl_material, distant_lights) == 48, "hwl_material layout");
_Static_assert(offsetof(struct hwl_material, point_light_count) == 96, "hwl_material layout");
_Static_assert(offsetof(struct hwl_material, plane) == 148, "hwl_material layout");
_Static_assert(offsetof(struct hwl_material, vertex_type) == 168, "hwl_material layout");
_Static_assert(sizeof(struct hwl_vertex) == 56, "hwl_vertex matches HWL_VERTEX_FORMAT");
_Static_assert(sizeof(struct hwl_lightmap_vertex) == 16, "hwl_lightmap_vertex matches HWL_LIGHTMAP_VERTEX_FORMAT");
_Static_assert(sizeof(struct hwl_surface) == 6, "hwl_surface matches HWL_SURFACE_FORMAT");

static const uint32_t record_bytes[6] = {16, 12, 188, 32, 8, 6};

static enum content_error check(const struct hwl_geometry *g)
{
    if (g->bsp.lightmap_count != g->lightmap_count || g->bsp.material_count != g->material_count ||
        g->bsp.surface_count != g->surface_count)
        return CONTENT_COUNT;
    uint64_t materials = 0;
    for (uint32_t i = 0; i < g->lightmap_count; ++i) {
        if (g->lightmaps[i].first_material != materials)
            return CONTENT_RANGE;
        materials += g->lightmaps[i].material_count;
    }
    if (materials != g->material_count)
        return CONTENT_RANGE;
    uint64_t vertices = 0, lightmap_vertices = 0;
    for (uint32_t i = 0; i < g->material_count; ++i) {
        const struct hwl_material *m = &g->materials[i];
        if (m->vertex_type != HWL_ENVIRONMENT_COMPRESSED ||
            m->lightmap_vertex_type != (m->lightmap_vertex_count ? HWL_LIGHTMAP_COMPRESSED : 0))
            return CONTENT_ENUM;
        if (m->vertex_count > HWL_MAX_MATERIAL_VERTICES || m->lightmap_vertex_count > m->vertex_count)
            return CONTENT_COUNT;
        if (m->first_vertex != vertices || m->first_lightmap_vertex != lightmap_vertices)
            return CONTENT_RANGE;
        vertices += m->vertex_count;
        lightmap_vertices += m->lightmap_vertex_count;
        if (vertices > g->vertex_count || lightmap_vertices > g->lightmap_vertex_count)
            return CONTENT_RANGE;
        if (m->first_surface < 0 || m->surface_count < 0 ||
            (uint64_t)m->first_surface + (uint64_t)m->surface_count > g->surface_count)
            return CONTENT_RANGE;
        for (int32_t s = 0; s < m->surface_count; ++s)
            for (int k = 0; k < 3; ++k)
                if (g->surfaces[m->first_surface + s].vertex[k] >= m->vertex_count)
                    return CONTENT_RANGE;
    }
    return vertices == g->vertex_count && lightmap_vertices == g->lightmap_vertex_count ? CONTENT_OK : CONTENT_RANGE;
}

enum content_error hwl_load(const unsigned char *data, uint32_t bytes, struct hwl_geometry *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    struct content_layout bsp, lightmap, material, surface;
    if (content_layout_init(&bsp, HWL_BSP_FORMAT) != sizeof(struct hwl_bsp) ||
        content_layout_init(&lightmap, HWL_LIGHTMAP_FORMAT) != sizeof(struct hwl_lightmap) ||
        content_layout_init(&material, HWL_MATERIAL_FORMAT) != sizeof(struct hwl_material) ||
        content_layout_init(&surface, HWL_SURFACE_FORMAT) != sizeof(struct hwl_surface))
        return CONTENT_ARGUMENT;
    struct content_section s[6];
    enum content_error error = content_sections_parse(data, bytes, "HWL1", record_bytes, 6, s);
    if (error != CONTENT_OK)
        return error;
    if (s[0].count != 1)
        return CONTENT_COUNT;
    struct hwl_geometry g;
    memset(&g, 0, sizeof(g));
    content_records_decode(&bsp, s[0].data, 1, &g.bsp, sizeof(g.bsp));
    g.lightmap_count = s[1].count;
    g.material_count = s[2].count;
    g.vertex_count = s[3].count;
    g.lightmap_vertex_count = s[4].count;
    g.surface_count = s[5].count;
    const uint64_t sizes[5] = {s[1].bytes, s[2].bytes, (uint64_t)g.vertex_count * sizeof(struct hwl_vertex),
                               (uint64_t)g.lightmap_vertex_count * sizeof(struct hwl_lightmap_vertex), s[5].bytes};
    error = content_arena_reserve(&g.arena, content_arena_size(sizes, 5));
    if (error != CONTENT_OK)
        return error;
    g.lightmaps = content_arena_take(&g.arena, s[1].bytes);
    g.materials = content_arena_take(&g.arena, s[2].bytes);
    g.vertices = content_arena_take(&g.arena, g.vertex_count * (uint32_t)sizeof(struct hwl_vertex));
    g.lightmap_vertices =
        content_arena_take(&g.arena, g.lightmap_vertex_count * (uint32_t)sizeof(struct hwl_lightmap_vertex));
    g.surfaces = content_arena_take(&g.arena, s[5].bytes);
    content_records_decode(&lightmap, s[1].data, g.lightmap_count, g.lightmaps, sizeof(struct hwl_lightmap));
    content_records_decode(&material, s[2].data, g.material_count, g.materials, sizeof(struct hwl_material));
    content_records_decode(&surface, s[5].data, g.surface_count, g.surfaces, sizeof(struct hwl_surface));
    error = check(&g);
    if (error != CONTENT_OK) {
        content_arena_release(&g.arena);
        return error;
    }
    for (uint32_t v = 0; v < g.vertex_count; ++v) {
        const unsigned char *in = s[3].data + 32u * v;
        struct hwl_vertex *o = &g.vertices[v];
        uint32_t bits[5] = {content_be32(in), content_be32(in + 4), content_be32(in + 8), content_be32(in + 24),
                            content_be32(in + 28)};
        memcpy(o->position, bits, 12);
        memcpy(o->texcoord, bits + 3, 8);
        content_unpack_vector(content_be32(in + 12), o->normal);
        content_unpack_vector(content_be32(in + 16), o->binormal);
        content_unpack_vector(content_be32(in + 20), o->tangent);
    }
    for (uint32_t v = 0; v < g.lightmap_vertex_count; ++v) {
        const unsigned char *in = s[4].data + 8u * v;
        struct hwl_lightmap_vertex *o = &g.lightmap_vertices[v];
        content_unpack_vector(content_be32(in), o->incident_radiosity);
        o->texcoord[0] = content_s16(in + 4);
        o->texcoord[1] = content_s16(in + 6);
    }
    *out = g;
    return CONTENT_OK;
}

void hwl_release(struct hwl_geometry *geometry)
{
    if (geometry == NULL)
        return;
    content_arena_release(&geometry->arena);
    memset(geometry, 0, sizeof(*geometry));
}

void hwl_digest(const struct hwl_geometry *g, unsigned char digest[32])
{
    struct content_layout bsp, lightmap, material, vertex, lightmap_vertex, surface;
    content_layout_init(&bsp, HWL_BSP_FORMAT);
    content_layout_init(&lightmap, HWL_LIGHTMAP_FORMAT);
    content_layout_init(&material, HWL_MATERIAL_FORMAT);
    content_layout_init(&vertex, HWL_VERTEX_FORMAT);
    content_layout_init(&lightmap_vertex, HWL_LIGHTMAP_VERTEX_FORMAT);
    content_layout_init(&surface, HWL_SURFACE_FORMAT);
    struct content_sha256 sha;
    content_sha256_init(&sha);
    content_records_digest(&sha, &bsp, &g->bsp, 1, sizeof(g->bsp));
    content_records_digest(&sha, &lightmap, g->lightmaps, g->lightmap_count, sizeof(struct hwl_lightmap));
    content_records_digest(&sha, &material, g->materials, g->material_count, sizeof(struct hwl_material));
    content_records_digest(&sha, &vertex, g->vertices, g->vertex_count, sizeof(struct hwl_vertex));
    content_records_digest(&sha, &lightmap_vertex, g->lightmap_vertices, g->lightmap_vertex_count,
                           sizeof(struct hwl_lightmap_vertex));
    content_records_digest(&sha, &surface, g->surfaces, g->surface_count, sizeof(struct hwl_surface));
    content_sha256_final(&sha, digest);
}
