#ifndef WII_CACHE_WIDGET_PROBE_H
#define WII_CACHE_WIDGET_PROBE_H

#include "cache_address_probe.h"
#include "cache_arena_plan.h"

#define CACHE_WIDGET_GROUP UINT32_C(0x44654c61)
#define CACHE_WIDGET_ROOT_BYTES 1004
#define CACHE_WIDGET_BLOCKS 5
#define CACHE_WIDGET_REFERENCES 6
#define CACHE_WIDGET_MAX_DEPTH 32

enum cache_widget_error
{
    CACHE_WIDGET_OK,
    CACHE_WIDGET_ARGUMENT,
    CACHE_WIDGET_STATE,
    CACHE_WIDGET_WORKSPACE,
    CACHE_WIDGET_COUNT,
    CACHE_WIDGET_SPAN,
    CACHE_WIDGET_OVERFLOW,
    CACHE_WIDGET_ENUM,
    CACHE_WIDGET_STRING,
    CACHE_WIDGET_DATUM,
    CACHE_WIDGET_GROUP_ERROR,
    CACHE_WIDGET_OVERLAP,
    CACHE_WIDGET_CYCLE,
    CACHE_WIDGET_DEPTH,
    CACHE_WIDGET_CAPACITY
};

enum cache_widget_block_kind
{
    CACHE_WIDGET_INPUT,
    CACHE_WIDGET_EVENT,
    CACHE_WIDGET_REPLACE,
    CACHE_WIDGET_CONDITIONAL,
    CACHE_WIDGET_CHILD
};

struct cache_widget_result
{
    enum cache_widget_error error;
    size_t offset, node_index, required;
};

struct cache_widget_reference
{
    uint32_t group, name_address, name_length_word, datum;
    struct cache_address_span name;
};

struct cache_widget_block
{
    int32_t count;
    uint32_t address, definition_word;
    size_t stride;
    struct cache_address_span span;
};

struct cache_widget_projection
{
    uint32_t datum;
    struct cache_address_span raw, name;
    int16_t type, controller, bounds[4];
    uint32_t flags, auto_close_word, fade_word;
    struct cache_widget_reference references[CACHE_WIDGET_REFERENCES];
    struct cache_widget_block blocks[CACHE_WIDGET_BLOCKS];
    uint32_t text_color_bits[4];
    int16_t justification, string_list_index, horizontal_offset, vertical_offset;
    uint16_t text_box_flags;
    uint32_t list_flags;
    int16_t list_header_bounds[4], list_footer_bounds[4];
    unsigned state, height;
};

struct cache_widget_element
{
    enum cache_widget_block_kind kind;
    struct cache_address_span raw, text;
    struct cache_widget_reference references[2];
    uint32_t flags;
    int16_t function, event_type, controller, vertical_offset, horizontal_offset;
};

struct cache_widget_graph
{
    const struct cache_arena_owner *owner;
    struct cache_arena_handle tag_handle;
    size_t resident_size;
    uint32_t encoded_base;
    struct cache_widget_projection *nodes;
    size_t count, capacity, serialized_size;
};

const char *cache_widget_error_name(enum cache_widget_error);
int cache_widget_workspace_size(size_t node_capacity, size_t *, struct cache_widget_result *);

/* Partial typed projection of named UI fields only. Root1004/event72/child80/
 * conditional80/input36/replace34 retain opaque bytes in the immutable source.
 * Workspace is aligned to _Alignof(cache_widget_projection), sized by the
 * helper, and disjoint from source/control/output. Its second half is scratch
 * and may change on failure; published first-half projections and graph output
 * change only after the complete graph validates. No allocator is used.
 * Capacity limits the caller's workload, not source block capacities64/32 or
 * loaded-widget depth32. Only child edges and column-list extended-description
 * edges are traversed; other references are checked identities, not loaded.
 * Owner/control/workspace remain live and immutable while views are used.
 * The owner must exclusively pin the tag slot; edits/releases/rebinds require
 * discarding views. Handles are used only with their originating owner.
 * Numeric encoded addresses, definition words, datums, flags and float bits
 * are preserved; no native on-disk structure casts or runtime UI execution. */
int cache_widget_graph_decode(const struct cache_arena_owner *, const struct cache_arena_handle *,
                              size_t resident_size, uint32_t encoded_base, uint32_t root_datum,
                              void *workspace, size_t workspace_bytes, size_t node_capacity,
                              struct cache_widget_graph *, struct cache_widget_result *);
int cache_widget_graph_node(const struct cache_widget_graph *, size_t,
                            struct cache_widget_projection *, struct cache_widget_result *);
int cache_widget_graph_element(const struct cache_widget_graph *, size_t,
                               enum cache_widget_block_kind, size_t,
                               struct cache_widget_element *, struct cache_widget_result *);

/* Lossless concatenation in DFS node order: each full raw root then its five
 * nonempty block spans in field order. This is an inspection serialization,
 * not a relocatable standalone cache or a complete root endian conversion.
 * Capacity/alias failure leaves destination and used unchanged. */
int cache_widget_graph_serialize(const struct cache_widget_graph *, void *, size_t, size_t *,
                                 struct cache_widget_result *);
void cache_widget_graph_release(struct cache_widget_graph *);

#endif
