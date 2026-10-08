#include "cache_widget_probe.h"
#include <string.h>

static const size_t block_offsets[CACHE_WIDGET_BLOCKS] = {72, 84, 96, 724, 992};
static const size_t block_strides[CACHE_WIDGET_BLOCKS] = {36, 72, 34, 80, 80};
static const int32_t block_limits[CACHE_WIDGET_BLOCKS] = {64, 32, 32, 32, 32};
static const size_t reference_offsets[CACHE_WIDGET_REFERENCES] = {56, 236, 252, 340, 356, 420};
static const uint32_t reference_groups[CACHE_WIDGET_REFERENCES] = {
    UINT32_C(0x6269746d), UINT32_C(0x75737472), UINT32_C(0x666f6e74),
    UINT32_C(0x6269746d), UINT32_C(0x6269746d), CACHE_WIDGET_GROUP
};

struct widget_reader
{
    struct cache_address_region region;
    struct cache_address_header header;
    size_t table_offset;
};

struct widget_frame
{
    size_t node, next;
    unsigned height;
};

static int start(struct cache_widget_result *result)
{
    if (!result)
        return 0;
    result->error = CACHE_WIDGET_OK;
    result->offset = 0;
    result->node_index = SIZE_MAX;
    result->required = 0;
    return 1;
}

static int fail(struct cache_widget_result *result, enum cache_widget_error error, size_t offset)
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

static uint16_t le16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
}

static int32_t s32(const unsigned char *bytes)
{
    uint32_t word = le32(bytes);
    return word <= INT32_MAX ? (int32_t)word : -(int32_t)(~word) - 1;
}

static int16_t s16(const unsigned char *bytes)
{
    uint16_t word = le16(bytes);
    return word <= INT16_MAX ? (int16_t)word : (int16_t)(-(int32_t)(uint16_t)~word - 1);
}

static void put32(unsigned char *bytes, uint32_t word)
{
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = (unsigned char)(word >> (index * 8));
}

static void put16(unsigned char *bytes, uint16_t word)
{
    bytes[0] = (unsigned char)word;
    bytes[1] = (unsigned char)(word >> 8);
}

static int ranges_overlap(const void *left, size_t left_size, const void *right, size_t right_size)
{
    uintptr_t a = (uintptr_t)left;
    uintptr_t b = (uintptr_t)right;
    if (!left_size || !right_size)
        return 0;
    return a <= b ? b - a < left_size : a - b < right_size;
}

static int address_failure(struct cache_widget_result *result, const struct cache_address_result *nested)
{
    enum cache_widget_error error = CACHE_WIDGET_SPAN;
    if (nested->error == CACHE_ADDRESS_OVERFLOW)
        error = CACHE_WIDGET_OVERFLOW;
    else if (nested->error == CACHE_ADDRESS_COUNT)
        error = CACHE_WIDGET_COUNT;
    else if (nested->error == CACHE_ADDRESS_ARGUMENT)
        error = CACHE_WIDGET_ARGUMENT;
    return fail(result, error, nested->offset);
}

static int resolve(const struct widget_reader *reader, uint32_t address, int64_t count,
                   size_t stride, struct cache_address_span *span, struct cache_widget_result *result)
{
    struct cache_address_result nested;
    if (!cache_address_resolve(&reader->region, address, count, stride, span, &nested))
        return address_failure(result, &nested);
    return 1;
}

static int fixed_string(const struct widget_reader *reader, size_t offset, size_t length,
                        struct cache_address_span *span, struct cache_widget_result *result)
{
    const unsigned char *end = memchr(reader->region.bytes + offset, 0, length);
    if (!end)
        return fail(result, CACHE_WIDGET_STRING, offset);
    span->offset = offset;
    span->length = (size_t)(end - (reader->region.bytes + offset)) + 1;
    return 1;
}

static int addressed_string(const struct widget_reader *reader, uint32_t address,
                            struct cache_address_span *span, struct cache_widget_result *result)
{
    if (!resolve(reader, address, 1, 1, span, result))
        return 0;
    return fixed_string(reader, span->offset, reader->region.size - span->offset, span, result);
}

static int instance(const struct widget_reader *reader, uint32_t datum, uint32_t group,
                    struct cache_address_instance *output, struct cache_widget_result *result)
{
    size_t ordinal = datum & UINT32_C(0xffff);
    struct cache_address_result nested;
    if (datum == UINT32_MAX || ordinal >= (size_t)reader->header.tag_count)
        return fail(result, CACHE_WIDGET_DATUM, ordinal);
    if (!cache_address_decode_instance(reader->region.bytes, reader->region.size,
                                        reader->table_offset + ordinal * 32, output, &nested))
        return address_failure(result, &nested);
    if (output->datum != datum)
        return fail(result, CACHE_WIDGET_DATUM, reader->table_offset + ordinal * 32 + 12);
    if (output->group != group)
        return fail(result, CACHE_WIDGET_GROUP_ERROR, reader->table_offset + ordinal * 32);
    return 1;
}

static int reference(const struct widget_reader *reader, size_t offset, uint32_t group,
                     struct cache_widget_reference *output, struct cache_widget_result *result)
{
    const unsigned char *bytes = reader->region.bytes + offset;
    struct cache_widget_reference value;
    memset(&value, 0, sizeof(value));
    value.group = le32(bytes);
    value.name_address = le32(bytes + 4);
    value.name_length_word = le32(bytes + 8);
    value.datum = le32(bytes + 12);
    /* NONE references may carry meaningless name pointers (tag_validate.c:702).
     * Preserve those words, but never follow them. */
    if (value.datum != UINT32_MAX)
    {
        struct cache_address_instance target;
        struct cache_address_span target_name;
        if (value.group != group)
            return fail(result, CACHE_WIDGET_GROUP_ERROR, offset);
        if (!instance(reader, value.datum, group, &target, result) ||
            !addressed_string(reader, value.name_address, &value.name, result) ||
            !addressed_string(reader, target.name_address, &target_name, result))
            return 0;
    }
    *output = value;
    return 1;
}

static int element(const struct widget_reader *reader, enum cache_widget_block_kind kind,
                   size_t offset, struct cache_widget_element *output, struct cache_widget_result *result)
{
    const unsigned char *bytes = reader->region.bytes + offset;
    struct cache_widget_element value;
    memset(&value, 0, sizeof(value));
    value.kind = kind;
    value.raw.offset = offset;
    value.raw.length = block_strides[kind];
    switch (kind)
    {
    case CACHE_WIDGET_INPUT:
        value.function = s16(bytes);
        break;
    case CACHE_WIDGET_EVENT:
        value.flags = le32(bytes);
        value.event_type = s16(bytes + 4);
        value.function = s16(bytes + 6);
        if (!reference(reader, offset + 8, CACHE_WIDGET_GROUP, &value.references[0], result) ||
            !reference(reader, offset + 24, UINT32_C(0x736e6421), &value.references[1], result) ||
            !fixed_string(reader, offset + 40, 32, &value.text, result))
            return 0;
        break;
    case CACHE_WIDGET_REPLACE:
        if (!fixed_string(reader, offset, 32, &value.text, result))
            return 0;
        value.function = s16(bytes + 32);
        break;
    case CACHE_WIDGET_CONDITIONAL:
    case CACHE_WIDGET_CHILD:
        if (!reference(reader, offset, CACHE_WIDGET_GROUP, &value.references[0], result) ||
            !fixed_string(reader, offset + 16, 32, &value.text, result))
            return 0;
        value.flags = le32(bytes + 48);
        value.controller = s16(bytes + 52);
        if (kind == CACHE_WIDGET_CHILD)
        {
            value.vertical_offset = s16(bytes + 54);
            value.horizontal_offset = s16(bytes + 56);
        }
        break;
    }
    *output = value;
    return 1;
}

static int root(const struct widget_reader *reader, uint32_t datum,
                struct cache_widget_projection *output, struct cache_widget_result *result)
{
    struct cache_address_instance target;
    struct cache_widget_projection value;
    struct cache_address_span target_name;
    memset(&value, 0, sizeof(value));
    if (!instance(reader, datum, CACHE_WIDGET_GROUP, &target, result) ||
        !addressed_string(reader, target.name_address, &target_name, result) ||
        !resolve(reader, target.root_address, 1, CACHE_WIDGET_ROOT_BYTES, &value.raw, result))
        return 0;
    const unsigned char *bytes = reader->region.bytes + value.raw.offset;
    value.datum = datum;
    value.type = s16(bytes);
    value.controller = s16(bytes + 2);
    value.justification = s16(bytes + 284);
    if (value.type < 0 || value.type >= 7 || value.controller < 0 || value.controller >= 5 ||
        value.justification < 0 || value.justification >= 3)
        return fail(result, CACHE_WIDGET_ENUM, value.raw.offset);
    if (!fixed_string(reader, value.raw.offset + 4, 32, &value.name, result))
        return 0;
    for (size_t index = 0; index < 4; ++index)
    {
        value.bounds[index] = s16(bytes + 36 + index * 2);
        value.text_color_bits[index] = le32(bytes + 268 + index * 4);
        value.list_header_bounds[index] = s16(bytes + 372 + index * 2);
        value.list_footer_bounds[index] = s16(bytes + 380 + index * 2);
    }
    value.flags = le32(bytes + 44);
    value.auto_close_word = le32(bytes + 48);
    value.fade_word = le32(bytes + 52);
    value.text_box_flags = le16(bytes + 286);
    value.string_list_index = s16(bytes + 302);
    value.horizontal_offset = s16(bytes + 304);
    value.vertical_offset = s16(bytes + 306);
    value.list_flags = le32(bytes + 336);
    for (size_t index = 0; index < CACHE_WIDGET_REFERENCES; ++index)
    {
        if (!reference(reader, value.raw.offset + reference_offsets[index], reference_groups[index],
                       &value.references[index], result))
            return 0;
    }
    for (size_t index = 0; index < CACHE_WIDGET_BLOCKS; ++index)
    {
        struct cache_widget_block *block = &value.blocks[index];
        const unsigned char *field = bytes + block_offsets[index];
        block->count = s32(field);
        block->address = le32(field + 4);
        block->definition_word = le32(field + 8);
        block->stride = block_strides[index];
        if (block->count < 0 || block->count > block_limits[index])
            return fail(result, CACHE_WIDGET_COUNT, value.raw.offset + block_offsets[index]);
        if (!resolve(reader, block->address, block->count, block->stride, &block->span, result))
            return 0;
        for (size_t element_index = 0; element_index < (size_t)block->count; ++element_index)
        {
            struct cache_widget_element item;
            if (!element(reader, (enum cache_widget_block_kind)index,
                         block->span.offset + element_index * block->stride, &item, result))
                return 0;
        }
    }
    *output = value;
    return 1;
}

static struct cache_address_span node_span(const struct cache_widget_projection *node, size_t index)
{
    return index ? node->blocks[index - 1].span : node->raw;
}

static int claims(const struct cache_widget_projection *nodes, size_t count,
                  struct cache_widget_result *result)
{
    const struct cache_widget_projection *latest = &nodes[count - 1];
    for (size_t index = 0; index <= CACHE_WIDGET_BLOCKS; ++index)
    {
        struct cache_address_span left = node_span(latest, index);
        if (!left.length)
            continue;
        for (size_t prior = 0; prior < count; ++prior)
        {
            size_t limit = prior + 1 == count ? index : CACHE_WIDGET_BLOCKS + 1;
            for (size_t field = 0; field < limit; ++field)
            {
                struct cache_address_span right = node_span(&nodes[prior], field);
                if (right.length && (left.offset <= right.offset ?
                        right.offset - left.offset < left.length :
                        left.offset - right.offset < right.length))
                    return fail(result, CACHE_WIDGET_OVERLAP, left.offset);
            }
        }
    }
    return 1;
}

static int reader_open(const struct cache_arena_owner *owner, const struct cache_arena_handle *handle,
                       size_t resident_size, uint32_t base, struct widget_reader *reader,
                       struct cache_widget_result *result)
{
    struct cache_arena_result arena_result;
    struct cache_address_result address_result;
    unsigned char *bytes;
    if (!owner || !handle)
        return fail(result, CACHE_WIDGET_ARGUMENT, 0);
    if (!cache_arena_owner_resolve(owner, handle, 0, resident_size, &bytes, &arena_result))
        return fail(result, CACHE_WIDGET_STATE, 0);
    if (resident_size > (size_t)0x01600000)
        return fail(result, CACHE_WIDGET_SPAN, resident_size);
    reader->region.bytes = bytes;
    reader->region.size = resident_size;
    reader->region.encoded_base = base;
    if (!cache_address_decode_header(&reader->region, &reader->header, &address_result))
        return address_failure(result, &address_result);
    reader->table_offset = reader->header.instances_address - base;
    return 1;
}

const char *cache_widget_error_name(enum cache_widget_error error)
{
    static const char *const names[] = {
        "ok", "argument", "state", "workspace", "count", "span", "overflow", "enum", "string",
        "datum", "group", "overlap", "cycle", "depth", "capacity"
    };
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

int cache_widget_workspace_size(size_t capacity, size_t *output, struct cache_widget_result *result)
{
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_WIDGET_ARGUMENT, 0);
    if (!capacity || capacity > 65535)
        return fail(result, CACHE_WIDGET_WORKSPACE, 0);
    if (capacity > SIZE_MAX / sizeof(struct cache_widget_projection) / 2)
        return fail(result, CACHE_WIDGET_OVERFLOW, 0);
    *output = capacity * sizeof(struct cache_widget_projection) * 2;
    result->required = *output;
    return 1;
}

int cache_widget_graph_decode(const struct cache_arena_owner *owner, const struct cache_arena_handle *handle,
                              size_t resident_size, uint32_t base, uint32_t root_datum,
                              void *workspace, size_t workspace_bytes, size_t capacity,
                              struct cache_widget_graph *output, struct cache_widget_result *result)
{
    size_t required;
    struct widget_reader reader;
    struct widget_frame frames[CACHE_WIDGET_MAX_DEPTH + 1];
    size_t depth = 0;
    size_t count = 1;
    size_t serialized = 0;
    if (!cache_widget_workspace_size(capacity, &required, result))
        return 0;
    if (!workspace || !output || !handle)
        return fail(result, CACHE_WIDGET_ARGUMENT, 0);
    if ((uintptr_t)workspace % _Alignof(struct cache_widget_projection) || workspace_bytes < required)
        return fail(result, CACHE_WIDGET_WORKSPACE, 0);
    if (workspace_bytes > UINTPTR_MAX - (uintptr_t)workspace)
        return fail(result, CACHE_WIDGET_OVERFLOW, 0);
    if (!reader_open(owner, handle, resident_size, base, &reader, result))
        return 0;
    if (ranges_overlap(workspace, workspace_bytes, reader.region.bytes, resident_size))
        return fail(result, CACHE_WIDGET_OVERLAP, 0);
    struct cache_widget_projection *published = workspace;
    struct cache_widget_projection *scratch = published + capacity;
    if (!root(&reader, root_datum, &scratch[0], result) || !claims(scratch, 1, result))
        return 0;
    scratch[0].state = 1;
    frames[0] = (struct widget_frame){0, 0, 0};
    for (;;)
    {
        struct widget_frame *frame = &frames[depth];
        struct cache_widget_projection *node = &scratch[frame->node];
        size_t edge_count = (size_t)node->blocks[CACHE_WIDGET_CHILD].count + (node->type == 3 ? 1 : 0);
        if (frame->next == edge_count)
        {
            node->state = 2;
            node->height = frame->height;
            if (!depth)
                break;
            --depth;
            if (frames[depth].height < node->height + 1)
                frames[depth].height = node->height + 1;
            continue;
        }
        uint32_t datum;
        if (frame->next < (size_t)node->blocks[CACHE_WIDGET_CHILD].count)
        {
            size_t offset = node->blocks[CACHE_WIDGET_CHILD].span.offset + frame->next * 80;
            datum = le32(reader.region.bytes + offset + 12);
        }
        else
            datum = node->references[5].datum;
        ++frame->next;
        if (datum == UINT32_MAX)
            continue;
        size_t next = 0;
        while (next < count && scratch[next].datum != datum)
            ++next;
        if (next < count)
        {
            if (scratch[next].state == 1)
                return fail(result, CACHE_WIDGET_CYCLE, scratch[next].raw.offset);
            if (depth + 1 + scratch[next].height > CACHE_WIDGET_MAX_DEPTH)
                return fail(result, CACHE_WIDGET_DEPTH, scratch[next].raw.offset);
            if (frame->height < scratch[next].height + 1)
                frame->height = scratch[next].height + 1;
            continue;
        }
        if (depth + 1 > CACHE_WIDGET_MAX_DEPTH)
            return fail(result, CACHE_WIDGET_DEPTH, node->raw.offset);
        if (count == capacity)
            return fail(result, CACHE_WIDGET_WORKSPACE, node->raw.offset);
        result->node_index = count;
        if (!root(&reader, datum, &scratch[count], result) || !claims(scratch, count + 1, result))
            return 0;
        scratch[count].state = 1;
        ++depth;
        frames[depth] = (struct widget_frame){count, 0, 0};
        ++count;
    }
    for (size_t index = 0; index < count; ++index)
    {
        for (size_t field = 0; field <= CACHE_WIDGET_BLOCKS; ++field)
        {
            size_t bytes = node_span(&scratch[index], field).length;
            if (bytes > SIZE_MAX - serialized)
                return fail(result, CACHE_WIDGET_OVERFLOW, scratch[index].raw.offset);
            serialized += bytes;
        }
    }
    memcpy(published, scratch, count * sizeof(*published));
    struct cache_widget_graph value = {owner, *handle, resident_size, base, published,
                                        count, capacity, serialized};
    *output = value;
    result->node_index = SIZE_MAX;
    result->required = serialized;
    return 1;
}

static int graph_reader(const struct cache_widget_graph *graph, struct widget_reader *reader,
                        struct cache_widget_result *result)
{
    if (!graph || !graph->owner || !graph->nodes || !graph->count || graph->count > graph->capacity)
        return fail(result, CACHE_WIDGET_STATE, 0);
    return reader_open(graph->owner, &graph->tag_handle, graph->resident_size, graph->encoded_base,
                       reader, result);
}

int cache_widget_graph_node(const struct cache_widget_graph *graph, size_t index,
                            struct cache_widget_projection *output, struct cache_widget_result *result)
{
    struct widget_reader reader;
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_WIDGET_ARGUMENT, 0);
    if (!graph_reader(graph, &reader, result))
        return 0;
    if (index >= graph->count)
        return fail(result, CACHE_WIDGET_COUNT, index);
    *output = graph->nodes[index];
    return 1;
}

int cache_widget_graph_element(const struct cache_widget_graph *graph, size_t node_index,
                               enum cache_widget_block_kind kind, size_t index,
                               struct cache_widget_element *output, struct cache_widget_result *result)
{
    struct widget_reader reader;
    if (!start(result))
        return 0;
    if (!output)
        return fail(result, CACHE_WIDGET_ARGUMENT, 0);
    if (!graph_reader(graph, &reader, result))
        return 0;
    if (node_index >= graph->count || (unsigned)kind >= CACHE_WIDGET_BLOCKS)
        return fail(result, CACHE_WIDGET_COUNT, node_index);
    const struct cache_widget_block *block = &graph->nodes[node_index].blocks[kind];
    if (index >= (size_t)block->count)
        return fail(result, CACHE_WIDGET_COUNT, index);
    return element(&reader, kind, block->span.offset + index * block->stride, output, result);
}

static void write_reference(unsigned char *bytes, const struct cache_widget_reference *reference_value)
{
    put32(bytes, reference_value->group);
    put32(bytes + 4, reference_value->name_address);
    put32(bytes + 8, reference_value->name_length_word);
    put32(bytes + 12, reference_value->datum);
}

static void write_root(unsigned char *bytes, const struct cache_widget_projection *node)
{
    put16(bytes, (uint16_t)node->type);
    put16(bytes + 2, (uint16_t)node->controller);
    put32(bytes + 44, node->flags);
    put32(bytes + 48, node->auto_close_word);
    put32(bytes + 52, node->fade_word);
    for (size_t index = 0; index < 4; ++index)
    {
        put16(bytes + 36 + index * 2, (uint16_t)node->bounds[index]);
        put32(bytes + 268 + index * 4, node->text_color_bits[index]);
        put16(bytes + 372 + index * 2, (uint16_t)node->list_header_bounds[index]);
        put16(bytes + 380 + index * 2, (uint16_t)node->list_footer_bounds[index]);
    }
    for (size_t index = 0; index < CACHE_WIDGET_REFERENCES; ++index)
        write_reference(bytes + reference_offsets[index], &node->references[index]);
    for (size_t index = 0; index < CACHE_WIDGET_BLOCKS; ++index)
    {
        unsigned char *field = bytes + block_offsets[index];
        put32(field, (uint32_t)node->blocks[index].count);
        put32(field + 4, node->blocks[index].address);
        put32(field + 8, node->blocks[index].definition_word);
    }
    put16(bytes + 284, (uint16_t)node->justification);
    put16(bytes + 286, node->text_box_flags);
    put16(bytes + 302, (uint16_t)node->string_list_index);
    put16(bytes + 304, (uint16_t)node->horizontal_offset);
    put16(bytes + 306, (uint16_t)node->vertical_offset);
    put32(bytes + 336, node->list_flags);
}

static void write_element(unsigned char *bytes, const struct cache_widget_element *value)
{
    switch (value->kind)
    {
    case CACHE_WIDGET_INPUT:
        put16(bytes, (uint16_t)value->function);
        break;
    case CACHE_WIDGET_EVENT:
        put32(bytes, value->flags);
        put16(bytes + 4, (uint16_t)value->event_type);
        put16(bytes + 6, (uint16_t)value->function);
        write_reference(bytes + 8, &value->references[0]);
        write_reference(bytes + 24, &value->references[1]);
        break;
    case CACHE_WIDGET_REPLACE:
        put16(bytes + 32, (uint16_t)value->function);
        break;
    case CACHE_WIDGET_CONDITIONAL:
    case CACHE_WIDGET_CHILD:
        write_reference(bytes, &value->references[0]);
        put32(bytes + 48, value->flags);
        put16(bytes + 52, (uint16_t)value->controller);
        if (value->kind == CACHE_WIDGET_CHILD)
        {
            put16(bytes + 54, (uint16_t)value->vertical_offset);
            put16(bytes + 56, (uint16_t)value->horizontal_offset);
        }
        break;
    }
}

int cache_widget_graph_serialize(const struct cache_widget_graph *graph, void *destination,
                                 size_t capacity, size_t *used, struct cache_widget_result *result)
{
    struct widget_reader reader;
    if (!start(result))
        return 0;
    if (!destination || !used)
        return fail(result, CACHE_WIDGET_ARGUMENT, 0);
    if (!graph_reader(graph, &reader, result))
        return 0;
    result->required = graph->serialized_size;
    if (capacity > UINTPTR_MAX - (uintptr_t)destination)
        return fail(result, CACHE_WIDGET_OVERFLOW, 0);
    if (capacity < graph->serialized_size)
        return fail(result, CACHE_WIDGET_CAPACITY, 0);
    if (ranges_overlap(destination, capacity, reader.region.bytes, reader.region.size) ||
        ranges_overlap(destination, capacity, graph->nodes, graph->capacity * sizeof(*graph->nodes)))
        return fail(result, CACHE_WIDGET_OVERLAP, 0);
    /* Complete record preflight precedes every destination write. Source,
     * owner and published projections stay immutable for the whole call. */
    for (size_t index = 0; index < graph->count; ++index)
    {
        const struct cache_widget_projection *node = &graph->nodes[index];
        for (size_t kind = 0; kind < CACHE_WIDGET_BLOCKS; ++kind)
        {
            const struct cache_widget_block *block = &node->blocks[kind];
            for (size_t record = 0; record < (size_t)block->count; ++record)
            {
                struct cache_widget_element value;
                if (!element(&reader, (enum cache_widget_block_kind)kind,
                             block->span.offset + record * block->stride, &value, result))
                    return 0;
            }
        }
    }
    unsigned char *bytes = destination;
    size_t cursor = 0;
    for (size_t index = 0; index < graph->count; ++index)
    {
        const struct cache_widget_projection *node = &graph->nodes[index];
        memcpy(bytes + cursor, reader.region.bytes + node->raw.offset, node->raw.length);
        write_root(bytes + cursor, node);
        cursor += node->raw.length;
        for (size_t kind = 0; kind < CACHE_WIDGET_BLOCKS; ++kind)
        {
            const struct cache_widget_block *block = &node->blocks[kind];
            if (!block->span.length)
                continue;
            memcpy(bytes + cursor, reader.region.bytes + block->span.offset, block->span.length);
            for (size_t record = 0; record < (size_t)block->count; ++record)
            {
                struct cache_widget_element value;
                /* The identical decode succeeded during preflight; the pinned
                 * immutable source makes this second pass infallible. */
                (void)element(&reader, (enum cache_widget_block_kind)kind,
                              block->span.offset + record * block->stride, &value, result);
                write_element(bytes + cursor + record * block->stride, &value);
            }
            cursor += block->span.length;
        }
    }
    *used = cursor;
    return 1;
}

void cache_widget_graph_release(struct cache_widget_graph *graph)
{
    if (graph)
        memset(graph, 0, sizeof(*graph));
}
