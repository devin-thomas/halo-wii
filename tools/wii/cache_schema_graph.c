#include "cache_schema_graph.h"
#include "cache_schema_tables.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NONE_WORD UINT32_C(0xffffffff)
#define TAGS_SIGNATURE UINT32_C(0x74616773)  /* 'tags' */
#define SBSP_GROUP UINT32_C(0x73627370)      /* 'sbsp' */
#define SCENARIO_GROUP UINT32_C(0x73636e72)  /* 'scnr' */
#define HEADER_BYTES 36u
#define INSTANCE_BYTES 32u
#define BUFFER_BYTES 12u
#define BSP_HEADER_BYTES 24u
#define MAX_DEPTH 32

enum { PASS_EXTENTS, PASS_VALUES };
enum { EVENT_TAG = 1, EVENT_BLOCK, EVENT_DATA, EVENT_FILE_DATA, EVENT_REFERENCE, EVENT_TAG_INDEX,
       EVENT_BLOCK_INDEX, EVENT_ENUM, EVENT_STRING, EVENT_BSP, EVENT_BUFFER, EVENT_INSTANCE, EVENT_SUPPLEMENTARY };

struct walk {
    const struct cache_graph_input *in;
    struct cache_graph_report *report;
    struct cache_graph_result *result;
    uint32_t *claims, *relocate, *nulls;
    uint16_t *edges;          /* from, to pairs */
    unsigned char *reached;
    uint16_t *queue;
    size_t region, region_bytes; /* slot offsets the current walk's pointers may target */
    size_t instances;         /* slot offset of the tag table */
    uint32_t tag_count;
    long tag;                 /* current tag ordinal */
    size_t frames[MAX_DEPTH];
    int depth;
    size_t structure;
    int failed;
    uint32_t crc;
};

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t index = 0; index < 256; ++index) {
        uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1u)));
        crc_table[index] = crc;
    }
}

static uint32_t crc_words(uint32_t crc, const uint32_t *words, size_t count)
{
    crc = ~crc;
    for (size_t index = 0; index < count; ++index)
        for (int shift = 0; shift < 32; shift += 8)
            crc = (crc >> 8) ^ crc_table[(crc ^ (words[index] >> shift)) & 0xffu];
    return ~crc;
}

static void emit(struct walk *w, uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t words[5] = {kind, a, b, c, d};
    w->crc = crc_words(w->crc, words, 5);
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t s32(uint32_t value)
{
    return value <= INT32_MAX ? (int32_t)value : -(int32_t)(~value) - 1;
}

static int fail(struct walk *w, enum cache_graph_error error, size_t offset)
{
    if (!w->failed) {
        w->failed = 1;
        w->result->error = error;
        w->result->offset = offset;
        w->result->tag_ordinal = w->tag;
    }
    return 0;
}

const char *cache_graph_error_name(enum cache_graph_error error)
{
    static const char *const names[] = {"ok", "argument", "workspace", "header", "table", "root", "extent",
        "overlap", "depth", "alignment", "bsp", "edges", "schema", "native"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

static size_t claim_bytes(size_t cache_bytes) { return ((cache_bytes + 31) / 32) * 4; }
static size_t word_bitmap_bytes(size_t cache_bytes) { return ((cache_bytes / 4 + 31) / 32) * 4; }

size_t cache_graph_workspace_bytes(size_t cache_bytes)
{
    return claim_bytes(cache_bytes) + 2 * word_bitmap_bytes(cache_bytes) + CACHE_GRAPH_MAX_EDGES * 4u +
           CACHE_GRAPH_MAX_TAGS + 1 + CACHE_GRAPH_MAX_TAGS * 2u + 8;
}

/* A pointer field's target as a slot offset; 0 when it names nothing addressable. */
static int pointer_offset(const struct walk *w, size_t field, size_t *offset)
{
    const unsigned char *p = w->in->tags + field;
    if (w->in->native) {
        uint32_t value;
        memcpy(&value, p, 4);
        uint32_t base = (uint32_t)(uintptr_t)w->in->tags;
        if (value < base)
            return 0;
        *offset = value - base;
    } else {
        uint32_t value = le32(p);
        if (value < w->in->encoded_base)
            return 0;
        *offset = value - w->in->encoded_base;
    }
    return 1;
}

static int region_contains(const struct walk *w, size_t offset, size_t size)
{
    return offset >= w->region && offset - w->region <= w->region_bytes &&
           size <= w->region_bytes - (offset - w->region);
}

static int claim(struct walk *w, size_t first, size_t size)
{
    size_t end, bit;
    if (!size)
        return 1;
    if (first > w->in->cache_bytes || size > w->in->cache_bytes - first)
        return 0;
    end = first + size;
    bit = first;
    while (bit < end) {
        uint32_t *word = &w->claims[bit / 32];
        if (bit % 32 == 0 && end - bit >= 32) {
            if (*word)
                return 0;
            *word = UINT32_MAX;
            bit += 32;
        } else {
            uint32_t mask = UINT32_C(1) << (bit % 32);
            if (*word & mask)
                return 0;
            *word |= mask;
            ++bit;
        }
    }
    w->report->claimed_bytes += (uint32_t)size;
    return 1;
}

static int claimed(const struct walk *w, size_t offset)
{
    return (w->claims[offset / 32] >> (offset % 32)) & 1u;
}

/* Marks a pointer field for relocation (target) or nulling. */
static int mark(struct walk *w, size_t field, int null)
{
    if (field % 4)
        return fail(w, CACHE_GRAPH_ALIGNMENT, field);
    uint32_t *bitmap = null ? w->nulls : w->relocate;
    size_t index = field / 4;
    if (!((bitmap[index / 32] >> (index % 32)) & 1u)) {
        bitmap[index / 32] |= UINT32_C(1) << (index % 32);
        if (null)
            ++w->report->null_fields;
        else
            ++w->report->pointer_fields;
    }
    return 1;
}

static int marked(const uint32_t *bitmap, size_t field)
{
    size_t index = field / 4;
    return (bitmap[index / 32] >> (index % 32)) & 1u;
}

/* A string the walk may trust: in the current region, or in the tags. */
static int string_valid(const struct walk *w, size_t pointer_field, size_t *offset)
{
    if (!pointer_offset(w, pointer_field, offset))
        return 0;
    if (region_contains(w, *offset, 1))
        return memchr(w->in->tags + *offset, 0, w->region_bytes - (*offset - w->region)) != NULL;
    if (*offset < w->in->tag_bytes)
        return memchr(w->in->tags + *offset, 0, w->in->tag_bytes - *offset) != NULL;
    return 0;
}

static size_t instance_at(const struct walk *w, uint32_t ordinal)
{
    return w->instances + (size_t)ordinal * INSTANCE_BYTES;
}

static int group_in(uint32_t group, uint32_t parent0, uint32_t parent1, int32_t list)
{
    if (list < 0)
        return 1;
    for (const uint32_t *groups = cache_schema_group_lists + list; *groups; ++groups)
        if (group == *groups || parent0 == *groups || parent1 == *groups)
            return 1;
    return 0;
}

static const struct cache_schema_group *schema_group(uint32_t group_tag)
{
    for (int32_t index = 0; index < cache_schema_counts.groups; ++index)
        if (cache_schema_groups[index].group_tag == group_tag)
            return &cache_schema_groups[index];
    return NULL;
}

static int add_edge(struct walk *w, uint32_t to)
{
    if (w->tag < 0)
        return 1;
    if (w->report->edges >= CACHE_GRAPH_MAX_EDGES)
        return fail(w, CACHE_GRAPH_EDGES, 0);
    w->edges[w->report->edges * 2] = (uint16_t)w->tag;
    w->edges[w->report->edges * 2 + 1] = (uint16_t)to;
    ++w->report->edges;
    return 1;
}

/* tag_validate.c validate_tag_index: 1 resolved (ordinal set), 0 none, -1 would be corrected */
static int tag_index(struct walk *w, uint32_t index, const uint32_t *group_tag, int32_t list, uint32_t *ordinal)
{
    if (index == NONE_WORD)
        return 0;
    int16_t absolute = (int16_t)(uint16_t)(index & 0xffffu);
    if (absolute < 0 || (uint32_t)absolute >= w->tag_count)
        return -1;
    const unsigned char *instance = w->in->tags + instance_at(w, (uint32_t)absolute);
    if (le32(instance + 12) != index ||
        !group_in(le32(instance), le32(instance + 4), le32(instance + 8), list))
        return -1;
    if (group_tag && *group_tag != le32(instance))
        ++w->report->corrections;
    *ordinal = (uint32_t)absolute;
    return add_edge(w, *ordinal) ? 1 : -2;
}

static long integer_get(const unsigned char *p, int size, int is_unsigned)
{
    if (size == 1)
        return is_unsigned ? (long)p[0] : (long)(int8_t)p[0];
    if (size == 2) {
        uint16_t value = (uint16_t)(p[0] | p[1] << 8);
        return is_unsigned ? (long)value : (long)(int16_t)value;
    }
    return (long)s32(le32(p));
}

static long integer_none(int size, int is_unsigned)
{
    if (!is_unsigned)
        return -1;
    return size == 1 ? 0xff : size == 2 ? 0xffff : -1;
}

static void walk_fields(struct walk *, size_t base, int32_t definition, int pass);

static void walk_element(struct walk *w, size_t base, int32_t definition)
{
    if (w->failed)
        return;
    if (w->depth >= MAX_DEPTH) {
        fail(w, CACHE_GRAPH_DEPTH, base);
        return;
    }
    w->frames[w->depth++] = base;
    if ((uint32_t)w->depth > w->report->max_depth)
        w->report->max_depth = (uint32_t)w->depth;
    ++w->report->elements;
    walk_fields(w, base, definition, PASS_EXTENTS);
    if (!w->failed)
        walk_fields(w, base, definition, PASS_VALUES);
    --w->depth;
}

static uint32_t block_count(const struct walk *w, size_t address, const struct cache_schema_field *field)
{
    int32_t count = s32(le32(w->in->tags + address));
    if (count < 0)
        return 0;
    if (field->maximum > 0 && count > field->maximum)
        count = field->maximum;
    return (uint32_t)count;
}

static void block_extent(struct walk *w, size_t address, const struct cache_schema_field *field, uint32_t id)
{
    int32_t raw = s32(le32(w->in->tags + address));
    size_t element = (size_t)cache_schema_definitions[field->link].size;
    size_t target = 0;
    ++w->report->blocks;
    if (raw < 0) {
        fail(w, CACHE_GRAPH_EXTENT, address);
        return;
    }
    uint32_t count = block_count(w, address, field);
    if ((uint32_t)raw != count)
        ++w->report->corrections;
    mark(w, address + 8, 1);  /* (the validator nulls a block's runtime definition pointer) */
    if (count) {
        if (!pointer_offset(w, address + 4, &target) || count > w->region_bytes / element ||
            !region_contains(w, target, count * element)) {
            fail(w, CACHE_GRAPH_EXTENT, address);
            return;
        }
        if (!claim(w, target, count * element)) {
            fail(w, CACHE_GRAPH_OVERLAP, address);
            return;
        }
        ++w->report->nonempty_blocks;
        mark(w, address + 4, 0);
    } else {
        mark(w, address + 4, 1);
    }
    emit(w, EVENT_BLOCK, id, (uint32_t)raw, count, count ? (uint32_t)target : NONE_WORD);
}

static void data_extent(struct walk *w, size_t address, const struct cache_schema_field *field, uint32_t id)
{
    int32_t size = s32(le32(w->in->tags + address));
    size_t target = 0;
    if (size < 0) {
        fail(w, CACHE_GRAPH_EXTENT, address);
        return;
    }
    if (field->maximum > 0 && size > field->maximum) {
        ++w->report->corrections;
        size = field->maximum;
    }
    mark(w, address + 16, 1);  /* (and data's) */
    if (field->type == CACHE_SCHEMA_FILE_DATA) {
        int32_t file_offset = s32(le32(w->in->tags + address + 8));
        ++w->report->file_data_fields;
        w->report->file_data_bytes += (uint32_t)size;
        if (file_offset < 0 && size)
            ++w->report->corrections;
        emit(w, EVENT_FILE_DATA, id, (uint32_t)size, (uint32_t)file_offset, 0);
        return;
    }
    ++w->report->data_fields;
    if (size) {
        if (!pointer_offset(w, address + 12, &target) || !region_contains(w, target, (size_t)size)) {
            fail(w, CACHE_GRAPH_EXTENT, address);
            return;
        }
        if (!claim(w, target, (size_t)size)) {
            fail(w, CACHE_GRAPH_OVERLAP, address);
            return;
        }
        w->report->data_bytes += (uint32_t)size;
        mark(w, address + 12, 0);
    } else {
        mark(w, address + 12, 1);
    }
    emit(w, EVENT_DATA, id, (uint32_t)size, size ? (uint32_t)target : NONE_WORD, 0);
}

static size_t block_index_target(const struct walk *w, const struct cache_schema_field *field, int *valid)
{
    size_t base;
    *valid = 1;
    if (field->target_level == CACHE_SCHEMA_ROOT)
        base = w->frames[0];
    else if (field->target_level == CACHE_SCHEMA_STRUCTURE)
        base = w->structure;
    else if (field->target_level >= 0 && field->target_level < w->depth)
        base = w->frames[w->depth - 1 - field->target_level];
    else {
        *valid = 0;
        return 0;
    }
    return base + (size_t)field->target_offset;
}

static void value(struct walk *w, size_t address, const struct cache_schema_field *field, uint32_t id)
{
    const unsigned char *p = w->in->tags + address;
    int is_unsigned = (field->flags & CACHE_SCHEMA_UNSIGNED_BIT) != 0;
    int none_allowed = (field->flags & CACHE_SCHEMA_NONE_BIT) != 0;
    uint32_t ordinal = 0;
    switch (field->type) {
    case CACHE_SCHEMA_REFERENCE: {
        size_t name = 0;
        uint32_t group = le32(p), index = le32(p + 12);
        int valid_name = string_valid(w, address + 4, &name);
        ++w->report->references;
        if (valid_name) {
            ++w->report->names_valid;
            mark(w, address + 4, 0);
        } else {
            ++w->report->names_invalid;
            if (index != NONE_WORD)
                ++w->report->corrections;
            mark(w, address + 4, 1);
        }
        int resolved = tag_index(w, index, &group, field->link, &ordinal);
        if (resolved == 1)
            ++w->report->references_resolved;
        else if (resolved == 0)
            ++w->report->references_none;
        else if (resolved == -1)
            ++w->report->corrections;
        emit(w, EVENT_REFERENCE, id, group, index, valid_name ? (uint32_t)name : NONE_WORD);
        break;
    }
    case CACHE_SCHEMA_TAG_INDEX: {
        uint32_t index = le32(p);
        int resolved = tag_index(w, index, NULL, field->link, &ordinal);
        ++w->report->tag_indices;
        if (resolved == 1)
            ++w->report->tag_indices_resolved;
        else if (resolved == 0)
            ++w->report->tag_indices_none;
        else if (resolved == -1)
            ++w->report->corrections;
        emit(w, EVENT_TAG_INDEX, id, index, 0, 0);
        break;
    }
    case CACHE_SCHEMA_BLOCK_INDEX: {
        int target_valid;
        size_t target = block_index_target(w, field, &target_valid);
        long number = integer_get(p, field->size, is_unsigned);
        long none = integer_none(field->size, is_unsigned);
        long count = target_valid && target + 4 <= w->in->cache_bytes ? (long)s32(le32(w->in->tags + target)) : 0;
        ++w->report->block_indices;
        if (!((number >= 0 && number < count && number != none) || ((none_allowed || !count) && number == none)))
            ++w->report->corrections;
        emit(w, EVENT_BLOCK_INDEX, id, (uint32_t)number, (uint32_t)count, 0);
        break;
    }
    case CACHE_SCHEMA_ENUM: {
        long number = integer_get(p, field->size, is_unsigned);
        long none = integer_none(field->size, is_unsigned);
        ++w->report->enums;
        if (!((number >= 0 && number < field->maximum && number != none) || (none_allowed && number == none)))
            ++w->report->corrections;
        emit(w, EVENT_ENUM, id, (uint32_t)number, 0, 0);
        break;
    }
    case CACHE_SCHEMA_STRING: {
        int terminated = memchr(p, 0, (size_t)field->size) != NULL;
        ++w->report->strings;
        if (!terminated)
            ++w->report->corrections;
        emit(w, EVENT_STRING, id, (uint32_t)terminated, 0, 0);
        break;
    }
    case CACHE_SCHEMA_RESET:
        ++w->report->resets;
        break;
    default:
        fail(w, CACHE_GRAPH_SCHEMA, address);
        break;
    }
}

/* The pointer fields a schema check or the loader reads (cache_schema_pointers): a target in the tag
 * slot is relocated; none or one outside it (which the checks null) is nulled. */
static void supplementary(struct walk *w, size_t base, int32_t definition)
{
    for (int32_t index = 0; index < cache_schema_counts.pointers; ++index) {
        size_t field, target = 0;
        if (cache_schema_pointers[index].definition != definition)
            continue;
        field = base + (size_t)cache_schema_pointers[index].offset;
        ++w->report->supplementary_fields;
        if (pointer_offset(w, field, &target) && target < w->in->cache_bytes) {
            mark(w, field, 0);
            emit(w, EVENT_SUPPLEMENTARY, (uint32_t)index, (uint32_t)target, 0, 0);
        } else {
            ++w->report->supplementary_none;
            mark(w, field, 1);
            emit(w, EVENT_SUPPLEMENTARY, (uint32_t)index, NONE_WORD, 0, 0);
        }
    }
}

static void walk_fields(struct walk *w, size_t base, int32_t definition, int pass)
{
    const struct cache_schema_definition *d = &cache_schema_definitions[definition];
    size_t structure = w->structure;
    w->structure = base;
    if (pass == PASS_VALUES)
        supplementary(w, base, definition);
    for (int32_t f = 0; f < d->field_count && !w->failed; ++f) {
        uint32_t id = (uint32_t)(d->first_field + f);
        const struct cache_schema_field *field = &cache_schema_fields[id];
        for (int16_t index = 0; index < field->count && !w->failed; ++index) {
            size_t address = base + (size_t)field->offset + (size_t)index * (size_t)field->size;
            switch (field->type) {
            case CACHE_SCHEMA_STRUCT:
                if (pass == PASS_EXTENTS)
                    ++w->report->structs;
                walk_fields(w, address, field->link, pass);
                break;
            case CACHE_SCHEMA_BLOCK:
                if (pass == PASS_EXTENTS) {
                    block_extent(w, address, field, id);
                } else {
                    uint32_t count = block_count(w, address, field);
                    size_t element = (size_t)cache_schema_definitions[field->link].size, target = 0;
                    if (count && !pointer_offset(w, address + 4, &target)) {
                        fail(w, CACHE_GRAPH_EXTENT, address);
                        break;
                    }
                    for (uint32_t e = 0; e < count && !w->failed; ++e)
                        walk_element(w, target + e * element, field->link);
                }
                break;
            case CACHE_SCHEMA_DATA:
            case CACHE_SCHEMA_FILE_DATA:
                if (pass == PASS_EXTENTS)
                    data_extent(w, address, field, id);
                break;
            case CACHE_SCHEMA_CHECK:
                if (pass == PASS_VALUES)
                    ++w->report->checks;
                break;
            default:
                if (pass == PASS_VALUES)
                    value(w, address, field, id);
                break;
            }
        }
    }
    w->structure = structure;
}

static void walk_root(struct walk *w, size_t root, int32_t definition)
{
    w->depth = 0;
    w->structure = root;
    ++w->report->roots_walked;
    emit(w, EVENT_TAG, (uint32_t)w->tag, (uint32_t)root, (uint32_t)definition, 0);
    walk_element(w, root, definition);
}

/* Buffer descriptor tables: claimed in the region; with data_pointers, each Data names a region byte. */
static int buffers(struct walk *w, size_t count_field, size_t pointer_field, int data_pointers, uint32_t kind)
{
    int32_t count = s32(le32(w->in->tags + count_field));
    size_t table = 0;
    if (count < 0 || (uint32_t)count > w->region_bytes / BUFFER_BYTES)
        return fail(w, CACHE_GRAPH_HEADER, count_field);
    if (count) {
        if (!pointer_offset(w, pointer_field, &table) || !region_contains(w, table, (size_t)count * BUFFER_BYTES))
            return fail(w, CACHE_GRAPH_HEADER, pointer_field);
        if (!claim(w, table, (size_t)count * BUFFER_BYTES))
            return fail(w, CACHE_GRAPH_OVERLAP, pointer_field);
        mark(w, pointer_field, 0);
    }
    for (int32_t index = 0; index < count; ++index) {
        size_t buffer = table + (size_t)index * BUFFER_BYTES, data = 0;
        uint32_t word = le32(w->in->tags + buffer + 4);
        ++w->report->buffers;
        if (data_pointers) {
            if (!pointer_offset(w, buffer + 4, &data) || !region_contains(w, data, 1))
                return fail(w, CACHE_GRAPH_HEADER, buffer + 4);
            mark(w, buffer + 4, 0);
            word = (uint32_t)data;
        }
        emit(w, EVENT_BUFFER, kind, (uint32_t)index, word, 0);
    }
    return !w->failed;
}

static int walk_tags(struct walk *w)
{
    const unsigned char *tags = w->in->tags;
    w->region = 0;
    w->region_bytes = w->in->tag_bytes;
    w->tag = -1;
    if (w->in->tag_bytes < HEADER_BYTES || w->in->tag_bytes > w->in->cache_bytes ||
        le32(tags + 32) != TAGS_SIGNATURE)
        return fail(w, CACHE_GRAPH_HEADER, 32);
    int32_t count = s32(le32(tags + 12));
    if (count <= 0 || (uint32_t)count > CACHE_GRAPH_MAX_TAGS ||
        !pointer_offset(w, 0, &w->instances) || !region_contains(w, w->instances, (size_t)count * INSTANCE_BYTES))
        return fail(w, CACHE_GRAPH_HEADER, 0);
    w->tag_count = (uint32_t)count;
    w->report->tag_count = w->tag_count;
    if (!claim(w, 0, HEADER_BYTES) || !claim(w, w->instances, (size_t)count * INSTANCE_BYTES))
        return fail(w, CACHE_GRAPH_HEADER, 0);
    mark(w, 0, 0);
    /* (each buffer's Data is its bytes' address in the tags before it is registered) */
    if (!buffers(w, 16, 20, 1, 0) || !buffers(w, 24, 28, 1, 1))
        return 0;

    /* the tag table: ordinal, name, groups, root */
    for (uint32_t ordinal = 0; ordinal < w->tag_count && !w->failed; ++ordinal) {
        size_t instance = instance_at(w, ordinal), name = 0, root = 0;
        const unsigned char *p = tags + instance;
        uint32_t group = le32(p);
        const struct cache_schema_group *schema = schema_group(group);
        w->tag = (long)ordinal;
        if ((le32(p + 12) & 0xffffu) != ordinal)
            return fail(w, CACHE_GRAPH_TABLE, instance + 12);
        if (string_valid(w, instance + 16, &name)) {
            mark(w, instance + 16, 0);
        } else {
            ++w->report->corrections;
            mark(w, instance + 16, 1);
        }
        if (schema && (le32(p + 4) != schema->parent_group_tags[0] || le32(p + 8) != schema->parent_group_tags[1]))
            ++w->report->corrections;
        if (group == SBSP_GROUP) {
            mark(w, instance + 20, 1);  /* (a structure bsp's root is where it loads) */
            emit(w, EVENT_INSTANCE, ordinal, group, NONE_WORD, 0);
            continue;
        }
        if (!pointer_offset(w, instance + 20, &root) || !region_contains(w, root, 1))
            return fail(w, CACHE_GRAPH_ROOT, instance + 20);
        mark(w, instance + 20, 0);
        if (schema && schema->definition >= 0) {
            size_t bytes = (size_t)cache_schema_definitions[schema->definition].size;
            if (!region_contains(w, root, bytes))
                return fail(w, CACHE_GRAPH_ROOT, instance + 20);
            if (!claim(w, root, bytes))
                return fail(w, CACHE_GRAPH_OVERLAP, instance + 20);
        }
        emit(w, EVENT_INSTANCE, ordinal, group, (uint32_t)root, (uint32_t)name);
    }
    /* every tag through its schema */
    for (uint32_t ordinal = 0; ordinal < w->tag_count && !w->failed; ++ordinal) {
        const unsigned char *p = tags + instance_at(w, ordinal);
        const struct cache_schema_group *schema = schema_group(le32(p));
        size_t root = 0;
        w->tag = (long)ordinal;
        if (le32(p) == SBSP_GROUP)
            continue;
        if (!schema || schema->definition < 0) {
            ++w->report->roots_without_schema;
            continue;
        }
        pointer_offset(w, instance_at(w, ordinal) + 20, &root);
        walk_root(w, root, schema->definition);
    }
    return !w->failed;
}

static int walk_bsp(struct walk *w)
{
    const struct cache_graph_input *in = w->in;
    size_t at = in->bsp_offset, root = 0;
    int16_t ordinal = (int16_t)(uint16_t)((uint32_t)in->bsp_tag_index & 0xffffu);
    w->tag = -1;
    if (ordinal < 0 || (uint32_t)ordinal >= w->tag_count ||
        le32(in->tags + instance_at(w, (uint32_t)ordinal) + 12) != (uint32_t)in->bsp_tag_index ||
        le32(in->tags + instance_at(w, (uint32_t)ordinal)) != SBSP_GROUP || in->bsp_bytes < BSP_HEADER_BYTES ||
        at < in->tag_bytes || at > in->cache_bytes || in->bsp_bytes > in->cache_bytes - at)
        return fail(w, CACHE_GRAPH_BSP, at);
    w->tag = ordinal;
    w->region = at;
    w->region_bytes = in->bsp_bytes;
    if (le32(in->tags + at + 20) != SBSP_GROUP || !claim(w, at, BSP_HEADER_BYTES))
        return fail(w, CACHE_GRAPH_BSP, at + 20);
    if (!buffers(w, at + 4, at + 8, 1, 2) || !buffers(w, at + 12, at + 16, 1, 3))
        return 0;
    const struct cache_schema_group *schema = schema_group(SBSP_GROUP);
    if (!schema || schema->definition < 0)
        return fail(w, CACHE_GRAPH_SCHEMA, at);
    size_t bytes = (size_t)cache_schema_definitions[schema->definition].size;
    if (!pointer_offset(w, at, &root) || !region_contains(w, root, bytes))
        return fail(w, CACHE_GRAPH_BSP, at);
    if (!claim(w, root, bytes))
        return fail(w, CACHE_GRAPH_OVERLAP, at);
    mark(w, at, 0);
    emit(w, EVENT_BSP, (uint32_t)ordinal, (uint32_t)at, (uint32_t)in->bsp_bytes, (uint32_t)root);
    walk_root(w, root, schema->definition);
    w->report->bsp_walked = !w->failed;
    return !w->failed;
}

static uint32_t group_slot(struct walk *w, uint32_t group)
{
    struct cache_graph_report *r = w->report;
    for (uint32_t index = 0; index < r->group_count; ++index)
        if (r->groups[index].group_tag == group)
            return index;
    if (r->group_count >= CACHE_GRAPH_MAX_GROUPS) {
        fail(w, CACHE_GRAPH_SCHEMA, 0);
        return 0;
    }
    r->groups[r->group_count].group_tag = group;
    return r->group_count++;
}

static void summarize(struct walk *w)
{
    struct cache_graph_report *r = w->report;
    const unsigned char *tags = w->in->tags;
    /* reachability from the scenario over reference/tag-index edges (edge order is the walk's) */
    uint32_t scenario = (uint32_t)(le32(tags + 4) & 0xffffu);
    if (scenario < w->tag_count && le32(tags + instance_at(w, scenario)) == SCENARIO_GROUP) {
        size_t head = 0, tail = 0;
        r->scenario_ordinal = scenario;
        memset(w->reached, 0, w->tag_count);
        w->reached[scenario] = 1;
        w->queue[tail++] = (uint16_t)scenario;
        while (head < tail) {
            uint16_t from = w->queue[head++];
            for (uint32_t e = 0; e < r->edges; ++e)
                if (w->edges[e * 2] == from && !w->reached[w->edges[e * 2 + 1]]) {
                    w->reached[w->edges[e * 2 + 1]] = 1;
                    w->queue[tail++] = w->edges[e * 2 + 1];
                }
        }
        r->reachable = (uint32_t)tail;
    } else {
        r->scenario_ordinal = NONE_WORD;
        fail(w, CACHE_GRAPH_HEADER, 4);
        return;
    }
    for (uint32_t ordinal = 0; ordinal < w->tag_count && !w->failed; ++ordinal) {
        uint32_t group = le32(tags + instance_at(w, ordinal));
        const struct cache_schema_group *schema = schema_group(group);
        uint32_t slot = group_slot(w, group);
        ++r->groups[slot].tags;
        if (group != SBSP_GROUP && schema && schema->definition >= 0)
            ++r->groups[slot].walked;
        r->groups[slot].reachable += w->reached[ordinal];
    }
    w->crc = 0;
    for (uint32_t e = 0; e < r->edges && !w->failed; ++e) {
        uint32_t from = le32(tags + instance_at(w, w->edges[e * 2]));
        uint32_t to = le32(tags + instance_at(w, w->edges[e * 2 + 1]));
        uint32_t index = 0;
        const uint32_t pair[2] = {w->edges[e * 2], w->edges[e * 2 + 1]};
        w->crc = crc_words(w->crc, pair, 2);
        while (index < r->pair_count && !(r->pairs[index].from_group == from && r->pairs[index].to_group == to))
            ++index;
        if (index == r->pair_count) {
            if (r->pair_count >= CACHE_GRAPH_MAX_PAIRS) {
                fail(w, CACHE_GRAPH_EDGES, 0);
                break;
            }
            r->pairs[r->pair_count].from_group = from;
            r->pairs[r->pair_count].to_group = to;
            ++r->pair_count;
        }
        ++r->pairs[index].edges;
    }
    r->edge_digest = w->crc;
}

/* Address-like words in the walked regions that no pointer field named. */
static void residual_audit(struct walk *w)
{
    const struct cache_graph_input *in = w->in;
    struct cache_graph_report *r = w->report;
    uint32_t low = in->encoded_base, high = in->encoded_base + (uint32_t)in->cache_bytes;
    for (int part = 0; part < 2; ++part) {
        size_t first = part ? in->bsp_offset : 0, end = part ? in->bsp_offset + in->bsp_bytes : in->tag_bytes;
        if (part && !in->bsp_bytes)
            break;
        for (size_t at = (first + 3) & ~(size_t)3; at + 4 <= end; at += 4) {
            uint32_t word = le32(in->tags + at);
            if (word < low || word >= high || marked(w->relocate, at) || marked(w->nulls, at))
                continue;
            ++r->residual_address_words;
            if (!claimed(w, at))
                ++r->residual_unclaimed;
            else if (word % 4)
                ++r->residual_claimed_unaligned;
            else
                ++r->residual_claimed_aligned;
        }
    }
}

static int setup(struct walk *w, const struct cache_graph_input *in, void *workspace, size_t workspace_bytes,
                 struct cache_graph_report *report, struct cache_graph_result *result)
{
    if (!result)
        return 0;
    result->error = CACHE_GRAPH_OK;
    result->offset = 0;
    result->tag_ordinal = -1;
    memset(w, 0, sizeof(*w));
    w->result = result;
    w->tag = -1;
    if (!in || !in->tags || !report || !workspace || in->cache_bytes % 32 || in->cache_bytes > 0x40000000u ||
        in->encoded_base > UINT32_MAX - in->cache_bytes)
        return fail(w, CACHE_GRAPH_ARGUMENT, 0);
    if (in->native && ((uintptr_t)in->tags > UINT32_MAX - in->cache_bytes))
        return fail(w, CACHE_GRAPH_NATIVE, 0);
    if ((uintptr_t)workspace % 8 || workspace_bytes < cache_graph_workspace_bytes(in->cache_bytes))
        return fail(w, CACHE_GRAPH_WORKSPACE, 0);
    crc_init();
    unsigned char *bytes = workspace;
    memset(workspace, 0, cache_graph_workspace_bytes(in->cache_bytes));
    memset(report, 0, sizeof(*report));
    w->in = in;
    w->report = report;
    w->claims = (uint32_t *)(void *)bytes;
    bytes += claim_bytes(in->cache_bytes);
    w->relocate = (uint32_t *)(void *)bytes;
    bytes += word_bitmap_bytes(in->cache_bytes);
    w->nulls = (uint32_t *)(void *)bytes;
    bytes += word_bitmap_bytes(in->cache_bytes);
    w->edges = (uint16_t *)(void *)bytes;
    bytes += CACHE_GRAPH_MAX_EDGES * 4u;
    w->queue = (uint16_t *)(void *)bytes;
    bytes += CACHE_GRAPH_MAX_TAGS * 2u;
    w->reached = bytes;
    return 1;
}

static int run(struct walk *w)
{
    if (!walk_tags(w))
        return 0;
    if (w->in->bsp_bytes && !walk_bsp(w))
        return 0;
    w->report->digest = w->crc;
    summarize(w);
    if (w->failed)
        return 0;
    residual_audit(w);
    return 1;
}

int cache_graph_walk(const struct cache_graph_input *in, void *workspace, size_t workspace_bytes,
                     struct cache_graph_report *report, struct cache_graph_result *result)
{
    struct walk w;
    if (!setup(&w, in, workspace, workspace_bytes, report, result))
        return 0;
    return run(&w);
}

int cache_graph_relocate(const struct cache_graph_input *in, unsigned char *tags, void *workspace,
                         size_t workspace_bytes, struct cache_graph_report *report, struct cache_graph_result *result)
{
    struct walk w;
    if (!setup(&w, in, workspace, workspace_bytes, report, result))
        return 0;
    if (in->native || tags != in->tags)
        return fail(&w, CACHE_GRAPH_ARGUMENT, 0);
    if ((uintptr_t)tags > UINT32_MAX - in->cache_bytes)
        return fail(&w, CACHE_GRAPH_NATIVE, 0);
    if (!run(&w))
        return 0;
    size_t words = in->cache_bytes / 4;
    for (size_t index = 0; index < words; ++index) {
        uint32_t bits = w.relocate[index / 32] | w.nulls[index / 32];
        if (!bits) {
            index |= 31;
            continue;
        }
        size_t at = index * 4;
        uint32_t native = 0;
        if (marked(w.relocate, at))
            native = (uint32_t)(uintptr_t)(tags + (le32(tags + at) - in->encoded_base));
        else if (!marked(w.nulls, at))
            continue;
        memcpy(tags + at, &native, 4);
    }
    return 1;
}

static size_t append(char *text, size_t capacity, size_t used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static size_t append(char *text, size_t capacity, size_t used, const char *format, ...)
{
    va_list args;
    if (used >= capacity)
        return used;
    va_start(args, format);
    int written = vsnprintf(text + used, capacity - used, format, args);
    va_end(args);
    if (written < 0)
        return used;
    return used + (size_t)written < capacity ? used + (size_t)written : capacity - 1;
}

static void tag_text(uint32_t tag, char out[5])
{
    for (int index = 0; index < 4; ++index) {
        unsigned char c = (unsigned char)(tag >> (24 - index * 8));
        out[index] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[4] = 0;
}

size_t cache_graph_describe(const struct cache_graph_report *r, char *text, size_t capacity)
{
    size_t used = 0;
    if (!r || !text || !capacity)
        return 0;
    text[0] = 0;
    used = append(text, capacity, used,
        "GRAPH tags=%u scenario=%u roots=%u roots_without_schema=%u bsp=%u blocks=%u nonempty=%u elements=%u "
        "structs=%u data=%u/%u file_data=%u/%u\n",
        r->tag_count, r->scenario_ordinal, r->roots_walked, r->roots_without_schema, r->bsp_walked, r->blocks,
        r->nonempty_blocks, r->elements, r->structs, r->data_fields, r->data_bytes, r->file_data_fields,
        r->file_data_bytes);
    used = append(text, capacity, used,
        "GRAPH references=%u none=%u resolved=%u names=%u/%u tag_indices=%u none=%u resolved=%u block_indices=%u "
        "enums=%u strings=%u resets=%u checks=%u buffers=%u corrections=%u\n",
        r->references, r->references_none, r->references_resolved, r->names_valid, r->names_invalid,
        r->tag_indices, r->tag_indices_none, r->tag_indices_resolved, r->block_indices, r->enums, r->strings,
        r->resets, r->checks, r->buffers, r->corrections);
    used = append(text, capacity, used,
        "GRAPH supplementary=%u supplementary_none=%u claimed=%u pointer_fields=%u null_fields=%u depth=%u edges=%u "
        "reachable=%u residual=%u residual_claimed_aligned=%u residual_claimed_unaligned=%u residual_unclaimed=%u "
        "digest=%08x edge_digest=%08x\n",
        r->supplementary_fields, r->supplementary_none, r->claimed_bytes, r->pointer_fields, r->null_fields,
        r->max_depth, r->edges, r->reachable,
        r->residual_address_words, r->residual_claimed_aligned, r->residual_claimed_unaligned,
        r->residual_unclaimed, r->digest, r->edge_digest);
    for (uint32_t index = 0; index < r->group_count; ++index) {
        char group[5];
        tag_text(r->groups[index].group_tag, group);
        used = append(text, capacity, used, "GROUP %s tags=%u walked=%u reachable=%u\n", group,
                      r->groups[index].tags, r->groups[index].walked, r->groups[index].reachable);
    }
    for (uint32_t index = 0; index < r->pair_count; ++index) {
        char from[5], to[5];
        tag_text(r->pairs[index].from_group, from);
        tag_text(r->pairs[index].to_group, to);
        used = append(text, capacity, used, "EDGE %s>%s %u\n", from, to, r->pairs[index].edges);
    }
    return used;
}
