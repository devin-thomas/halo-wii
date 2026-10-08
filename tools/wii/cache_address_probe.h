#ifndef WII_CACHE_ADDRESS_PROBE_H
#define WII_CACHE_ADDRESS_PROBE_H
#include <stddef.h>
#include <stdint.h>

enum cache_address_error {
    CACHE_ADDRESS_OK, CACHE_ADDRESS_ARGUMENT, CACHE_ADDRESS_SPAN,
    CACHE_ADDRESS_COUNT, CACHE_ADDRESS_OVERFLOW, CACHE_ADDRESS_SIGNATURE,
    CACHE_ADDRESS_DATUM, CACHE_ADDRESS_NAME, CACHE_ADDRESS_ROOT,
    CACHE_ADDRESS_ALLOCATION, CACHE_ADDRESS_STATE, CACHE_ADDRESS_GROUP
};
struct cache_address_result { enum cache_address_error error; size_t offset,instance_index; };
struct cache_address_span { size_t offset,length; };
struct cache_address_region { const unsigned char *bytes; size_t size; uint32_t encoded_base; };
struct cache_address_header {
    uint32_t instances_address,scenario_datum,checksum;
    int32_t tag_count,vertex_count;
    uint32_t vertex_address;
    int32_t index_count;
    uint32_t index_address,signature;
};
struct cache_address_instance {
    uint32_t group,parent[2],datum,name_address,root_address,unused[2];
};
struct cache_address_graph {
    unsigned char *bytes;
    size_t size,count;
    uint32_t encoded_base;
    struct cache_address_header header;
    struct cache_address_instance *instances;
};
struct cache_address_reference {
    struct cache_address_instance instance;
    struct cache_address_span name,root;
};
const char *cache_address_error_name(enum cache_address_error);
/* Truthful, immutable source buffers and disjoint result/output/control storage
 * are required. Outputs change only on success. Encoded addresses stay numeric.
 * Zero elements accept any encoded address without following it; stride must
 * be nonzero for nonempty spans. Region may not wrap the 32-bit address domain. */
int cache_address_resolve(const struct cache_address_region *,uint32_t,int64_t,size_t,
                          struct cache_address_span *,struct cache_address_result *);
int cache_address_decode_header(const struct cache_address_region *,struct cache_address_header *,
                                struct cache_address_result *);
int cache_address_decode_instance(const void *,size_t,size_t,struct cache_address_instance *,
                                  struct cache_address_result *);
/* Own a lossless copy and numeric index table, at most 22MiB/65535 instances.
 * Destination is zero-initialized/unowned. Load rejects nonordinal datums,
 * missing scenario/scnr, unterminated names, and root addresses outside the
 * actual copied blob. Only sbsp may have a null root (unloaded BSP).
 * Buffer table extents use the source's 12-byte stride; their Data fields are
 * resource offsets and are not interpreted as tag addresses. No nested tag,
 * BSP, geometry, script, checksum or gameplay conversion is established. */
int cache_address_graph_load(const struct cache_address_region *,struct cache_address_graph *,
                             struct cache_address_result *);
int cache_address_graph_lookup(const struct cache_address_graph *,uint32_t,uint32_t,
                               struct cache_address_reference *,struct cache_address_result *);
const unsigned char *cache_address_graph_bytes(const struct cache_address_graph *,size_t *);
void cache_address_graph_unload(struct cache_address_graph *);
#endif
