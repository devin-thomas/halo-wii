#ifndef WII_CACHE_BSP_PROBE_H
#define WII_CACHE_BSP_PROBE_H
#include "cache_address_probe.h"
#include "cache_arena_plan.h"

#define CACHE_BSP_TAG_BASE UINT32_C(0x803a6000)
#define CACHE_BSP_TAG_LIMIT ((size_t)0x01600000)
#define CACHE_BSP_SCENARIO_BYTES 1456u
#define CACHE_BSP_ROOT_BYTES 648u
#define CACHE_BSP_HEADER_BYTES 24u
#define CACHE_BSP_REFERENCE_BYTES 32u
#define CACHE_BSP_MAX_REFERENCES 16u

enum cache_bsp_error
{
    CACHE_BSP_OK, CACHE_BSP_ARGUMENT, CACHE_BSP_STATE, CACHE_BSP_GENERATION,
    CACHE_BSP_SPAN, CACHE_BSP_COUNT, CACHE_BSP_OVERFLOW, CACHE_BSP_DATUM,
    CACHE_BSP_GROUP, CACHE_BSP_SIGNATURE, CACHE_BSP_OVERLAP, CACHE_BSP_CAPACITY
};
struct cache_bsp_result { enum cache_bsp_error error; size_t offset; };
struct cache_bsp_reference
{
    int32_t file_offset, file_size;
    uint32_t address, unused_word, group, name_address, name_length_word, datum;
    uint32_t scenario_datum;
    size_t ordinal, slot_offset, rounded_bytes;
};
struct cache_bsp_header
{
    uint32_t root_address;
    int32_t vertex_count;
    uint32_t vertex_address;
    int32_t index_count;
    uint32_t index_address, signature;
};
struct cache_bsp_descriptor
{
    uint32_t words[3];
    struct cache_address_span raw;
};
struct cache_bsp_control
{
    const struct cache_arena_owner *owner;
    struct cache_arena_handle handle;
    size_t tag_bytes, slot_bytes;
    uint32_t map_bytes;
    struct cache_bsp_reference reference;
    struct cache_bsp_header header;
    struct cache_address_span root, descriptors[2];
    uint64_t generation;
    int live;
};
struct cache_bsp_view
{
    const struct cache_arena_owner *owner;
    struct cache_arena_handle handle;
    const struct cache_bsp_control *control;
    uint64_t generation;
};

const char *cache_bsp_error_name(enum cache_bsp_error);

/* Allocation-free, isolated Xbox LE address inspection, not engine integration.
 * Originating owner remains live at a stable address. Zero-initialized child
 * control is accessible while its parent handle is live; it may occupy a
 * separate parent slot. Views copy the parent pin and check it BEFORE touching
 * child control, so parent release safely invalidates old child backing.
 * All tag/BSP source/control/output/result objects are truthful and
 * disjoint. The parent slot is at most 22MiB; tag bytes and bound BSP bytes are
 * immutable until unload/rebind. Single caller; no concurrent mutation.
 * Select reads only the actual tag-byte window and preserves numeric reference
 * words. The caller copies the declared BSP to reference.slot_offset before
 * bind. The gap is never valid data. Raw Xbox pointers are never native casts.
 * Successful bind and active unload advance child generation. Failed operations
 * preserve control/output/source; an exhausted active unload retains ownership.
 * Unload requires an accessible control, never already-freed parent backing.
 * Inactive unload is idempotent. Native control/handle pairs are not authenticated
 * capabilities; use them only with their originating controls. */
int cache_bsp_select(const struct cache_arena_owner *, const struct cache_arena_handle *,
                     size_t tag_bytes, uint32_t map_bytes, size_t ordinal,
                     struct cache_bsp_reference *, struct cache_bsp_result *);
int cache_bsp_bind(struct cache_bsp_control *, const struct cache_arena_owner *,
                   const struct cache_arena_handle *, size_t tag_bytes, uint32_t map_bytes,
                   size_t ordinal, struct cache_bsp_view *, struct cache_bsp_result *);
int cache_bsp_unload(struct cache_bsp_control *, struct cache_bsp_result *);

/* Every use first checks parent and child epochs. Returned spans are numeric
 * offsets relative to the shared tag slot. Empty spans return {0,0}, accept any
 * address/stride, and permit no dereference (including one-past addresses).
 * Nonempty spans must fit wholly in ONE valid window. Root proves only its
 * opaque 648-byte extent. Descriptor Data(+4) proves one addressed byte only;
 * neither descriptor array proves geometry payload length or conversion. */
int cache_bsp_lookup(const struct cache_bsp_view *, struct cache_bsp_reference *, struct cache_bsp_result *);
int cache_bsp_get_header(const struct cache_bsp_view *, struct cache_bsp_header *, struct cache_bsp_result *);
int cache_bsp_get_root(const struct cache_bsp_view *, struct cache_address_span *, struct cache_bsp_result *);
int cache_bsp_get_descriptor(const struct cache_bsp_view *, unsigned kind, size_t index,
                             struct cache_bsp_descriptor *, struct cache_bsp_result *);
int cache_bsp_resolve(const struct cache_bsp_view *, uint32_t address, int64_t count, size_t stride,
                      struct cache_address_span *, struct cache_bsp_result *);
/* Whole declared BSP inspection serialization only: preserve all original
 * encoded addresses/opaque bytes, rewrite six header/three descriptor LE words.
 * Destination cannot overlap the bound tag slot bytes. This is not a standalone
 * cache or full typed root conversion. used/output change only on success. */
int cache_bsp_serialize(const struct cache_bsp_view *, void *output, size_t capacity,
                        size_t *used, struct cache_bsp_result *);
#endif
