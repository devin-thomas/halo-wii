/* HWI-007 game-state ownership prototype; see game_state_image.h. Authored port
 * code around the actual engine units source/memory/data.c and memory_pool.c. */
#include "cseries.h"
#include "data.h"
#include "memory_pool.h"
#include "game_state_image.h"

#define IMAGE_MAGIC UINT32_C(0x53475748) /* "HWGS" little-endian */
#define IMAGE_VERSION 1u
#define NO_BLOCK UINT32_C(0xffffffff)
#define DATA_SIGNATURE 0x64407440ul /* 'd@t@' */
#define OBJECT_POOL_FIELD 8u        /* object_header_datum.datum */
#define MAXIMUM_OBJECTS 8192u

unsigned long gs_engine_assert_count;
long gs_engine_assert_line;
char temporary[256];

void gs_engine_assert(long line)
{
    ++gs_engine_assert_count;
    gs_engine_assert_line = line;
}

void platform_log(const char *format, ...);
void platform_log(const char *format, ...)
{
    (void)format;
}

/* source/objects/objects.h's object header (12 bytes; its datum field is the pool reference) */
struct gs_object_header {
    short identifier;
    byte flags;
    byte type;
    short cluster_index;
    short data_size;
    void *datum;
};
_Static_assert(sizeof(struct gs_object_header) == 12, "object header layout");

enum { OBJECT_BIPED, OBJECT_VEHICLE, OBJECT_WEAPON, OBJECT_SCENERY };
static const long object_bytes[] = {1208, 1440, 692, 380}; /* representative payloads (authored) */

static int fail(struct gs_result *result, enum gs_error error, size_t offset)
{
    result->error = error;
    result->offset = offset;
    return 0;
}

const char *gs_error_name(enum gs_error error)
{
    static const char *const names[] = {"ok", "argument", "capacity", "engine", "layout", "full", "io",
        "magic", "checksum", "truncated", "mismatch", "counts", "owner", "state"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

/* ---------- CRC-32 */

static uint32_t crc_table[256];

static uint32_t crc_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
    if (!crc_table[1])
        for (uint32_t index = 0; index < 256; ++index) {
            uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (value & 1u)));
            crc_table[index] = value;
        }
    crc = ~crc;
    while (length--)
        crc = (crc >> 8) ^ crc_table[(crc ^ *bytes++) & 0xffu];
    return ~crc;
}

/* ---------- layout */

void gs_identifier_seeds(const char *name, uint16_t *identifier_seed, uint16_t *native_seed)
{
    struct data_array probe;
    short native = 0;
    memset(&probe, 0, sizeof(probe));
    strncpy(probe.name, name, sizeof(probe.name) - 1);
    probe.maximum_count = 1;
    probe.size = 4;
    probe.signature = DATA_SIGNATURE;
    probe.data = &native; /* (data_delete_all clears element 0's identifier: this short) */
    probe.valid = TRUE;
    data_delete_all(&probe);
    *identifier_seed = (uint16_t)probe.next_identifier;
    /* the unported copy (csstrncpy of two characters into the short), read in this target's order */
    const char copied[2] = {probe.name[0], probe.name[0] ? probe.name[1] : 0};
    memcpy(&native, copied, sizeof(native));
    native |= (short)0x8000;
    *native_seed = (uint16_t)native;
}

static int find(const struct gs_region *region, const char *name)
{
    for (unsigned index = 0; index < region->count; ++index)
        if (!strcmp(region->allocations[index].name, name))
            return (int)index;
    return -1;
}

static void *address(const struct gs_region *region, int allocation)
{
    return region->base + region->allocations[allocation].offset;
}

static struct data_array *array(const struct gs_region *region, int allocation)
{
    return (struct data_array *)address(region, allocation);
}

static void put_le32(unsigned char *bytes, uint32_t value)
{
    for (int index = 0; index < 4; ++index)
        bytes[index] = (unsigned char)(value >> (8 * index));
}

int gs_build(struct gs_region *region, void *base, size_t size, size_t cpu_size, size_t gpu_size,
             struct gs_result *result)
{
    if (!result)
        return 0;
    result->error = GS_OK;
    result->offset = 0;
    if (!region || !base || (uintptr_t)base % 32 || cpu_size % 4 || gpu_size % 4 || cpu_size > size ||
        gpu_size > size - cpu_size)
        return fail(result, GS_ARGUMENT, 0);
    memset(region, 0, sizeof(*region));
    region->base = base;
    region->size = size;
    region->cpu_size = cpu_size;
    region->gpu_size = gpu_size;
    memset(base, 0, cpu_size + gpu_size);
    unsigned long engine_asserts = gs_engine_assert_count;
    for (unsigned row = 0; row < gs_census_row_count; ++row)
        for (long copy = 0; copy < gs_census_rows[row].multiplicity; ++copy) {
            const struct gs_census_row *census = &gs_census_rows[row];
            size_t bytes = (size_t)census->bytes_each, offset;
            unsigned char size_bytes[4];
            if (region->count >= GS_MAX_ALLOCATIONS)
                return fail(result, GS_FULL, row);
            if (bytes % 4)
                return fail(result, GS_LAYOUT, row);
            if (census->kind == GS_GPU) {
                if (bytes > gpu_size - region->gpu_used)
                    return fail(result, GS_CAPACITY, row);
                region->gpu_used += bytes;
                offset = cpu_size + gpu_size - region->gpu_used;
            } else {
                if (bytes > cpu_size - region->cpu_used)
                    return fail(result, GS_CAPACITY, row);
                offset = region->cpu_used;
                region->cpu_used += bytes;
            }
            struct gs_allocation *allocation = &region->allocations[region->count++];
            allocation->kind = census->kind;
            allocation->offset = offset;
            allocation->size = bytes;
            allocation->count = census->count;
            allocation->element_size = census->element_size;
            allocation->name = census->name;
            put_le32(size_bytes, (uint32_t)bytes);
            region->allocation_checksum = crc_update(region->allocation_checksum, size_bytes, 4);
            if (census->kind == GS_DATA) {
                struct data_array *data = (struct data_array *)(region->base + offset);
                if ((size_t)data_allocation_size((short)census->count, (short)census->element_size) != bytes)
                    return fail(result, GS_LAYOUT, row);
                data_initialize(data, census->name, (short)census->count, (short)census->element_size);
                data_make_valid(data);
            } else if (census->kind == GS_POOL) {
                if ((size_t)memory_pool_allocation_size(census->element_size) != bytes)
                    return fail(result, GS_LAYOUT, row);
                memory_pool_initialize((struct memory_pool *)(region->base + offset), census->name,
                                       census->element_size);
            }
        }
    if (gs_engine_assert_count != engine_asserts)
        return fail(result, GS_ENGINE, 0);
    region->players = find(region, "players");
    region->objects = find(region, "object");
    region->pool = find(region, "objects (memory pool)");
    region->actors = find(region, "actor");
    region->props = find(region, "prop");
    if (region->players < 0 || region->objects < 0 || region->pool < 0 || region->actors < 0 ||
        region->props < 0 || region->allocations[region->objects].element_size != 12 ||
        region->allocations[region->objects].count != MAXIMUM_OBJECTS)
        return fail(result, GS_LAYOUT, 0);
    region->live = 1;
    return 1;
}

/* ---------- representative state through the engine */

static void store32(void *element, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)element + offset, &value, 4);
}

static uint32_t load32(const void *element, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)element + offset, 4);
    return value;
}

static long new_object(struct gs_region *region, int type, uint32_t salt)
{
    struct data_array *objects = array(region, region->objects);
    struct memory_pool *pool = (struct memory_pool *)address(region, region->pool);
    long index = datum_new(objects);
    if (index == NONE)
        return NONE;
    struct gs_object_header *header = datum_get(objects, index);
    header->type = (byte)type;
    header->flags = 1;
    header->cluster_index = (short)(index & 7);
    header->data_size = (short)object_bytes[type];
    if (!memory_pool_block_allocate(pool, &header->datum, object_bytes[type])) {
        datum_delete(objects, index);
        return NONE;
    }
    for (long word = 0; word < object_bytes[type] / 4; ++word)
        store32(header->datum, (size_t)word * 4, (uint32_t)(word * 2654435761u) ^ salt);
    store32(header->datum, 0, (uint32_t)type);
    store32(header->datum, 4, (uint32_t)index);
    return index;
}

static void *object_data(struct gs_region *region, long index)
{
    struct gs_object_header *header = datum_try_and_get(array(region, region->objects), index);
    return header ? header->datum : NULL;
}

static void delete_object(struct gs_region *region, long index)
{
    struct data_array *objects = array(region, region->objects);
    struct gs_object_header *header = datum_get(objects, index);
    memory_pool_block_free((struct memory_pool *)address(region, region->pool), &header->datum);
    header->datum = NULL;
    datum_delete(objects, index);
}

static int create(struct gs_region *region, struct gs_result *result)
{
    struct data_array *players = array(region, region->players), *actors = array(region, region->actors);
    struct data_array *props = array(region, region->props);
    long bipeds[20];
    for (int player = 0; player < 4; ++player) {
        long index = datum_new(players);
        long biped = new_object(region, OBJECT_BIPED, 0x1000u + (uint32_t)player);
        if (index == NONE || biped == NONE)
            return fail(result, GS_FULL, (size_t)player);
        void *element = datum_get(players, index);
        ((short *)element)[1] = (short)player;   /* local player index (u16 at 2) */
        store32(element, 28, (uint32_t)player);    /* squad */
        store32(element, 32, (uint32_t)(player & 1)); /* team */
        store32(element, 36, (uint32_t)biped);     /* unit */
        store32(element, 40, 0);                   /* score */
        bipeds[player] = biped;
    }
    for (int ai = 0; ai < 16; ++ai) {
        long biped = new_object(region, OBJECT_BIPED, 0x2000u + (uint32_t)ai);
        long actor = datum_new(actors);
        if (biped == NONE || actor == NONE)
            return fail(result, GS_FULL, 100u + (unsigned)ai);
        void *element = datum_get(actors, actor);
        store32(element, 4, (uint32_t)biped);
        store32(element, 8, (uint32_t)(ai / 4)); /* encounter */
        store32(element, 12, 1);                 /* state */
        store32(element, 16, (uint32_t)bipeds[ai % 4]);
        if (ai < 16)
            bipeds[4 + ai] = biped;
    }
    for (int owner = 0; owner < 20; ++owner)
        for (int slot = 0; slot < (owner < 4 ? 3 : 1); ++slot) {
            long weapon = new_object(region, OBJECT_WEAPON, 0x3000u + (uint32_t)(owner * 4 + slot));
            if (weapon == NONE)
                return fail(result, GS_FULL, 200u + (unsigned)owner);
            store32(object_data(region, weapon), 32, (uint32_t)bipeds[owner]); /* carrier */
            store32(object_data(region, bipeds[owner]), 32 + 4 * (size_t)slot, (uint32_t)weapon);
        }
    for (int vehicle = 0; vehicle < 2; ++vehicle) {
        long index = new_object(region, OBJECT_VEHICLE, 0x4000u + (uint32_t)vehicle);
        if (index == NONE)
            return fail(result, GS_FULL, 300u + (unsigned)vehicle);
        store32(object_data(region, index), 32, (uint32_t)bipeds[vehicle]); /* driver */
    }
    for (int scenery = 0; scenery < 40; ++scenery)
        if (new_object(region, OBJECT_SCENERY, 0x5000u + (uint32_t)scenery) == NONE)
            return fail(result, GS_FULL, 400u + (unsigned)scenery);
    for (int prop = 0; prop < 64; ++prop) {
        long index = datum_new(props);
        if (index == NONE)
            return fail(result, GS_FULL, 500u + (unsigned)prop);
        void *element = datum_get(props, index);
        store32(element, 4, (uint32_t)(prop % 16)); /* actor ordinal */
        store32(element, 8, (uint32_t)bipeds[prop % 20]);
        store32(element, 12, (uint32_t)prop);
    }
    return 1;
}

static int update(struct gs_region *region, struct gs_result *result)
{
    struct data_array *objects = array(region, region->objects), *players = array(region, region->players);
    struct data_array *actors = array(region, region->actors);
    struct memory_pool *pool = (struct memory_pool *)address(region, region->pool);
    struct data_iterator iterator;
    long deleted[GS_MAX_STALE];
    unsigned count = 0;
    /* drop every third weapon (by absolute index and step) and remember its datum */
    data_iterator_new(&iterator, objects);
    for (struct gs_object_header *header; (header = data_iterator_next(&iterator)) != NULL;)
        if (header->type == OBJECT_WEAPON && (iterator.datum_index & 0xffff) % 3 == region->step % 3 &&
            count < GS_MAX_STALE)
            deleted[count++] = iterator.datum_index;
    for (unsigned index = 0; index < count; ++index) {
        delete_object(region, deleted[index]);
        region->stale[index] = (uint32_t)deleted[index];
    }
    region->stale_count = count;
    /* as many new weapons: datum_new reuses the freed indices with new identifiers */
    for (unsigned index = 0; index < count; ++index)
        if (new_object(region, OBJECT_WEAPON, 0x6000u + region->step * 64u + index) == NONE)
            return fail(result, GS_FULL, index);
    /* grow the players' bipeds (their blocks move to the pool's end), then compact */
    data_iterator_new(&iterator, players);
    for (void *player; (player = data_iterator_next(&iterator)) != NULL;) {
        long biped = (long)load32(player, 36);
        struct gs_object_header *header = datum_try_and_get(objects, biped);
        if (!header || !memory_pool_block_reallocate(pool, &header->datum, header->data_size + 64))
            return fail(result, GS_STATE, (size_t)biped);
        header->data_size = (short)(header->data_size + 64);
        store32(header->datum, (size_t)header->data_size - 4, region->step);
        store32(player, 40, load32(player, 40) + region->step + 1);
    }
    memory_pool_compact(pool);
    data_iterator_new(&iterator, actors);
    for (void *actor; (actor = data_iterator_next(&iterator)) != NULL;)
        store32(actor, 12, (load32(actor, 12) + 1) % 5);
    return 1;
}

int gs_update(struct gs_region *region, struct gs_result *result)
{
    if (!result)
        return 0;
    result->error = GS_OK;
    result->offset = 0;
    if (!region || !region->live)
        return fail(result, GS_ARGUMENT, 0);
    unsigned long engine_asserts = gs_engine_assert_count;
    int ok = region->step ? update(region, result) : create(region, result);
    if (gs_engine_assert_count != engine_asserts)
        return fail(result, GS_ENGINE, (size_t)gs_engine_assert_line);
    if (!ok)
        return 0;
    ++region->step;
    return 1;
}

/* ---------- canonical image */

struct sink {
    FILE *file;
    uint32_t crc;
    size_t bytes;
    int failed;
};

static void put(struct sink *sink, const void *bytes, size_t length)
{
    if (sink->failed)
        return;
    if (sink->file && fwrite(bytes, 1, length, sink->file) != length)
        sink->failed = 1;
    sink->crc = crc_update(sink->crc, bytes, length);
    sink->bytes += length;
}

static void put8(struct sink *sink, uint32_t value)
{
    unsigned char byte_value = (unsigned char)value;
    put(sink, &byte_value, 1);
}

static void put16(struct sink *sink, uint32_t value)
{
    unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
    put(sink, bytes, 2);
}

static void put32(struct sink *sink, uint32_t value)
{
    unsigned char bytes[4];
    put_le32(bytes, value);
    put(sink, bytes, 4);
}

/* the ordinal of the pool block a reference names, in block order */
static uint32_t block_ordinal(const struct memory_pool *pool, const void *payload)
{
    uint32_t ordinal = 0;
    for (const struct memory_pool_block *block = pool->first_block; block; block = block->next_block, ++ordinal)
        if ((const void *)(block + 1) == payload)
            return ordinal;
    return NO_BLOCK;
}

static void put_words(struct sink *sink, const unsigned char *bytes, size_t length)
{
    size_t index = 0;
    for (; index + 4 <= length; index += 4)
        put32(sink, load32(bytes, index));
    for (; index < length; ++index)
        put8(sink, bytes[index]);
}

static void put_element(struct sink *sink, const struct gs_region *region, int allocation,
                        const unsigned char *element)
{
    const struct gs_allocation *a = &region->allocations[allocation];
    uint16_t second;
    memcpy(&second, element + 2, 2);
    if (allocation == region->objects) {
        const struct gs_object_header *header = (const void *)element;
        put8(sink, header->flags);
        put8(sink, header->type);
        put16(sink, (uint16_t)header->cluster_index);
        put16(sink, (uint16_t)header->data_size);
        put32(sink, header->datum ?
              block_ordinal((const struct memory_pool *)address(region, region->pool), header->datum) : NO_BLOCK);
        return;
    }
    put16(sink, second);
    put_words(sink, element + 4, (size_t)a->element_size - 4);
}

int gs_save(const struct gs_region *region, FILE *file, struct gs_report *report, struct gs_result *result)
{
    struct sink sink = {file, 0, 0, 0};
    if (!result)
        return 0;
    result->error = GS_OK;
    result->offset = 0;
    if (!region || !region->live || !report)
        return fail(result, GS_ARGUMENT, 0);
    memset(report, 0, sizeof(*report));
    put32(&sink, IMAGE_MAGIC);
    put32(&sink, IMAGE_VERSION);
    put32(&sink, region->step);
    put32(&sink, region->count);
    put32(&sink, region->allocation_checksum);
    for (unsigned index = 0; index < region->count; ++index) {
        const struct gs_allocation *a = &region->allocations[index];
        const unsigned char *bytes = address(region, (int)index);
        put32(&sink, (uint32_t)a->kind);
        put32(&sink, (uint32_t)a->size);
        if (a->kind == GS_DATA) {
            const struct data_array *data = (const void *)bytes;
            put(&sink, data->name, sizeof(data->name));
            put16(&sink, (uint16_t)data->maximum_count);
            put16(&sink, (uint16_t)data->size);
            put8(&sink, data->valid);
            put8(&sink, data->identifier_zero_invalid);
            put32(&sink, (uint32_t)data->signature);
            put16(&sink, (uint16_t)data->first_free_absolute_index);
            put16(&sink, (uint16_t)data->count);
            put16(&sink, (uint16_t)data->actual_count);
            put16(&sink, (uint16_t)data->next_identifier);
            for (short absolute = 0; absolute < data->count; ++absolute) {
                const unsigned char *element = (const unsigned char *)data->data + (size_t)absolute * (size_t)data->size;
                uint16_t identifier;
                memcpy(&identifier, element, 2);
                put16(&sink, identifier);
                if (identifier)
                    put_element(&sink, region, (int)index, element);
            }
        } else if (a->kind == GS_POOL) {
            const struct memory_pool *pool = (const void *)bytes;
            uint32_t blocks = 0;
            for (const struct memory_pool_block *block = pool->first_block; block; block = block->next_block)
                ++blocks;
            put32(&sink, (uint32_t)pool->size);
            put32(&sink, (uint32_t)pool->free_size);
            put32(&sink, blocks);
            for (const struct memory_pool_block *block = pool->first_block; block; block = block->next_block) {
                const unsigned char *owner = (const unsigned char *)block->reference;
                const struct data_array *objects = array(region, region->objects);
                size_t relative = (size_t)(owner - (const unsigned char *)objects->data);
                put32(&sink, (uint32_t)((const unsigned char *)block - (const unsigned char *)pool->base_address));
                put32(&sink, (uint32_t)block->size);
                put32(&sink, (uint32_t)region->objects);
                put32(&sink, (uint32_t)(relative / 12));
                put32(&sink, (uint32_t)(relative % 12));
                put_words(&sink, (const unsigned char *)(block + 1), (size_t)block->size - sizeof(*block));
            }
            report->blocks = blocks;
            report->pool_free = (uint32_t)pool->free_size;
            report->pool_used = (uint32_t)(pool->size - pool->free_size);
        } else if (a->kind == GS_BYTES) {
            put_words(&sink, bytes, a->size);
        }
        /* (GS_GPU and GS_LRUV are rebuilt empty: see the header) */
    }
    uint32_t crc = sink.crc;
    put32(&sink, crc);
    if (sink.failed)
        return fail(result, GS_IO, sink.bytes);
    report->step = region->step;
    report->players = (uint32_t)array(region, region->players)->actual_count;
    report->objects = (uint32_t)array(region, region->objects)->actual_count;
    report->actors = (uint32_t)array(region, region->actors)->actual_count;
    report->props = (uint32_t)array(region, region->props)->actual_count;
    report->object_identifier_seed = (uint16_t)array(region, region->objects)->next_identifier;
    report->player_identifier_seed = (uint16_t)array(region, region->players)->next_identifier;
    report->image_bytes = (uint32_t)sink.bytes;
    report->image_crc = crc;
    for (unsigned index = 0; index < region->stale_count; ++index) {
        ++report->stale_checked;
        if (!datum_try_and_get(array(region, region->objects), (long)region->stale[index]))
            ++report->stale_rejected;
    }
    return 1;
}

/* ---------- restore */

struct source {
    FILE *file;
    uint32_t crc;
    size_t offset;
    int failed;
};

static int get(struct source *source, void *bytes, size_t length)
{
    if (source->failed || fread(bytes, 1, length, source->file) != length) {
        source->failed = 1;
        memset(bytes, 0, length);
        return 0;
    }
    source->crc = crc_update(source->crc, bytes, length);
    source->offset += length;
    return 1;
}

static uint32_t get8(struct source *source)
{
    unsigned char value;
    get(source, &value, 1);
    return value;
}

static uint32_t get16(struct source *source)
{
    unsigned char bytes[2];
    get(source, bytes, 2);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8;
}

static uint32_t get32(struct source *source)
{
    unsigned char bytes[4];
    get(source, bytes, 4);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void get_words(struct source *source, unsigned char *bytes, size_t length)
{
    size_t index = 0;
    for (; index + 4 <= length; index += 4)
        store32(bytes, index, get32(source));
    for (; index < length; ++index)
        bytes[index] = (unsigned char)get8(source);
}

int gs_restore(struct gs_region *region, FILE *file, struct gs_result *result)
{
    static unsigned char bound[MAXIMUM_OBJECTS];
    struct source source = {file, 0, 0, 0};
    if (!result)
        return 0;
    result->error = GS_OK;
    result->offset = 0;
    if (!region || !region->live || !file || region->step)
        return fail(result, GS_ARGUMENT, 0);
    region->live = 0; /* (until the image is wholly taken) */
    if (get32(&source) != IMAGE_MAGIC || get32(&source) != IMAGE_VERSION)
        return fail(result, source.failed ? GS_TRUNCATED : GS_MAGIC, source.offset);
    uint32_t step = get32(&source);
    if (get32(&source) != region->count || get32(&source) != region->allocation_checksum)
        return fail(result, source.failed ? GS_TRUNCATED : GS_MISMATCH, source.offset);
    memset(bound, 0, sizeof(bound));
    struct data_array *objects = array(region, region->objects);
    for (unsigned index = 0; index < region->count && !source.failed; ++index) {
        const struct gs_allocation *a = &region->allocations[index];
        unsigned char *bytes = address(region, (int)index);
        if (get32(&source) != (uint32_t)a->kind || get32(&source) != (uint32_t)a->size)
            return fail(result, source.failed ? GS_TRUNCATED : GS_MISMATCH, source.offset);
        if (a->kind == GS_DATA) {
            struct data_array *data = (void *)bytes;
            char name[sizeof(data->name)];
            get(&source, name, sizeof(name));
            uint32_t maximum = get16(&source), size = get16(&source);
            uint32_t valid = get8(&source), zero_invalid = get8(&source), signature = get32(&source);
            int32_t first_free = (int16_t)get16(&source), count = (int16_t)get16(&source);
            int32_t actual = (int16_t)get16(&source);
            uint32_t next = get16(&source);
            if (source.failed)
                return fail(result, GS_TRUNCATED, source.offset);
            if (memcmp(name, data->name, sizeof(name)) || maximum != (uint32_t)data->maximum_count ||
                size != (uint32_t)data->size || signature != DATA_SIGNATURE || valid > 1 || zero_invalid > 1)
                return fail(result, GS_MISMATCH, source.offset);
            if (count < 0 || count > data->maximum_count || actual < 0 || actual > count || first_free < 0 ||
                first_free > data->maximum_count || (valid && !(next & 0x8000u)))
                return fail(result, GS_COUNTS, source.offset);
            int32_t used = 0;
            for (int32_t absolute = 0; absolute < count; ++absolute) {
                unsigned char *element = (unsigned char *)data->data + (size_t)absolute * (size_t)data->size;
                uint32_t identifier = get16(&source);
                if (!identifier)
                    continue;
                if (!(identifier & 0x8000u))
                    return fail(result, GS_COUNTS, source.offset);
                ++used;
                memset(element, 0, (size_t)data->size);
                short stored = (short)identifier;
                memcpy(element, &stored, 2);
                if ((int)index == region->objects) {
                    struct gs_object_header *header = (void *)element;
                    header->flags = (byte)get8(&source);
                    header->type = (byte)get8(&source);
                    header->cluster_index = (short)get16(&source);
                    header->data_size = (short)get16(&source);
                    uint32_t ordinal = get32(&source);
                    /* (the block's ordinal until the pool is taken) */
                    header->datum = NULL;
                    memcpy(&header->datum, &ordinal, 4);
                } else {
                    uint16_t second = (uint16_t)get16(&source);
                    memcpy(element + 2, &second, 2);
                    get_words(&source, element + 4, (size_t)data->size - 4);
                }
            }
            if (used != actual)
                return fail(result, GS_COUNTS, source.offset);
            data->valid = (boolean)valid;
            data->identifier_zero_invalid = (boolean)zero_invalid;
            data->first_free_absolute_index = (short)first_free;
            data->count = (short)count;
            data->actual_count = (short)actual;
            data->next_identifier = (short)next;
        } else if (a->kind == GS_POOL) {
            struct memory_pool *pool = (void *)bytes;
            uint32_t size = get32(&source), free_size = get32(&source), blocks = get32(&source);
            uint32_t cursor = 0;
            struct memory_pool_block *previous = NULL;
            if (source.failed)
                return fail(result, GS_TRUNCATED, source.offset);
            if (size != (uint32_t)pool->size || free_size > size || blocks > size / sizeof(struct memory_pool_block))
                return fail(result, GS_MISMATCH, source.offset);
            for (uint32_t ordinal = 0; ordinal < blocks; ++ordinal) {
                uint32_t at = get32(&source), block_size = get32(&source);
                uint32_t owner = get32(&source), element = get32(&source), field = get32(&source);
                if (source.failed)
                    return fail(result, GS_TRUNCATED, source.offset);
                if (at < cursor || at % 4 || block_size < sizeof(struct memory_pool_block) || block_size % 4 ||
                    at > size || block_size > size - at)
                    return fail(result, GS_COUNTS, source.offset);
                if (owner != (uint32_t)region->objects || field != OBJECT_POOL_FIELD ||
                    element >= (uint32_t)objects->count || bound[element])
                    return fail(result, GS_OWNER, source.offset);
                struct gs_object_header *header =
                    (void *)((unsigned char *)objects->data + (size_t)element * 12u);
                uint32_t claimed;
                memcpy(&claimed, &header->datum, 4);
                if (!header->identifier || claimed != ordinal)
                    return fail(result, GS_OWNER, source.offset);
                struct memory_pool_block *block = (void *)((unsigned char *)pool->base_address + at);
                block->header_signature = 0x68656164ul; /* 'head' */
                block->size = (long)block_size;
                block->reference = &header->datum;
                block->next_block = NULL;
                block->previous_block = previous;
                block->trailer_signature = 0x7461696cul; /* 'tail' */
                if (previous)
                    previous->next_block = block;
                else
                    pool->first_block = block;
                pool->last_block = block;
                previous = block;
                get_words(&source, (unsigned char *)(block + 1), block_size - sizeof(*block));
                header->datum = block + 1;
                bound[element] = 1;
                cursor = at + block_size;
                pool->free_size -= (long)block_size;
            }
            if ((uint32_t)pool->free_size != free_size)
                return fail(result, GS_COUNTS, source.offset);
        } else if (a->kind == GS_BYTES) {
            get_words(&source, bytes, a->size);
        }
    }
    uint32_t computed = source.crc;
    uint32_t stored = get32(&source);
    if (source.failed)
        return fail(result, GS_TRUNCATED, source.offset);
    if (stored != computed)
        return fail(result, GS_CHECKSUM, source.offset);
    unsigned char extra;
    if (fread(&extra, 1, 1, file) != 0)
        return fail(result, GS_TRUNCATED, source.offset);
    /* every live object has its block back */
    for (short absolute = 0; absolute < objects->count; ++absolute) {
        struct gs_object_header *header = (void *)((unsigned char *)objects->data + (size_t)absolute * 12u);
        if (header->identifier && !bound[absolute])
            return fail(result, GS_OWNER, (size_t)absolute);
    }
    unsigned long engine_asserts = gs_engine_assert_count;
    for (unsigned index = 0; index < region->count; ++index)
        if (region->allocations[index].kind == GS_DATA)
            data_verify(array(region, (int)index));
    if (gs_engine_assert_count != engine_asserts)
        return fail(result, GS_ENGINE, (size_t)gs_engine_assert_line);
    region->step = step;
    region->live = 1;
    return 1;
}

size_t gs_describe(const struct gs_report *r, char *text, size_t capacity)
{
    if (!r || !text || !capacity)
        return 0;
    int written = snprintf(text, capacity,
        "STATE step=%u players=%u objects=%u actors=%u props=%u blocks=%u pool_used=%u pool_free=%u "
        "object_next_identifier=%04x player_next_identifier=%04x image_bytes=%u image_crc=%08x stale=%u/%u\n",
        r->step, r->players, r->objects, r->actors, r->props, r->blocks, r->pool_used, r->pool_free,
        r->object_identifier_seed, r->player_identifier_seed, r->image_bytes, r->image_crc, r->stale_rejected,
        r->stale_checked);
    return written < 0 ? 0 : (size_t)written < capacity ? (size_t)written : capacity - 1;
}
