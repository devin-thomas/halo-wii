#ifndef WII_PACKET_ARRAY_POLICY_H
#define WII_PACKET_ARRAY_POLICY_H
#include "packet_verifier_policy.h"

enum packet_array_error {
    PACKET_ARRAY_OK, PACKET_ARRAY_ARGUMENT, PACKET_ARRAY_SCHEMA,
    PACKET_ARRAY_MISSING_END, PACKET_ARRAY_EXTENT, PACKET_ARRAY_SIZE,
    PACKET_ARRAY_ALLOCATION, PACKET_ARRAY_VERSION, PACKET_ARRAY_CAPACITY,
    PACKET_ARRAY_LENGTH, PACKET_ARRAY_COUNT, PACKET_ARRAY_WIRE,
    PACKET_ARRAY_STRING, PACKET_ARRAY_BUDGET, PACKET_ARRAY_OVERLAP
};
struct packet_array_result {
    enum packet_array_error error;
    enum packet_verifier_error schema_error;
    size_t field_index;
    uint32_t native_required;
    long wire_used;
};
struct packet_array_node {
    struct data_packet_field field;
    uint32_t extent, child_extent;
    size_t next;
};
struct packet_array_plan {
    struct packet_array_node *nodes;
    size_t count, depth;
    uint32_t extent;
    short version;
};
/* A zero-initialized, unowned plan receives a schema snapshot on success.
 * The truthful bound is <=32767. Original metadata is never modified.
 * Every field reserves its full latent extent at every runtime version. */
boolean packet_array_compile(const struct data_packet_definition *, size_t,
                             struct packet_array_plan *, struct packet_array_result *);
void packet_array_destroy(struct packet_array_plan *);
const char *packet_array_error_name(enum packet_array_error);
/* Capacities describe actual accessible objects. Plan must remain immutable.
 * Overlapping native/wire spans reject before IO. Plan, result and optional
 * version control storage must be separate from data buffers and each other.
 * Successful decode accepts trailing bytes and reports only consumed bytes. */
boolean packet_array_encode(const struct packet_array_plan *, long version,
                            const void *native, long native_capacity, void *wire,
                            long wire_capacity, struct packet_array_result *);
boolean packet_array_decode(const struct packet_array_plan *, void *wire, long wire_length,
                            void *native, long native_capacity, short *version,
                            struct packet_array_result *);
#endif
