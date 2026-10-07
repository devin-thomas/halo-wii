#include "native_packet_binding.h"
#include <limits.h>

const char *native_packet_binding_error_name(enum native_packet_binding_error error)
{
    static const char *const names[] = {"ok", "argument", "identity", "layout", "schema"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}
static boolean binding_fail(struct native_packet_binding_result *r, enum native_packet_binding_error error)
{
    r->error = error;
    return FALSE;
}
boolean native_packet_bind(const struct native_abi_catalog *row, short type,
                           const char *identity, size_t native_size,
                           struct native_packet_binding *binding,
                           struct native_packet_binding_result *r)
{
    match_assert(__FILE__, __LINE__, r);
    *r = (struct native_packet_binding_result){0};
    r->measured_native_size = native_size;
    if (!row || !identity || !binding || !row->name || !row->definition || !row->definition->name ||
        binding->plan.nodes || binding->plan.count || binding->plan.depth ||
        binding->plan.extent || binding->plan.version)
        return binding_fail(r, NATIVE_PACKET_BINDING_ARGUMENT);
    r->catalog_native_size = row->definition->size;
    if (type < 0 || row->type != type || strcmp(row->name, identity) != 0 ||
        strcmp(row->definition->name, row->name) != 0)
        return binding_fail(r, NATIVE_PACKET_BINDING_IDENTITY);
    if (native_size > SHRT_MAX || row->definition->size < 0 ||
        native_size != (size_t)row->definition->size)
        return binding_fail(r, NATIVE_PACKET_BINDING_LAYOUT);
    struct packet_array_plan plan = {0};
    if (!packet_array_compile(row->definition, row->field_bound, &plan, &r->schema))
        return binding_fail(r, NATIVE_PACKET_BINDING_SCHEMA);
    if (plan.extent != native_size) {
        packet_array_destroy(&plan);
        return binding_fail(r, NATIVE_PACKET_BINDING_LAYOUT);
    }
    *binding = (struct native_packet_binding){plan, type, row->packet_class, (uint32_t)native_size};
    return TRUE;
}
void native_packet_binding_destroy(struct native_packet_binding *binding)
{
    if (!binding) return;
    packet_array_destroy(&binding->plan);
    *binding = (struct native_packet_binding){0};
}
