#include "cache_address_probe.h"
#include <stdlib.h>
#include <string.h>

#define CACHE_BLOB_MAX ((size_t)0x01600000)
#define CACHE_SCENARIO UINT32_C(0x73636e72)
#define CACHE_BSP UINT32_C(0x73627370)
#define CACHE_TAGS UINT32_C(0x74616773)

static int fail(struct cache_address_result *r, enum cache_address_error error, size_t offset)
{
    if (r)
    {
        r->error = error;
        r->offset = offset;
    }
    return 0;
}

static int start(struct cache_address_result *r)
{
    if (!r)
        return 0;
    r->error = CACHE_ADDRESS_OK;
    r->offset = 0;
    r->instance_index = SIZE_MAX;
    return 1;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t signed32(const unsigned char *p)
{
    uint32_t value = le32(p);
    /* Decode negatives without an implementation-defined unsigned-to-signed cast. */
    return value <= INT32_MAX ? (int32_t)value : (int32_t)(-1 - (int64_t)(UINT32_MAX - value));
}

static int region_valid(const struct cache_address_region *region, struct cache_address_result *r)
{
    if (!region || (!region->bytes && region->size))
        return fail(r, CACHE_ADDRESS_ARGUMENT, 0);
    if ((uint64_t)region->size > UINT64_C(0x100000000) - region->encoded_base ||
        (region->bytes && region->size > UINTPTR_MAX - (uintptr_t)region->bytes))
        return fail(r, CACHE_ADDRESS_OVERFLOW, 0);
    return 1;
}

const char *cache_address_error_name(enum cache_address_error error)
{
    static const char *const names[] = {"ok",       "argument",   "span",  "count",
                                        "overflow", "signature",  "datum", "name",
                                        "root",     "allocation", "state", "group"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

int cache_address_resolve(const struct cache_address_region *region, uint32_t address,
                          int64_t count, size_t stride, struct cache_address_span *out,
                          struct cache_address_result *r)
{
    if (!start(r))
        return 0;
    if (!out || !region_valid(region, r))
        return fail(r, r->error ? r->error : CACHE_ADDRESS_ARGUMENT, r->offset);
    if (count < 0)
        return fail(r, CACHE_ADDRESS_COUNT, 0);
    struct cache_address_span span = {0, 0};
    if (!count)
    {
        *out = span;
        return 1;
    }
    if (!stride)
        return fail(r, CACHE_ADDRESS_ARGUMENT, 0);
    if ((uint64_t)count > SIZE_MAX / stride)
        return fail(r, CACHE_ADDRESS_OVERFLOW, 0);
    if (address < region->encoded_base)
        return fail(r, CACHE_ADDRESS_SPAN, 0);
    span.offset = (size_t)(address - region->encoded_base);
    span.length = (size_t)count * stride;
    if (span.offset > region->size || span.length > region->size - span.offset)
        return fail(r, CACHE_ADDRESS_SPAN, span.offset);
    *out = span;
    return 1;
}

int cache_address_decode_instance(const void *bytes, size_t size, size_t offset,
                                  struct cache_address_instance *out,
                                  struct cache_address_result *r)
{
    if (!start(r))
        return 0;
    if (!bytes || !out)
        return fail(r, CACHE_ADDRESS_ARGUMENT, offset);
    if (size > UINTPTR_MAX - (uintptr_t)bytes)
        return fail(r, CACHE_ADDRESS_OVERFLOW, offset);
    if (offset > size || 32 > size - offset)
        return fail(r, CACHE_ADDRESS_SPAN, offset);
    const unsigned char *p = (const unsigned char *)bytes + offset;
    struct cache_address_instance value = {le32(p),      {le32(p + 4), le32(p + 8)},
                                           le32(p + 12), le32(p + 16),
                                           le32(p + 20), {le32(p + 24), le32(p + 28)}};
    *out = value;
    return 1;
}

int cache_address_decode_header(const struct cache_address_region *region,
                                struct cache_address_header *out, struct cache_address_result *r)
{
    if (!start(r))
        return 0;
    if (!out || !region_valid(region, r))
        return fail(r, r->error ? r->error : CACHE_ADDRESS_ARGUMENT, r->offset);
    if (region->size < 36)
        return fail(r, CACHE_ADDRESS_SPAN, 0);
    const unsigned char *p = region->bytes;
    struct cache_address_header h = {le32(p),          le32(p + 4),      le32(p + 8),
                                     signed32(p + 12), signed32(p + 16), le32(p + 20),
                                     signed32(p + 24), le32(p + 28),     le32(p + 32)};
    if (h.signature != CACHE_TAGS)
        return fail(r, CACHE_ADDRESS_SIGNATURE, 32);
    if (h.tag_count <= 0 || h.tag_count > 65535 || h.vertex_count < 0 || h.index_count < 0)
        return fail(r, CACHE_ADDRESS_COUNT, 12);
    struct cache_address_span span;
    if (!cache_address_resolve(region, h.instances_address, h.tag_count, 32, &span, r) ||
        !cache_address_resolve(region, h.vertex_address, h.vertex_count, 12, &span, r) ||
        !cache_address_resolve(region, h.index_address, h.index_count, 12, &span, r))
        return 0;
    *out = h;
    return 1;
}

static int references(const struct cache_address_region *region,
                      const struct cache_address_instance *instance,
                      struct cache_address_span *name, struct cache_address_span *root,
                      struct cache_address_result *r)
{
    if (!cache_address_resolve(region, instance->name_address, 1, 1, name, r))
        return fail(r, CACHE_ADDRESS_NAME, r->offset);
    const unsigned char *end = memchr(region->bytes + name->offset, 0, region->size - name->offset);
    if (!end)
        return fail(r, CACHE_ADDRESS_NAME, name->offset);
    name->length = (size_t)(end - (region->bytes + name->offset)) + 1;
    if (!instance->root_address)
    {
        if (instance->group != CACHE_BSP)
            return fail(r, CACHE_ADDRESS_ROOT, 0);
        *root = (struct cache_address_span){0, 0};
        return 1;
    }
    if (!cache_address_resolve(region, instance->root_address, 1, 1, root, r))
        return fail(r, CACHE_ADDRESS_ROOT, r->offset);
    return 1;
}

int cache_address_graph_load(const struct cache_address_region *region,
                             struct cache_address_graph *out, struct cache_address_result *r)
{
    if (!start(r))
        return 0;
    if (!out || !region)
        return fail(r, CACHE_ADDRESS_ARGUMENT, 0);
    if (out->bytes || out->instances || out->size || out->count)
        return fail(r, CACHE_ADDRESS_STATE, 0);
    if (region->size > CACHE_BLOB_MAX)
        return fail(r, CACHE_ADDRESS_SPAN, 0);
    struct cache_address_graph graph = {0};
    if (!cache_address_decode_header(region, &graph.header, r))
        return 0;
    graph.size = region->size;
    graph.encoded_base = region->encoded_base;
    graph.count = (size_t)graph.header.tag_count;
    /* All temporary ownership stays local until every index reference validates. */
    graph.bytes = malloc(graph.size);
    graph.instances = calloc(graph.count, sizeof(*graph.instances));
    if (!graph.bytes || !graph.instances)
    {
        fail(r, CACHE_ADDRESS_ALLOCATION, 0);
        goto rejected;
    }
    memcpy(graph.bytes, region->bytes, graph.size);
    struct cache_address_region owned = {graph.bytes, graph.size, graph.encoded_base};
    struct cache_address_span table;
    if (!cache_address_resolve(&owned, graph.header.instances_address, graph.header.tag_count, 32,
                               &table, r))
        goto rejected;
    for (size_t i = 0; i < graph.count; ++i)
    {
        if (!cache_address_decode_instance(graph.bytes, graph.size, table.offset + i * 32,
                                           &graph.instances[i], r))
            goto rejected;
        if ((graph.instances[i].datum & 65535U) != i)
        {
            fail(r, CACHE_ADDRESS_DATUM, table.offset + i * 32 + 12);
            r->instance_index = i;
            goto rejected;
        }
        struct cache_address_span name, root;
        if (!references(&owned, &graph.instances[i], &name, &root, r))
        {
            r->instance_index = i;
            goto rejected;
        }
    }
    size_t scenario = graph.header.scenario_datum & 65535U;
    if (scenario >= graph.count || graph.instances[scenario].datum != graph.header.scenario_datum ||
        graph.instances[scenario].group != CACHE_SCENARIO)
    {
        fail(r, CACHE_ADDRESS_DATUM, 4);
        r->instance_index = scenario;
        goto rejected;
    }
    /* Publish only the complete graph; rejection preserves the caller's control. */
    *out = graph;
    return 1;
rejected:
    free(graph.instances);
    free(graph.bytes);
    return 0;
}

int cache_address_graph_lookup(const struct cache_address_graph *graph, uint32_t datum,
                               uint32_t group, struct cache_address_reference *out,
                               struct cache_address_result *r)
{
    if (!start(r))
        return 0;
    if (!graph || !out)
        return fail(r, CACHE_ADDRESS_ARGUMENT, 0);
    if (!graph->bytes || !graph->instances || !graph->count)
        return fail(r, CACHE_ADDRESS_STATE, 0);
    size_t index = datum & 65535U;
    if (index >= graph->count || graph->instances[index].datum != datum)
    {
        r->instance_index = index;
        return fail(r, CACHE_ADDRESS_DATUM, 0);
    }
    const struct cache_address_instance *instance = &graph->instances[index];
    if (group == UINT32_MAX ||
        (instance->group != group && instance->parent[0] != group && instance->parent[1] != group))
        return fail(r, CACHE_ADDRESS_GROUP, 0);
    struct cache_address_reference value;
    value.instance = *instance;
    struct cache_address_region region = {graph->bytes, graph->size, graph->encoded_base};
    if (!references(&region, instance, &value.name, &value.root, r))
        return 0;
    *out = value;
    return 1;
}

const unsigned char *cache_address_graph_bytes(const struct cache_address_graph *graph,
                                               size_t *size)
{
    if (size)
        *size = graph && graph->bytes ? graph->size : 0;
    return graph ? graph->bytes : NULL;
}

void cache_address_graph_unload(struct cache_address_graph *graph)
{
    if (!graph)
        return;
    free(graph->instances);
    free(graph->bytes);
    memset(graph, 0, sizeof(*graph));
}
