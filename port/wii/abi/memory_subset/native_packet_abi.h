#ifndef WII_NATIVE_PACKET_ABI_H
#define WII_NATIVE_PACKET_ABI_H
#include <stddef.h>

struct data_packet_definition;
struct data_packet_group_definition;
struct native_abi_member {
    const char *name;
    size_t offset, size;
};
struct native_abi_layout {
    const char *owner, *name;
    size_t size, alignment, member_count;
    const struct native_abi_member *members;
};
struct native_abi_catalog {
    const char *name;
    short type, packet_class;
    struct data_packet_definition *definition;
    size_t field_bound;
};
struct native_abi_enum {
    const char *name;
    short value;
};

size_t native_abi_owner_count(void);
const struct native_abi_layout *native_abi_owner_layouts(size_t, size_t *);
size_t native_abi_default_wchar_width(void);
size_t native_abi_selected_wchar_width(void);
/* Diagnostic copies of actual catalog declarations are borrowed and immutable.
 * packet_array_compile reads them without cache writes. Copy before any mutable
 * initialization/cache experiment; these accessors do not initialize production. */
const struct native_abi_catalog *native_abi_network_catalog(size_t *);
const struct native_abi_catalog *native_abi_key_catalog(size_t *);
const struct native_abi_enum *native_abi_network_enum(size_t *);
const struct data_packet_group_definition *native_abi_network_group(void);
const struct data_packet_group_definition *native_abi_key_group(void);
#endif
