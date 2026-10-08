#include "cache_material_probe.h"
#include <string.h>

#define SHADER_GROUP UINT32_C(0x73686472)
#define MAX_SURFACES UINT32_C(131072)

_Static_assert(sizeof(struct cache_material_vertex_metadata) == 20, "vertex metadata");
_Static_assert(sizeof(struct cache_material_data_metadata) == 20, "data metadata");
_Static_assert(sizeof(struct cache_material_projection) == 128, "material projection");
_Static_assert(sizeof(struct cache_material_root_projection) == 24, "root projection");
_Static_assert(sizeof(struct cache_material_lightmap_projection) == 20, "lightmap projection");

struct material_reader {
    const struct cache_bsp_view *parent;
    unsigned char *bytes;
    size_t slot_bytes;
    struct cache_bsp_reference reference;
    struct cache_bsp_header bsp_header;
    struct cache_address_span root;
    struct cache_address_region tags;
    struct cache_address_header tag_header;
};

static int start(struct cache_material_result *result)
{
    if (!result)
        return 0;
    *result = (struct cache_material_result){CACHE_MATERIAL_OK, 0, SIZE_MAX, 0};
    return 1;
}

static int fail(struct cache_material_result *result, enum cache_material_error error, size_t at)
{
    result->error = error;
    result->offset = at;
    return 0;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static int32_t s32(const unsigned char *p)
{
    uint32_t value = le32(p);
    return value <= INT32_MAX ? (int32_t)value : -(int32_t)(~value) - 1;
}

static int16_t s16(const unsigned char *p)
{
    uint16_t value = le16(p);
    return value <= INT16_MAX ? (int16_t)value : (int16_t)(-(int32_t)(uint16_t)~value - 1);
}

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an && bn && (x <= y ? y - x < an : x - y < bn);
}

static int span_overlap(struct cache_address_span a, struct cache_address_span b)
{
    return a.length && b.length && (a.offset <= b.offset ?
        b.offset - a.offset < a.length : a.offset - b.offset < b.length);
}

static int parent_open(const struct cache_bsp_view *parent, struct material_reader *reader,
                        struct cache_material_result *result)
{
    struct cache_bsp_result bsp;
    struct cache_arena_result arena;
    struct cache_address_result address;
    memset(reader, 0, sizeof(*reader));
    /* lookup checks the copied outer owner pin before reading BSP child control. */
    if (!cache_bsp_lookup(parent, &reader->reference, &bsp))
        return fail(result, bsp.error == CACHE_BSP_ARGUMENT ? CACHE_MATERIAL_ARGUMENT : CACHE_MATERIAL_STATE, bsp.offset);
    if (!cache_bsp_get_root(parent, &reader->root, &bsp) ||
        !cache_bsp_get_header(parent, &reader->bsp_header, &bsp) ||
        !cache_arena_owner_resolve(parent->owner, &parent->handle, 0, 0, &reader->bytes, &arena))
        return fail(result, CACHE_MATERIAL_STATE, 0);
    reader->parent = parent;
    reader->slot_bytes = parent->control->slot_bytes;
    reader->tags = (struct cache_address_region){reader->bytes, parent->control->tag_bytes, CACHE_BSP_TAG_BASE};
    if (!cache_address_decode_header(&reader->tags, &reader->tag_header, &address))
        return fail(result, CACHE_MATERIAL_SPAN, address.offset);
    return 1;
}

static int bsp_span(const struct material_reader *reader, uint32_t address, int64_t count,
                     size_t stride, struct cache_address_span *span, struct cache_material_result *result)
{
    struct cache_bsp_result bsp;
    if (!cache_bsp_resolve(reader->parent, address, count, stride, span, &bsp)) {
        enum cache_material_error error = bsp.error == CACHE_BSP_OVERFLOW ? CACHE_MATERIAL_OVERFLOW :
            bsp.error == CACHE_BSP_COUNT ? CACHE_MATERIAL_COUNT : CACHE_MATERIAL_SPAN;
        return fail(result, error, bsp.offset);
    }
    if (span->length && (span->offset < reader->reference.slot_offset ||
        span->offset - reader->reference.slot_offset > (size_t)reader->reference.file_size ||
        span->length > (size_t)reader->reference.file_size - (span->offset - reader->reference.slot_offset)))
        return fail(result, CACHE_MATERIAL_SPAN, span->offset);
    return 1;
}

static int tag_string(const struct material_reader *reader, uint32_t address, struct cache_material_result *result)
{
    struct cache_address_span span;
    struct cache_address_result nested;
    if (!cache_address_resolve(&reader->tags, address, 1, 1, &span, &nested) ||
        !memchr(reader->bytes + span.offset, 0, reader->tags.size - span.offset))
        return fail(result, CACHE_MATERIAL_STRING, address);
    return 1;
}

static int shader(const struct material_reader *reader, const uint32_t words[4], struct cache_material_result *result)
{
    if (words[3] == UINT32_MAX)
        return 1;
    size_t ordinal = words[3] & UINT32_C(0xffff);
    if (ordinal >= (size_t)reader->tag_header.tag_count)
        return fail(result, CACHE_MATERIAL_DATUM, ordinal);
    size_t at = reader->tag_header.instances_address - CACHE_BSP_TAG_BASE + ordinal * 32;
    struct cache_address_instance instance;
    struct cache_address_result nested;
    if (!cache_address_decode_instance(reader->bytes, reader->tags.size, at, &instance, &nested))
        return fail(result, CACHE_MATERIAL_SPAN, nested.offset);
    if (instance.datum != words[3])
        return fail(result, CACHE_MATERIAL_DATUM, at + 12);
    if (instance.group != words[0] || (instance.group != SHADER_GROUP &&
        instance.parent[0] != SHADER_GROUP && instance.parent[1] != SHADER_GROUP))
        return fail(result, CACHE_MATERIAL_GROUP, at);
    return tag_string(reader, words[1], result) && tag_string(reader, instance.name_address, result);
}

static int hardware(const struct material_reader *reader, unsigned kind,
                     const struct cache_material_vertex_metadata *buffer, struct cache_material_result *result)
{
    static const size_t sizes[] = {56, 32, 20, 8};
    if (!buffer->hardware_format)
        return 1;
    int32_t count = kind ? reader->bsp_header.index_count : reader->bsp_header.vertex_count;
    uint32_t base = kind ? reader->bsp_header.index_address : reader->bsp_header.vertex_address;
    if (buffer->hardware_format < base || (buffer->hardware_format - base) % 12 ||
        (buffer->hardware_format - base) / 12 >= (uint32_t)count)
        return fail(result, CACHE_MATERIAL_RESOURCE, buffer->hardware_format);
    struct cache_bsp_descriptor descriptor;
    struct cache_bsp_result nested;
    if (!cache_bsp_get_descriptor(reader->parent, kind, (buffer->hardware_format - base) / 12, &descriptor, &nested))
        return fail(result, CACHE_MATERIAL_RESOURCE, buffer->hardware_format);
    struct cache_address_span data;
    return bsp_span(reader, descriptor.words[1], buffer->count, sizes[buffer->type], &data, result);
}

static int material(const struct material_reader *reader, size_t at, int32_t surfaces,
                     struct cache_material_projection *output, struct cache_material_result *result)
{
    const unsigned char *p = reader->bytes + at;
    struct cache_material_projection value = {0};
    for (unsigned i = 0; i < 4; ++i)
        value.shader_words[i] = le32(p + i * 4);
    value.permutation = s16(p + 16);
    value.flags = le16(p + 18);
    value.first_surface = s32(p + 20);
    value.surface_count = s32(p + 24);
    for (unsigned i = 0; i < 3; ++i)
        value.centroid_bits[i] = le32(p + 28 + i * 4);
    if (value.first_surface < 0 || value.surface_count < 0 || surfaces < 0 || (uint32_t)surfaces > MAX_SURFACES ||
        value.surface_count > surfaces || value.first_surface > surfaces - value.surface_count)
        return fail(result, CACHE_MATERIAL_COUNT, at + 20);
    if (!shader(reader, value.shader_words, result))
        return 0;
    for (unsigned kind = 0; kind < 2; ++kind) {
        const unsigned char *b = p + 176 + kind * 20;
        struct cache_material_vertex_metadata *v = &value.vertex_buffers[kind];
        *v = (struct cache_material_vertex_metadata){s16(b), le16(b + 2), s32(b + 4), s32(b + 8), le32(b + 12), le32(b + 16)};
        if ((kind == 0 && v->type != 0 && v->type != 1) || (kind == 1 && v->type != 2 && v->type != 3))
            return fail(result, CACHE_MATERIAL_TYPE, at + 176 + kind * 20);
        if (v->count < 0 || (uint32_t)v->count > CACHE_MATERIAL_MAX_VERTICES)
            return fail(result, CACHE_MATERIAL_COUNT, at + 180 + kind * 20);
        if (!hardware(reader, kind, v, result))
            return 0;
        const unsigned char *d = p + 216 + kind * 20;
        struct cache_material_data_metadata *data = &value.data_fields[kind];
        *data = (struct cache_material_data_metadata){s32(d), le32(d + 4), s32(d + 8), le32(d + 12), le32(d + 16)};
        size_t maximum = CACHE_MATERIAL_MAX_VERTICES * (kind ? 40u : 76u);
        if (data->size < 0 || (uint32_t)data->size > maximum)
            return fail(result, CACHE_MATERIAL_COUNT, at + 216 + kind * 20);
        struct cache_address_span data_span;
        if (!bsp_span(reader, data->address, data->size, 1, &data_span, result))
            return 0;
    }
    /* Xbox tag data stays compressed; hardware extents above use buffer types. */
    uint64_t minimum = (uint64_t)value.vertex_buffers[0].count * 32 + (uint64_t)value.vertex_buffers[1].count * 8;
    if (minimum > (uint32_t)value.data_fields[1].size)
        return fail(result, CACHE_MATERIAL_SPAN, at + 236);
    value.source = (struct cache_material_span){(uint32_t)at, CACHE_MATERIAL_BYTES};
    *output = value;
    return 1;
}

static int claim(struct cache_address_span span, struct cache_address_span *claims, size_t *count,
                  struct cache_material_result *result)
{
    for (size_t i = 0; i < *count; ++i)
        if (span_overlap(span, claims[i]))
            return fail(result, CACHE_MATERIAL_OVERLAP, span.offset);
    claims[(*count)++] = span;
    return 1;
}

static int sizes(size_t lc, size_t mc, struct cache_material_requirements *req, struct cache_material_result *result)
{
    size_t half = sizeof(struct cache_material_root_projection) + lc * sizeof(struct cache_material_lightmap_projection);
    if (mc > (SIZE_MAX - half) / sizeof(struct cache_material_projection))
        return fail(result, CACHE_MATERIAL_OVERFLOW, 0);
    half += mc * sizeof(struct cache_material_projection);
    if (half > SIZE_MAX / 2 || mc > (SIZE_MAX - CACHE_BSP_ROOT_BYTES - lc * 32) / 256)
        return fail(result, CACHE_MATERIAL_OVERFLOW, 0);
    *req = (struct cache_material_requirements){lc, mc, half * 2, CACHE_BSP_ROOT_BYTES + lc * 32 + mc * 256};
    return 1;
}

static int scan(const struct material_reader *reader, struct cache_material_requirements *req,
                 unsigned char *scratch, struct cache_material_result *result)
{
    size_t root = reader->root.offset;
    int32_t surface_count = s32(reader->bytes + root + 248);
    if (surface_count < 0 || (uint32_t)surface_count > MAX_SURFACES)
        return fail(result, CACHE_MATERIAL_COUNT, root + 248);
    struct cache_address_span surface_span;
    if (!bsp_span(reader, le32(reader->bytes + root + 252), surface_count, 6, &surface_span, result))
        return 0;
    int32_t lc = s32(reader->bytes + root + 260);
    if (lc < 0 || (uint32_t)lc > CACHE_MATERIAL_MAX_LIGHTMAPS)
        return fail(result, CACHE_MATERIAL_COUNT, root + 260);
    struct cache_address_span lightmaps;
    if (!bsp_span(reader, le32(reader->bytes + root + 264), lc, 32, &lightmaps, result))
        return 0;
    struct cache_address_span claims[CACHE_MATERIAL_MAX_LIGHTMAPS + 5];
    size_t claims_count = 0;
    claims[claims_count++] = reader->root;
    claims[claims_count++] = (struct cache_address_span){reader->reference.slot_offset, 24};
    claims[claims_count++] = reader->parent->control->descriptors[0];
    claims[claims_count++] = reader->parent->control->descriptors[1];
    if (!claim(lightmaps, claims, &claims_count, result))
        return 0;
    size_t total = 0;
    for (int32_t i = 0; i < lc; ++i) {
        size_t at = lightmaps.offset + (size_t)i * 32;
        int32_t mc = s32(reader->bytes + at + 20);
        if (mc < 0 || (uint32_t)mc > CACHE_MATERIAL_MAX_PER_LIGHTMAP)
            return fail(result, CACHE_MATERIAL_COUNT, at + 20);
        struct cache_address_span block;
        if (!bsp_span(reader, le32(reader->bytes + at + 24), mc, 256, &block, result) ||
            !claim(block, claims, &claims_count, result))
            return 0;
        struct cache_material_lightmap_projection lm = {s16(reader->bytes + at), le16(reader->bytes + at + 2),
            (uint32_t)total, (uint32_t)mc, {(uint32_t)at, 32}};
        if (scratch)
            memcpy(scratch + sizeof(struct cache_material_root_projection) + (size_t)i * sizeof(lm), &lm, sizeof(lm));
        for (int32_t j = 0; j < mc; ++j) {
            struct cache_material_projection value;
            result->material_index = total + (size_t)j;
            if (!material(reader, block.offset + (size_t)j * 256, surface_count, &value, result))
                return 0;
            if (scratch)
                memcpy(scratch + sizeof(struct cache_material_root_projection) + (size_t)lc * sizeof(lm) +
                       (total + (size_t)j) * sizeof(value), &value, sizeof(value));
        }
        total += (size_t)mc;
    }
    if (!sizes((size_t)lc, total, req, result))
        return 0;
    if (scratch) {
        struct cache_material_root_projection value = {{(uint32_t)root, 648},
            {(uint32_t)lightmaps.offset, (uint32_t)lightmaps.length}, (uint32_t)total, (uint32_t)lc};
        memcpy(scratch, &value, sizeof(value));
    }
    result->material_index = SIZE_MAX;
    return 1;
}

const char *cache_material_error_name(enum cache_material_error error)
{
    static const char *const names[] = {"ok", "argument", "state", "generation", "workspace", "count",
        "span", "overflow", "type", "string", "datum", "group", "resource", "overlap", "capacity"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

int cache_material_measure(const struct cache_bsp_view *parent, struct cache_material_requirements *output,
                            struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    struct material_reader reader;
    struct cache_material_requirements req;
    if (!parent_open(parent, &reader, result) || !scan(&reader, &req, NULL, result))
        return 0;
    if (overlap(output, sizeof(*output), reader.bytes, reader.slot_bytes) ||
        overlap(output, sizeof(*output), parent, sizeof(*parent)) ||
        overlap(output, sizeof(*output), parent->control, sizeof(*parent->control)) ||
        overlap(output, sizeof(*output), parent->owner, sizeof(*parent->owner)))
        return fail(result, CACHE_MATERIAL_OVERLAP, 0);
    *output = req;
    return 1;
}

int cache_material_bind(struct cache_material_control *control, const struct cache_bsp_view *parent,
                         void *workspace, size_t workspace_bytes, struct cache_material_view *output,
                         struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!control || !output || !workspace)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    struct material_reader reader;
    if (!parent_open(parent, &reader, result))
        return 0;
    if (workspace_bytes > UINTPTR_MAX - (uintptr_t)workspace)
        return fail(result, CACHE_MATERIAL_OVERFLOW, 0);
    if ((uintptr_t)workspace % _Alignof(struct cache_material_projection))
        return fail(result, CACHE_MATERIAL_WORKSPACE, 0);
    if (overlap(workspace, workspace_bytes, reader.bytes, reader.slot_bytes) ||
        overlap(workspace, workspace_bytes, control, sizeof(*control)) ||
        overlap(workspace, workspace_bytes, output, sizeof(*output)) ||
        overlap(workspace, workspace_bytes, parent, sizeof(*parent)) ||
        overlap(workspace, workspace_bytes, parent->control, sizeof(*parent->control)) ||
        overlap(workspace, workspace_bytes, parent->owner, sizeof(*parent->owner)) ||
        overlap(workspace, workspace_bytes, result, sizeof(*result)) ||
        overlap(control, sizeof(*control), output, sizeof(*output)) ||
        overlap(control, sizeof(*control), parent, sizeof(*parent)) ||
        overlap(control, sizeof(*control), parent->control, sizeof(*parent->control)) ||
        overlap(control, sizeof(*control), parent->owner, sizeof(*parent->owner)) ||
        overlap(output, sizeof(*output), parent, sizeof(*parent)) ||
        overlap(output, sizeof(*output), parent->control, sizeof(*parent->control)) ||
        overlap(output, sizeof(*output), parent->owner, sizeof(*parent->owner)) ||
        overlap(control, sizeof(*control), reader.bytes, reader.slot_bytes) ||
        overlap(output, sizeof(*output), reader.bytes, reader.slot_bytes))
        return fail(result, CACHE_MATERIAL_OVERLAP, 0);
    if (control->generation == UINT64_MAX)
        return fail(result, CACHE_MATERIAL_GENERATION, 0);
    struct cache_material_requirements req;
    if (!scan(&reader, &req, NULL, result))
        return 0;
    result->required = req.workspace_bytes;
    if (workspace_bytes < req.workspace_bytes)
        return fail(result, CACHE_MATERIAL_WORKSPACE, req.workspace_bytes);
    size_t half = req.workspace_bytes / 2;
    unsigned char *scratch = workspace;
    if (control->live) {
        const void *prior = (const void *)((uintptr_t)control->workspace + control->published.offset);
        if (overlap(scratch, half, prior, control->published.bytes)) {
            scratch += half;
            if (overlap(scratch, half, prior, control->published.bytes))
                return fail(result, CACHE_MATERIAL_WORKSPACE, req.workspace_bytes);
        }
    }
    /* The second scan writes scratch only; immutable source was fully validated. */
    if (!scan(&reader, &req, scratch, result))
        return 0;
    struct cache_material_control next = {*parent, control->generation + 1, 1,
        (uint32_t)req.lightmap_count, (uint32_t)req.material_count,
        {(uint32_t)(scratch - (unsigned char *)workspace), (uint32_t)half}, workspace, workspace_bytes};
    struct cache_material_view view = {*parent, control, next.generation};
    *control = next;
    *output = view;
    return 1;
}

static int graph_open(const struct cache_material_view *view, struct material_reader *reader,
                       const unsigned char **published, struct cache_material_result *result)
{
    if (!view)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    /* This must precede any dereference of potentially overwritten child backing. */
    if (!parent_open(&view->parent, reader, result))
        return 0;
    if (!view->control)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    const struct cache_material_control *control = view->control;
    if (!control->live || control->generation != view->generation ||
        control->parent.owner != view->parent.owner || control->parent.control != view->parent.control ||
        control->parent.generation != view->parent.generation ||
        control->parent.handle.generation != view->parent.handle.generation ||
        control->parent.handle.slot_index != view->parent.handle.slot_index)
        return fail(result, CACHE_MATERIAL_STATE, 0);
    *published = (const unsigned char *)control->workspace + control->published.offset;
    return 1;
}

static int output_object(const struct cache_material_view *view, const struct material_reader *reader,
                          void *output, size_t bytes, struct cache_material_result *result)
{
    if (overlap(output, bytes, reader->bytes, reader->slot_bytes) ||
        overlap(output, bytes, view->control->workspace, view->control->workspace_bytes) ||
        overlap(output, bytes, view->control, sizeof(*view->control)) ||
        overlap(output, bytes, view, sizeof(*view)) ||
        overlap(output, bytes, view->parent.control, sizeof(*view->parent.control)) ||
        overlap(output, bytes, view->parent.owner, sizeof(*view->parent.owner)))
        return fail(result, CACHE_MATERIAL_OVERLAP, 0);
    return 1;
}

int cache_material_get_root(const struct cache_material_view *view, struct cache_material_root_projection *output,
                             struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    struct material_reader reader;
    const unsigned char *published;
    if (!graph_open(view, &reader, &published, result))
        return 0;
    if (!output_object(view, &reader, output, sizeof(*output), result))
        return 0;
    memcpy(output, published, sizeof(*output));
    return 1;
}

int cache_material_get_lightmap(const struct cache_material_view *view, size_t index,
                                 struct cache_material_lightmap_projection *output, struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    struct material_reader reader;
    const unsigned char *published;
    if (!graph_open(view, &reader, &published, result))
        return 0;
    if (index >= view->control->lightmap_count)
        return fail(result, CACHE_MATERIAL_COUNT, index);
    if (!output_object(view, &reader, output, sizeof(*output), result))
        return 0;
    memcpy(output, published + sizeof(struct cache_material_root_projection) + index * sizeof(*output), sizeof(*output));
    return 1;
}

int cache_material_get_material(const struct cache_material_view *view, size_t index,
                                 struct cache_material_projection *output, struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    struct material_reader reader;
    const unsigned char *published;
    if (!graph_open(view, &reader, &published, result))
        return 0;
    if (index >= view->control->material_count)
        return fail(result, CACHE_MATERIAL_COUNT, index);
    if (!output_object(view, &reader, output, sizeof(*output), result))
        return 0;
    memcpy(output, published + sizeof(struct cache_material_root_projection) +
           view->control->lightmap_count * sizeof(struct cache_material_lightmap_projection) + index * sizeof(*output), sizeof(*output));
    return 1;
}

static void write_material(unsigned char *p, const struct cache_material_projection *m)
{
    for (unsigned i = 0; i < 4; ++i)
        put32(p + i * 4, m->shader_words[i]);
    put16(p + 16, (uint16_t)m->permutation);
    put16(p + 18, m->flags);
    put32(p + 20, (uint32_t)m->first_surface);
    put32(p + 24, (uint32_t)m->surface_count);
    for (unsigned i = 0; i < 3; ++i)
        put32(p + 28 + i * 4, m->centroid_bits[i]);
    for (unsigned i = 0; i < 2; ++i) {
        const struct cache_material_vertex_metadata *v = &m->vertex_buffers[i];
        unsigned char *b = p + 176 + i * 20;
        put16(b, (uint16_t)v->type);
        put16(b + 2, v->pad);
        put32(b + 4, (uint32_t)v->count);
        put32(b + 8, (uint32_t)v->offset);
        put32(b + 12, v->base_address);
        put32(b + 16, v->hardware_format);
        const struct cache_material_data_metadata *d = &m->data_fields[i];
        b = p + 216 + i * 20;
        put32(b, (uint32_t)d->size);
        put32(b + 4, d->pad);
        put32(b + 8, (uint32_t)d->file_offset);
        put32(b + 12, d->address);
        put32(b + 16, d->definition);
    }
}

int cache_material_serialize(const struct cache_material_view *view, void *output, size_t capacity,
                              size_t *used, struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!output || !used)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    struct material_reader reader;
    const unsigned char *published;
    if (!graph_open(view, &reader, &published, result))
        return 0;
    const struct cache_material_control *control = view->control;
    struct cache_material_requirements req;
    if (!sizes(control->lightmap_count, control->material_count, &req, result))
        return 0;
    result->required = req.serialized_bytes;
    if (capacity > UINTPTR_MAX - (uintptr_t)output)
        return fail(result, CACHE_MATERIAL_OVERFLOW, 0);
    if (overlap(output, capacity, reader.bytes, reader.slot_bytes) ||
        overlap(output, capacity, control->workspace, control->workspace_bytes) ||
        overlap(output, capacity, control, sizeof(*control)) ||
        overlap(output, capacity, view, sizeof(*view)) ||
        overlap(output, capacity, view->parent.control, sizeof(*view->parent.control)) ||
        overlap(output, capacity, view->parent.owner, sizeof(*view->parent.owner)) ||
        overlap(output, capacity, result, sizeof(*result)) || overlap(output, capacity, used, sizeof(*used)))
        return fail(result, CACHE_MATERIAL_OVERLAP, 0);
    if (capacity < req.serialized_bytes)
        return fail(result, CACHE_MATERIAL_CAPACITY, req.serialized_bytes);
    unsigned char *destination = output;
    struct cache_material_root_projection root;
    memcpy(&root, published, sizeof(root));
    memcpy(destination, reader.bytes + root.source.offset, 648);
    for (unsigned i = 0; i < 3; ++i)
        put32(destination + 248 + i * 4, le32(reader.bytes + root.source.offset + 248 + i * 4));
    put32(destination + 260, root.lightmap_count);
    put32(destination + 264, le32(reader.bytes + root.source.offset + 264));
    put32(destination + 268, le32(reader.bytes + root.source.offset + 268));
    size_t cursor = 648;
    for (size_t i = 0; i < control->lightmap_count; ++i) {
        struct cache_material_lightmap_projection lm;
        memcpy(&lm, published + sizeof(root) + i * sizeof(lm), sizeof(lm));
        memcpy(destination + cursor, reader.bytes + lm.source.offset, 32);
        put16(destination + cursor, (uint16_t)lm.bitmap_index);
        put16(destination + cursor + 2, lm.pad);
        put32(destination + cursor + 20, lm.material_count);
        put32(destination + cursor + 24, le32(reader.bytes + lm.source.offset + 24));
        put32(destination + cursor + 28, le32(reader.bytes + lm.source.offset + 28));
        cursor += 32;
    }
    /* Published projections and source were fully validated before publication
     * and remain immutable, so all source spans are safe before the first write. */
    for (size_t i = 0; i < control->material_count; ++i) {
        struct cache_material_projection m;
        memcpy(&m, published + sizeof(root) + control->lightmap_count * sizeof(struct cache_material_lightmap_projection) +
               i * sizeof(m), sizeof(m));
        memcpy(destination + cursor, reader.bytes + m.source.offset, 256);
        write_material(destination + cursor, &m);
        cursor += 256;
    }
    *used = cursor;
    return 1;
}

int cache_material_unload(struct cache_material_control *control, struct cache_material_result *result)
{
    if (!start(result))
        return 0;
    if (!control)
        return fail(result, CACHE_MATERIAL_ARGUMENT, 0);
    if (!control->live)
        return 1;
    if (control->generation == UINT64_MAX)
        return fail(result, CACHE_MATERIAL_GENERATION, 0);
    uint64_t generation = control->generation + 1;
    memset(control, 0, sizeof(*control));
    control->generation = generation;
    return 1;
}
