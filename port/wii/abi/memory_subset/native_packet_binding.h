#ifndef WII_NATIVE_PACKET_BINDING_H
#define WII_NATIVE_PACKET_BINDING_H
#include "native_packet_abi.h"
#include "packet_array_policy.h"

enum native_packet_binding_error {
    NATIVE_PACKET_BINDING_OK, NATIVE_PACKET_BINDING_ARGUMENT,
    NATIVE_PACKET_BINDING_IDENTITY, NATIVE_PACKET_BINDING_LAYOUT,
    NATIVE_PACKET_BINDING_SCHEMA
};
struct native_packet_binding {
    struct packet_array_plan plan;
    short type, packet_class;
    uint32_t native_size;
};
struct native_packet_binding_result {
    enum native_packet_binding_error error;
    struct packet_array_result schema;
    size_t measured_native_size;
    short catalog_native_size;
};
const char *native_packet_binding_error_name(enum native_packet_binding_error);
/* Isolated guard: expected enum identity and a measured caller sizeof must
 * agree with the actual table row before owning a schema snapshot. Names and
 * source objects must be live, immutable during binding, truthful and separate
 * from result/control data. The binding stays live and immutable while a group
 * borrows its plan; destroy only after that use ends. Group validation still
 * checks portable type/class and capacity limits independently.
 * Destination is zero-initialized and unowned; rejection leaves it untouched.
 * This is not a production cache, dispatch correction or peer negotiation. */
boolean native_packet_bind(const struct native_abi_catalog *, short expected_type,
                           const char *expected_schema_name, size_t measured_native_size,
                           struct native_packet_binding *, struct native_packet_binding_result *);
void native_packet_binding_destroy(struct native_packet_binding *);
#endif
