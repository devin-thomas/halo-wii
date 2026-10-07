#ifndef WII_PACKET_GROUP_POLICY_H
#define WII_PACKET_GROUP_POLICY_H
#include "packet_array_policy.h"

enum packet_group_error {
    PACKET_GROUP_OK, PACKET_GROUP_ARGUMENT, PACKET_GROUP_SCHEMA,
    PACKET_GROUP_CAPACITY, PACKET_GROUP_LENGTH, PACKET_GROUP_OVERLAP,
    PACKET_GROUP_TYPE, PACKET_GROUP_CLASS, PACKET_GROUP_NO_HEADER,
    PACKET_GROUP_NO_DEFINITION, PACKET_GROUP_PAYLOAD, PACKET_GROUP_APPEND
};
struct packet_group_entry {
    short packet_class;
    const struct packet_array_plan *plan;
};
struct packet_group_plan {
    short type_count, class_count;
    long maximum_decoded_size, maximum_encoded_size;
    const struct packet_group_entry *entries;
};
struct packet_group_result {
    enum packet_group_error error;
    struct packet_array_result payload;
    long supplied_length, payload_length, wire_used;
};
/* Immutable caller-owned entries/plans with a truthful accessible table bound.
 * Portable trailer types are 0..127; limits must fit the signed-short wire API.
 * Borrowed plans must come from packet_array_compile and remain alive. Control
 * objects must be separate from data and one another. Validation never
 * initializes or changes production definitions or their cached metadata. */
boolean packet_group_validate(const struct packet_group_plan *, size_t entry_bound,
                              struct packet_group_result *);
const char *packet_group_error_name(enum packet_group_error);
boolean packet_group_encode(const struct packet_group_plan *, size_t entry_bound,
                            const void *native, long native_capacity, void *wire,
                            long wire_capacity, short *wire_size, short type,
                            long version, struct packet_group_result *);
/* wire_capacity is the actual accessible object; *wire_size is supplied length.
 * Valid trailer type/class removes one byte from *wire_size before payload IO,
 * including payload failure. wire_used separately reports consumed payload;
 * successful payload decode accepts unconsumed bytes before the trailer.
 * Null definitions retain legacy type-only decode success; encode rejects. */
boolean packet_group_decode(const struct packet_group_plan *, size_t entry_bound,
                            void *native, long native_capacity, void *wire,
                            long wire_capacity, short *wire_size, short *type,
                            short *version, short expected_class,
                            struct packet_group_result *);
#endif
