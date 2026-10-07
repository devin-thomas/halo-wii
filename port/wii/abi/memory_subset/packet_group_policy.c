#include "packet_group_policy.h"
#include <limits.h>

static boolean group_fail(struct packet_group_result *r, enum packet_group_error error)
{
    r->error = error;
    return FALSE;
}

const char *packet_group_error_name(enum packet_group_error error)
{
    static const char *const names[] = {
        "ok", "argument", "schema", "capacity", "length", "overlap",
        "type", "class", "no_header", "no_definition", "payload", "append"
    };
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

boolean packet_group_validate(const struct packet_group_plan *g, size_t bound,
                              struct packet_group_result *r)
{
    match_assert(__FILE__, __LINE__, r);
    *r = (struct packet_group_result){0};
    if (!g || !g->entries) return group_fail(r, PACKET_GROUP_ARGUMENT);
    if (g->type_count < 1 || g->type_count > 128 || (size_t)g->type_count > bound ||
        g->class_count < 1 || g->maximum_decoded_size < 0 ||
        g->maximum_decoded_size > SHRT_MAX || g->maximum_encoded_size < 1 ||
        g->maximum_encoded_size > SHRT_MAX) return group_fail(r, PACKET_GROUP_SCHEMA);
    for (short n = 0; n < g->type_count; ++n) {
        const struct packet_group_entry *e = &g->entries[n];
        if (e->packet_class < 0 || e->packet_class >= g->class_count)
            return group_fail(r, PACKET_GROUP_SCHEMA);
        if (e->plan && (!e->plan->nodes || !e->plan->count || !e->plan->depth ||
            e->plan->extent > (uint32_t)g->maximum_decoded_size ||
            e->plan->extent >= (uint32_t)g->maximum_encoded_size ||
            e->plan->version < 0 || e->plan->version > 255))
            return group_fail(r, PACKET_GROUP_SCHEMA);
    }
    return TRUE;
}

static boolean accessible(const void *native, uint32_t native_extent,
                          const void *wire, long wire_extent,
                          struct packet_group_result *r)
{
    uintptr_t a = (uintptr_t)native, b = (uintptr_t)wire;
    if (native_extent > UINTPTR_MAX - a || (uintptr_t)wire_extent > UINTPTR_MAX - b)
        return group_fail(r, PACKET_GROUP_ARGUMENT);
    if (native_extent && wire_extent && a < b + (uintptr_t)wire_extent && b < a + native_extent)
        return group_fail(r, PACKET_GROUP_OVERLAP);
    return TRUE;
}

static boolean capacities(long native_capacity, long wire_capacity,
                          struct packet_group_result *r)
{
    if (native_capacity < 0 || wire_capacity < 0) return group_fail(r, PACKET_GROUP_CAPACITY);
    if (wire_capacity > SHRT_MAX) return group_fail(r, PACKET_GROUP_LENGTH);
    return TRUE;
}

boolean packet_group_encode(const struct packet_group_plan *g, size_t bound,
                            const void *native, long native_capacity, void *wire,
                            long wire_capacity, short *wire_size, short type,
                            long version, struct packet_group_result *r)
{
    if (!packet_group_validate(g, bound, r)) return FALSE;
    if (!native || !wire || !wire_size) return group_fail(r, PACKET_GROUP_ARGUMENT);
    if (!capacities(native_capacity, wire_capacity, r)) return FALSE;
    if (type < 0 || type >= g->type_count) return group_fail(r, PACKET_GROUP_TYPE);
    const struct packet_array_plan *p = g->entries[type].plan;
    if (!p) return group_fail(r, PACKET_GROUP_NO_DEFINITION);
    if (!accessible(native, p->extent, wire, wire_capacity, r)) return FALSE;
    long limit = wire_capacity < g->maximum_encoded_size ? wire_capacity : g->maximum_encoded_size;
    boolean encoded = packet_array_encode(p, version, native, native_capacity, wire, limit, &r->payload);
    r->wire_used = r->payload.wire_used;
    r->payload_length = r->wire_used;
    *wire_size = (short)r->wire_used;
    if (!encoded) return group_fail(r, PACKET_GROUP_PAYLOAD);
    /* Retain the original strict group maximum and the completed payload on
     * trailer failure, while separately checking actual writable capacity. */
    if (r->wire_used >= g->maximum_encoded_size - 1 || r->wire_used >= wire_capacity)
        return group_fail(r, PACKET_GROUP_APPEND);
    ((byte *)wire)[r->wire_used++] = (byte)type;
    *wire_size = (short)r->wire_used;
    return TRUE;
}

boolean packet_group_decode(const struct packet_group_plan *g, size_t bound,
                            void *native, long native_capacity, void *wire,
                            long wire_capacity, short *wire_size, short *type,
                            short *version, short expected_class,
                            struct packet_group_result *r)
{
    if (!packet_group_validate(g, bound, r)) return FALSE;
    if (!native || !wire || !wire_size || !type || !version)
        return group_fail(r, PACKET_GROUP_ARGUMENT);
    r->supplied_length = *wire_size;
    if (!capacities(native_capacity, wire_capacity, r)) return FALSE;
    if (expected_class < 0 || expected_class >= g->class_count)
        return group_fail(r, PACKET_GROUP_CLASS);
    if (*wire_size < 0) return group_fail(r, PACKET_GROUP_LENGTH);
    if (*wire_size > wire_capacity) return group_fail(r, PACKET_GROUP_CAPACITY);
    if (!*wire_size) return group_fail(r, PACKET_GROUP_NO_HEADER);
    short selected = ((const byte *)wire)[*wire_size - 1];
    if (selected >= g->type_count) return group_fail(r, PACKET_GROUP_TYPE);
    const struct packet_group_entry *e = &g->entries[selected];
    if (e->packet_class != expected_class) return group_fail(r, PACKET_GROUP_CLASS);
    if (!accessible(native, e->plan ? e->plan->extent : 0, wire, *wire_size, r)) return FALSE;
    r->payload_length = --*wire_size;
    if (e->plan) {
        boolean decoded = packet_array_decode(e->plan, wire, *wire_size, native,
                                              native_capacity, version, &r->payload);
        r->wire_used = r->payload.wire_used;
        if (!decoded) return group_fail(r, PACKET_GROUP_PAYLOAD);
    }
    *type = selected;
    return TRUE;
}
