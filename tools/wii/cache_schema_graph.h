#ifndef WII_CACHE_SCHEMA_GRAPH_H
#define WII_CACHE_SCHEMA_GRAPH_H
#include <stddef.h>
#include <stdint.h>

/* Whole-map tag graph traversal through the upstream validator's schema
 * (cache_schema_tables.c), and relocation of every pointer field it names
 * (HWI-007). Portable C11: numeric fields are read explicitly little-endian;
 * pointer fields are read either as encoded Xbox addresses (LE) or, after
 * cache_graph_relocate, as native pointers stored in native byte order.
 *
 * The walk follows tag_validate.c: the header and tag table, each tag's root
 * through its group's schema (extents, then values, then its blocks'
 * elements), then the one loaded structure BSP. Nothing is corrected: what
 * the validator would correct is counted, and what it would refuse rejects.
 * Buffer descriptors' Data words (tag header and BSP header) are addresses, as
 * the validator checks them; the model-part and BSP-material buffer pointers a
 * schema check reads are walked from cache_schema_pointers. */

#define CACHE_GRAPH_TAG_BASE UINT32_C(0x803a6000)
#define CACHE_GRAPH_CACHE_BYTES ((size_t)0x01600000)
#define CACHE_GRAPH_MAX_TAGS 65535u
#define CACHE_GRAPH_MAX_EDGES 262144u
#define CACHE_GRAPH_MAX_GROUPS 96u
#define CACHE_GRAPH_MAX_PAIRS 512u

enum cache_graph_error {
    CACHE_GRAPH_OK, CACHE_GRAPH_ARGUMENT, CACHE_GRAPH_WORKSPACE, CACHE_GRAPH_HEADER,
    CACHE_GRAPH_TABLE, CACHE_GRAPH_ROOT, CACHE_GRAPH_EXTENT, CACHE_GRAPH_OVERLAP,
    CACHE_GRAPH_DEPTH, CACHE_GRAPH_ALIGNMENT, CACHE_GRAPH_BSP, CACHE_GRAPH_EDGES,
    CACHE_GRAPH_SCHEMA, CACHE_GRAPH_NATIVE
};

struct cache_graph_result {
    enum cache_graph_error error;
    size_t offset;        /* tag-slot offset of the failing field or extent */
    long tag_ordinal;     /* -1 outside a tag */
};

struct cache_graph_input {
    const unsigned char *tags;   /* the tag slot: encoded address base maps to tags[0] */
    size_t tag_bytes;            /* loaded tag data */
    size_t cache_bytes;          /* tag slot capacity (the Xbox tag cache) */
    uint32_t encoded_base;
    size_t bsp_offset, bsp_bytes; /* one loaded structure BSP in the slot; bsp_bytes 0: none */
    int32_t bsp_tag_index;       /* its tag datum */
    int native;                  /* pointer fields hold native pointers (after relocation) */
};

struct cache_graph_group_count { uint32_t group_tag, tags, walked, reachable; };
struct cache_graph_pair_count { uint32_t from_group, to_group, edges; };

struct cache_graph_report {
    uint32_t tag_count, scenario_ordinal, roots_walked, roots_without_schema, bsp_walked;
    uint32_t blocks, nonempty_blocks, elements, structs, data_fields, data_bytes;
    uint32_t file_data_fields, file_data_bytes;
    uint32_t references, references_none, references_resolved, names_valid, names_invalid;
    uint32_t tag_indices, tag_indices_none, tag_indices_resolved;
    uint32_t block_indices, enums, strings, resets, checks, buffers;
    uint32_t corrections;        /* what the validator would correct (none expected for retail) */
    uint32_t supplementary_fields, supplementary_none; /* check-read buffer pointers (cache_schema_pointers) */
    uint32_t claimed_bytes, pointer_fields, null_fields, max_depth, edges, reachable;
    /* address-like words (in the tag slot's encoded range) that no pointer field names: in claimed tag
     * bytes (4-aligned values: candidate unread pointers, or datums/data alike), or in unclaimed bytes */
    uint32_t residual_address_words, residual_claimed_aligned, residual_claimed_unaligned, residual_unclaimed;
    uint32_t digest, edge_digest;
    uint32_t group_count, pair_count;
    struct cache_graph_group_count groups[CACHE_GRAPH_MAX_GROUPS];
    struct cache_graph_pair_count pairs[CACHE_GRAPH_MAX_PAIRS];
};

const char *cache_graph_error_name(enum cache_graph_error);
/* Workspace for a tag slot of cache_bytes: claim/pointer/null bitmaps, edges and reachability. */
size_t cache_graph_workspace_bytes(size_t cache_bytes);
/* Walk and report; workspace is scratch (any alignment-8 storage disjoint from the tags). The tags
 * are only read. The report's digest is region-independent: offsets, never addresses. */
int cache_graph_walk(const struct cache_graph_input *, void *workspace, size_t workspace_bytes,
                     struct cache_graph_report *, struct cache_graph_result *);
/* Walk the encoded tags (input->native must be 0), then rewrite every pointer field the walk
 * named to the native address of its target in this slot (native order), and zero the empty
 * ones the validator nulls. Native pointers must fit 32 bits. On rejection nothing is
 * written. The report is the encoded walk's (with the residual audit). */
int cache_graph_relocate(const struct cache_graph_input *, unsigned char *tags, void *workspace,
                         size_t workspace_bytes, struct cache_graph_report *, struct cache_graph_result *);
/* The canonical report as text lines (identical on every host/target for the same input). Returns
 * the length written (truncated to capacity - 1), or 0 on argument error. */
size_t cache_graph_describe(const struct cache_graph_report *, char *text, size_t capacity);
#endif
