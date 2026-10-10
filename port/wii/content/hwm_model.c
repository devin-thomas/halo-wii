/* HWM1 model geometry loader (HWI-008E). See hwm_model.h. */
#include "hwm_model.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(struct hwm_part) == 56, "hwm_part matches HWM_PART_FORMAT");
_Static_assert(offsetof(struct hwm_part, centroid_primary_weight) == 16, "hwm_part layout");
_Static_assert(offsetof(struct hwm_part, vertex_type) == 36, "hwm_part layout");
_Static_assert(offsetof(struct hwm_part, first_index) == 52, "hwm_part layout");
_Static_assert(sizeof(struct hwm_vertex) == 56, "hwm_vertex matches HWM_VERTEX_FORMAT");
_Static_assert(offsetof(struct hwm_vertex, texcoord) == 48, "hwm_vertex layout");
_Static_assert(offsetof(struct hwm_vertex, node_weight) == 54, "hwm_vertex layout");

static const uint32_t record_bytes[3] = {56, 32, 2};

static enum content_error check_parts(const struct hwm_model *m)
{
    uint64_t vertices = 0, indices = 0;
    for (uint32_t i = 0; i < m->part_count; ++i) {
        const struct hwm_part *p = &m->parts[i];
        if (p->vertex_type != HWM_COMPRESSED_VERTEX_TYPE || (p->strip_type != HWM_STRIP && p->strip_type != HWM_TRIANGLES))
            return CONTENT_ENUM;
        if (p->vertex_count > HWM_MAX_PART_VERTICES || p->index_count > 3u * HWM_MAX_PART_VERTICES ||
            (p->strip_type == HWM_TRIANGLES ? p->index_count % 3 != 0 : p->index_count == 1 || p->index_count == 2))
            return CONTENT_COUNT;
        if (p->geometry >= HWM_MAX_GEOMETRIES || p->part >= HWM_MAX_PARTS_PER_GEOMETRY)
            return CONTENT_RANGE;
        if (i == 0 ? p->part != 0
                   : (p->geometry == m->parts[i - 1].geometry ? p->part != m->parts[i - 1].part + 1
                                                              : p->geometry < m->parts[i - 1].geometry || p->part != 0))
            return CONTENT_RANGE;
        if (p->first_vertex != vertices || p->first_index != indices)
            return CONTENT_RANGE;
        vertices += p->vertex_count;
        indices += p->index_count;
        if (vertices > m->vertex_count || indices > m->index_count)
            return CONTENT_RANGE;
        for (uint32_t k = 0; k < p->index_count; ++k)
            if (m->indices[p->first_index + k] >= p->vertex_count)
                return CONTENT_RANGE;
    }
    return vertices == m->vertex_count && indices == m->index_count ? CONTENT_OK : CONTENT_RANGE;
}

enum content_error hwm_load(const unsigned char *data, uint32_t bytes, struct hwm_model *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    struct content_layout part, source, index;
    if (content_layout_init(&part, HWM_PART_FORMAT) != sizeof(struct hwm_part) ||
        content_layout_init(&source, HWM_SOURCE_VERTEX_FORMAT) != record_bytes[1] ||
        content_layout_init(&index, "H") != 2)
        return CONTENT_ARGUMENT;
    struct content_section s[3];
    enum content_error error = content_sections_parse(data, bytes, "HWM1", record_bytes, 3, s);
    if (error != CONTENT_OK)
        return error;
    struct hwm_model m;
    memset(&m, 0, sizeof(m));
    m.part_count = s[0].count;
    m.vertex_count = s[1].count;
    m.index_count = s[2].count;
    const uint64_t sizes[3] = {s[0].bytes, (uint64_t)m.vertex_count * sizeof(struct hwm_vertex), s[2].bytes};
    error = content_arena_reserve(&m.arena, content_arena_size(sizes, 3));
    if (error != CONTENT_OK)
        return error;
    m.parts = content_arena_take(&m.arena, s[0].bytes);
    m.vertices = content_arena_take(&m.arena, m.vertex_count * (uint32_t)sizeof(struct hwm_vertex));
    m.indices = content_arena_take(&m.arena, s[2].bytes);
    content_records_decode(&part, s[0].data, m.part_count, m.parts, sizeof(struct hwm_part));
    content_records_decode(&index, s[2].data, m.index_count, m.indices, 2);
    error = check_parts(&m);
    if (error != CONTENT_OK) {
        content_arena_release(&m.arena);
        return error;
    }
    for (uint32_t v = 0; v < m.vertex_count; ++v) {
        const unsigned char *in = s[1].data + 32u * v;
        struct hwm_vertex *o = &m.vertices[v];
        for (int c = 0; c < 3; ++c) {
            uint32_t bits = content_be32(in + 4 * c);
            memcpy(&o->position[c], &bits, 4);
        }
        content_unpack_vector(content_be32(in + 12), o->normal);
        content_unpack_vector(content_be32(in + 16), o->binormal);
        content_unpack_vector(content_be32(in + 20), o->tangent);
        o->texcoord[0] = content_s16(in + 24);
        o->texcoord[1] = content_s16(in + 26);
        o->node[0] = in[28];
        o->node[1] = in[29];
        o->node_weight = content_s16(in + 30);
    }
    *out = m;
    return CONTENT_OK;
}

void hwm_release(struct hwm_model *model)
{
    if (model == NULL)
        return;
    content_arena_release(&model->arena);
    memset(model, 0, sizeof(*model));
}

void hwm_digest(const struct hwm_model *m, unsigned char digest[32])
{
    struct content_layout part, vertex, index;
    content_layout_init(&part, HWM_PART_FORMAT);
    content_layout_init(&vertex, HWM_VERTEX_FORMAT);
    content_layout_init(&index, "H");
    struct content_sha256 sha;
    content_sha256_init(&sha);
    content_records_digest(&sha, &part, m->parts, m->part_count, sizeof(struct hwm_part));
    content_records_digest(&sha, &vertex, m->vertices, m->vertex_count, sizeof(struct hwm_vertex));
    content_records_digest(&sha, &index, m->indices, m->index_count, 2);
    content_sha256_final(&sha, digest);
}
