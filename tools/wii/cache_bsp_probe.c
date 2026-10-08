#include "cache_bsp_probe.h"
#include <string.h>

#define BSP_GROUP UINT32_C(0x73627370)
#define SCENARIO_GROUP UINT32_C(0x73636e72)

static int start(struct cache_bsp_result *result)
{
    if (!result)
        return 0;
    result->error = CACHE_BSP_OK;
    result->offset = 0;
    return 1;
}

static int fail(struct cache_bsp_result *result, enum cache_bsp_error error, size_t offset)
{
    result->error = error;
    result->offset = offset;
    return 0;
}

static uint32_t le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int32_t s32(const unsigned char *bytes)
{
    uint32_t value = le32(bytes);
    return value <= INT32_MAX ? (int32_t)value : -(int32_t)(~value) - 1;
}

static void put32(unsigned char *bytes, uint32_t word)
{
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[byte] = (unsigned char)(word >> (byte * 8));
}

static int overlaps(struct cache_address_span a, struct cache_address_span b)
{
    return a.length && b.length && (a.offset <= b.offset ?
        b.offset - a.offset < a.length : a.offset - b.offset < b.length);
}

static int slot(const struct cache_arena_owner *owner, const struct cache_arena_handle *handle,
                unsigned char **bytes, size_t *size, struct cache_bsp_result *result)
{
    struct cache_arena_result arena;
    if (!owner || !handle)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    if (!cache_arena_owner_resolve(owner, handle, 0, 0, bytes, &arena))
        return fail(result, CACHE_BSP_STATE, 0);
    *size = owner->plan.slots[handle->slot_index].size;
    if (*size > CACHE_BSP_TAG_LIMIT)
        return fail(result, CACHE_BSP_CAPACITY, 0);
    return 1;
}

static int address_span(uint32_t address, int64_t count, size_t stride,
                        size_t window_offset, size_t window_bytes,
                        struct cache_address_span *output, struct cache_bsp_result *result)
{
    struct cache_address_span value = {0, 0};
    if (count < 0)
        return fail(result, CACHE_BSP_COUNT, window_offset);
    if (!count)
    {
        *output = value;
        return 1;
    }
    if (!stride)
        return fail(result, CACHE_BSP_ARGUMENT, window_offset);
    if ((uint64_t)count > SIZE_MAX / stride)
        return fail(result, CACHE_BSP_OVERFLOW, window_offset);
    if (address < CACHE_BSP_TAG_BASE || address - CACHE_BSP_TAG_BASE < window_offset)
        return fail(result, CACHE_BSP_SPAN, window_offset);
    value.offset = address - CACHE_BSP_TAG_BASE;
    size_t relative = value.offset - window_offset;
    value.length = (size_t)count * stride;
    if (relative > window_bytes || value.length > window_bytes - relative)
        return fail(result, CACHE_BSP_SPAN, value.offset);
    *output = value;
    return 1;
}

static int tag_instance(const struct cache_address_region *region, const struct cache_address_header *header,
                        uint32_t datum, uint32_t group, struct cache_address_instance *output,
                        struct cache_bsp_result *result)
{
    size_t ordinal = datum & UINT32_C(0xffff);
    struct cache_address_result nested;
    if (datum == UINT32_MAX || ordinal >= (size_t)header->tag_count)
        return fail(result, CACHE_BSP_DATUM, ordinal);
    size_t offset = header->instances_address - CACHE_BSP_TAG_BASE + ordinal * 32;
    if (!cache_address_decode_instance(region->bytes, region->size, offset, output, &nested))
        return fail(result, CACHE_BSP_SPAN, nested.offset);
    if (output->datum != datum)
        return fail(result, CACHE_BSP_DATUM, offset + 12);
    if (output->group != group)
        return fail(result, CACHE_BSP_GROUP, offset);
    return 1;
}

const char *cache_bsp_error_name(enum cache_bsp_error error)
{
    static const char *const names[] = {"ok", "argument", "state", "generation", "span", "count",
        "overflow", "datum", "group", "signature", "overlap", "capacity"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

int cache_bsp_select(const struct cache_arena_owner *owner, const struct cache_arena_handle *handle,
                     size_t tag_bytes, uint32_t map_bytes, size_t ordinal,
                     struct cache_bsp_reference *output, struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    size_t slot_bytes;
    if (!slot(owner, handle, &bytes, &slot_bytes, result))
        return 0;
    if (tag_bytes < 36 || tag_bytes > slot_bytes || map_bytes < 2048 || map_bytes > UINT32_C(0x11600000))
        return fail(result, CACHE_BSP_CAPACITY, 0);
    struct cache_address_region region = {bytes, tag_bytes, CACHE_BSP_TAG_BASE};
    struct cache_address_header header;
    struct cache_address_result nested;
    if (!cache_address_decode_header(&region, &header, &nested))
        return fail(result, nested.error == CACHE_ADDRESS_COUNT ? CACHE_BSP_COUNT :
                    nested.error == CACHE_ADDRESS_SIGNATURE ? CACHE_BSP_SIGNATURE : CACHE_BSP_SPAN, nested.offset);
    struct cache_address_span claimed[5] = {{0, 36},
        {header.instances_address - CACHE_BSP_TAG_BASE, (size_t)header.tag_count * 32},
        {0, 0}, {0, 0}, {0, 0}};
    /* Tag-header descriptor tables occupy tag bytes. Their Data words are
     * resource offsets, so only the table extents are claimed here. */
    if (!address_span(header.vertex_address, header.vertex_count, 12, 0, tag_bytes, &claimed[2], result) ||
        !address_span(header.index_address, header.index_count, 12, 0, tag_bytes, &claimed[3], result))
        return 0;
    for (unsigned left = 0; left < 4; ++left)
        for (unsigned right = left + 1; right < 4; ++right)
            if (overlaps(claimed[left], claimed[right]))
                return fail(result, CACHE_BSP_OVERLAP, claimed[right].offset);
    struct cache_address_instance scenario, bsp;
    struct cache_address_span root, table;
    if (!tag_instance(&region, &header, header.scenario_datum, SCENARIO_GROUP, &scenario, result) ||
        !address_span(scenario.root_address, 1, CACHE_BSP_SCENARIO_BYTES, 0, tag_bytes, &root, result))
        return 0;
    for (unsigned index = 0; index < 4; ++index)
        if (overlaps(root, claimed[index]))
            return fail(result, CACHE_BSP_OVERLAP, root.offset);
    claimed[4] = root;
    size_t block = root.offset + 0x5a4;
    int32_t count = s32(bytes + block);
    if (count < 0 || count > (int32_t)CACHE_BSP_MAX_REFERENCES)
        return fail(result, CACHE_BSP_COUNT, block);
    if (ordinal >= (size_t)count)
        return fail(result, CACHE_BSP_DATUM, block);
    if (!address_span(le32(bytes + block + 4), count, CACHE_BSP_REFERENCE_BYTES, 0, tag_bytes, &table, result))
        return 0;
    for (unsigned index = 0; index < 5; ++index)
        if (overlaps(table, claimed[index]))
            return fail(result, CACHE_BSP_OVERLAP, table.offset);
    size_t at = table.offset + ordinal * CACHE_BSP_REFERENCE_BYTES;
    struct cache_bsp_reference value = {
        s32(bytes + at), s32(bytes + at + 4), le32(bytes + at + 8), le32(bytes + at + 12),
        le32(bytes + at + 16), le32(bytes + at + 20), le32(bytes + at + 24), le32(bytes + at + 28),
        header.scenario_datum, ordinal, 0, 0
    };
    if (value.group != BSP_GROUP)
        return fail(result, CACHE_BSP_GROUP, at + 16);
    if (!tag_instance(&region, &header, value.datum, BSP_GROUP, &bsp, result))
        return 0;
    if (bsp.root_address)
        return fail(result, CACHE_BSP_STATE, at + 28);
    if (value.file_offset < 0 || value.file_size < (int32_t)CACHE_BSP_HEADER_BYTES ||
        value.file_size > (int32_t)CACHE_BSP_TAG_LIMIT || (uint32_t)value.file_size > map_bytes ||
        (uint32_t)value.file_offset > map_bytes - (uint32_t)value.file_size)
        return fail(result, CACHE_BSP_SPAN, at);
    value.rounded_bytes = ((size_t)value.file_size + 511) & ~(size_t)511;
    if (!address_span(value.address, 1, value.rounded_bytes, tag_bytes, slot_bytes - tag_bytes, &root, result))
        return 0;
    value.slot_offset = root.offset;
    *output = value;
    return 1;
}

int cache_bsp_bind(struct cache_bsp_control *control, const struct cache_arena_owner *owner,
                   const struct cache_arena_handle *handle, size_t tag_bytes, uint32_t map_bytes,
                   size_t ordinal, struct cache_bsp_view *output, struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!control || !output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    if (control->generation == UINT64_MAX)
        return fail(result, CACHE_BSP_GENERATION, 0);
    struct cache_bsp_control next = {0};
    if (!cache_bsp_select(owner, handle, tag_bytes, map_bytes, ordinal, &next.reference, result))
        return 0;
    unsigned char *bytes;
    if (!slot(owner, handle, &bytes, &next.slot_bytes, result))
        return 0;
    size_t at = next.reference.slot_offset;
    next.header = (struct cache_bsp_header){le32(bytes + at), s32(bytes + at + 4),
        le32(bytes + at + 8), s32(bytes + at + 12), le32(bytes + at + 16), le32(bytes + at + 20)};
    if (next.header.signature != BSP_GROUP)
        return fail(result, CACHE_BSP_SIGNATURE, at + 20);
    if (!address_span(next.header.root_address, 1, CACHE_BSP_ROOT_BYTES, at,
                       (size_t)next.reference.file_size, &next.root, result))
        return 0;
    struct cache_address_span claimed[4] = {{at, CACHE_BSP_HEADER_BYTES}, next.root, {0, 0}, {0, 0}};
    for (unsigned kind = 0; kind < 2; ++kind)
    {
        int32_t count = kind ? next.header.index_count : next.header.vertex_count;
        uint32_t address = kind ? next.header.index_address : next.header.vertex_address;
        if (count < 0 || (uint32_t)count > (uint32_t)next.reference.file_size / 12)
            return fail(result, CACHE_BSP_COUNT, at + 4 + kind * 8);
        if (!address_span(address, count, 12, at, (size_t)next.reference.file_size,
                           &next.descriptors[kind], result))
            return 0;
        claimed[kind + 2] = next.descriptors[kind];
    }
    for (unsigned left = 0; left < 4; ++left)
        for (unsigned right = left + 1; right < 4; ++right)
            if (overlaps(claimed[left], claimed[right]))
                return fail(result, CACHE_BSP_OVERLAP, claimed[right].offset);
    for (unsigned kind = 0; kind < 2; ++kind)
        for (size_t index = 0; index < next.descriptors[kind].length / 12; ++index)
        {
            struct cache_address_span data;
            size_t descriptor = next.descriptors[kind].offset + index * 12;
            if (!address_span(le32(bytes + descriptor + 4), 1, 1, at,
                               (size_t)next.reference.file_size, &data, result))
                return 0;
        }
    next.owner = owner;
    next.handle = *handle;
    next.tag_bytes = tag_bytes;
    next.map_bytes = map_bytes;
    next.generation = control->generation + 1;
    next.live = 1;
    struct cache_bsp_view view = {owner, *handle, control, next.generation};
    *control = next;
    *output = view;
    return 1;
}

int cache_bsp_unload(struct cache_bsp_control *control, struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!control)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    if (!control->live)
        return 1;
    if (control->generation == UINT64_MAX)
        return fail(result, CACHE_BSP_GENERATION, 0);
    uint64_t generation = control->generation + 1;
    memset(control, 0, sizeof(*control));
    control->generation = generation;
    return 1;
}

static int live(const struct cache_bsp_view *view, unsigned char **bytes, struct cache_bsp_result *result)
{
    if (!view)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    size_t size;
    if (!slot(view->owner, &view->handle, bytes, &size, result))
        return 0;
    if (!view->control)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    const struct cache_bsp_control *control = view->control;
    if (!control->live || view->generation != control->generation || control->owner != view->owner ||
        control->handle.generation != view->handle.generation || control->handle.slot_index != view->handle.slot_index)
        return fail(result, CACHE_BSP_STATE, 0);
    if (size != control->slot_bytes)
        return fail(result, CACHE_BSP_STATE, 0);
    return 1;
}

int cache_bsp_lookup(const struct cache_bsp_view *view, struct cache_bsp_reference *output,
                     struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    if (!live(view, &bytes, result))
        return 0;
    *output = view->control->reference;
    return 1;
}

int cache_bsp_get_header(const struct cache_bsp_view *view, struct cache_bsp_header *output,
                         struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    if (!live(view, &bytes, result))
        return 0;
    *output = view->control->header;
    return 1;
}

int cache_bsp_get_root(const struct cache_bsp_view *view, struct cache_address_span *output,
                       struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    if (!live(view, &bytes, result))
        return 0;
    *output = view->control->root;
    return 1;
}

int cache_bsp_get_descriptor(const struct cache_bsp_view *view, unsigned kind, size_t index,
                             struct cache_bsp_descriptor *output, struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    if (!live(view, &bytes, result))
        return 0;
    if (kind > 1 || index >= view->control->descriptors[kind].length / 12)
        return fail(result, CACHE_BSP_COUNT, 0);
    size_t at = view->control->descriptors[kind].offset + index * 12;
    struct cache_bsp_descriptor value = {{le32(bytes + at), le32(bytes + at + 4), le32(bytes + at + 8)}, {at, 12}};
    *output = value;
    return 1;
}

int cache_bsp_resolve(const struct cache_bsp_view *view, uint32_t address, int64_t count, size_t stride,
                      struct cache_address_span *output, struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    if (!live(view, &bytes, result))
        return 0;
    struct cache_address_span value;
    const struct cache_bsp_control *control = view->control;
    if (!address_span(address, count, stride, 0, control->tag_bytes, &value, result))
    {
        if (result->error != CACHE_BSP_SPAN)
            return 0;
        if (!address_span(address, count, stride, control->reference.slot_offset,
                           (size_t)control->reference.file_size, &value, result))
            return 0;
        result->error = CACHE_BSP_OK;
        result->offset = 0;
    }
    *output = value;
    return 1;
}

int cache_bsp_serialize(const struct cache_bsp_view *view, void *output, size_t capacity,
                        size_t *used, struct cache_bsp_result *result)
{
    if (!start(result))
        return 0;
    if (!output || !used)
        return fail(result, CACHE_BSP_ARGUMENT, 0);
    unsigned char *bytes;
    if (!live(view, &bytes, result))
        return 0;
    const struct cache_bsp_control *control = view->control;
    size_t required = (size_t)control->reference.file_size;
    if (capacity < required)
        return fail(result, CACHE_BSP_CAPACITY, required);
    uintptr_t destination = (uintptr_t)output, source = (uintptr_t)bytes;
    if (required > UINTPTR_MAX - destination)
        return fail(result, CACHE_BSP_OVERFLOW, required);
    if (destination <= source ? source - destination < required : destination - source < control->slot_bytes)
        return fail(result, CACHE_BSP_OVERLAP, 0);
    unsigned char *serialized = output;
    memcpy(serialized, bytes + control->reference.slot_offset, required);
    uint32_t words[6] = {control->header.root_address, (uint32_t)control->header.vertex_count,
        control->header.vertex_address, (uint32_t)control->header.index_count,
        control->header.index_address, control->header.signature};
    for (unsigned word = 0; word < 6; ++word)
        put32(serialized + word * 4, words[word]);
    for (unsigned kind = 0; kind < 2; ++kind)
        for (size_t at = 0; at < control->descriptors[kind].length; at += 12)
            for (unsigned word = 0; word < 3; ++word)
            {
                size_t offset = control->descriptors[kind].offset + at + word * 4;
                put32(serialized + offset - control->reference.slot_offset, le32(bytes + offset));
            }
    *used = required;
    return 1;
}
