#ifndef WII_CACHE_MATERIAL_PROBE_H
#define WII_CACHE_MATERIAL_PROBE_H

#include "cache_bsp_probe.h"

#define CACHE_MATERIAL_MAX_LIGHTMAPS 128u
#define CACHE_MATERIAL_MAX_PER_LIGHTMAP 2048u
#define CACHE_MATERIAL_MAX_VERTICES 64000u
#define CACHE_MATERIAL_BYTES 256u
#define CACHE_MATERIAL_LIGHTMAP_BYTES 32u

enum cache_material_error {
    CACHE_MATERIAL_OK, CACHE_MATERIAL_ARGUMENT, CACHE_MATERIAL_STATE,
    CACHE_MATERIAL_GENERATION, CACHE_MATERIAL_WORKSPACE, CACHE_MATERIAL_COUNT,
    CACHE_MATERIAL_SPAN, CACHE_MATERIAL_OVERFLOW, CACHE_MATERIAL_TYPE,
    CACHE_MATERIAL_STRING, CACHE_MATERIAL_DATUM, CACHE_MATERIAL_GROUP,
    CACHE_MATERIAL_RESOURCE, CACHE_MATERIAL_OVERLAP, CACHE_MATERIAL_CAPACITY
};

struct cache_material_result {
    enum cache_material_error error;
    size_t offset, material_index, required;
};

struct cache_material_span { uint32_t offset, bytes; };
struct cache_material_vertex_metadata {
    int16_t type;
    uint16_t pad;
    int32_t count, offset;
    uint32_t base_address, hardware_format;
};
struct cache_material_data_metadata {
    int32_t size;
    uint32_t pad;
    int32_t file_offset;
    uint32_t address, definition;
};
struct cache_material_projection {
    uint32_t shader_words[4];
    int16_t permutation;
    uint16_t flags;
    int32_t first_surface, surface_count;
    uint32_t centroid_bits[3];
    struct cache_material_vertex_metadata vertex_buffers[2];
    struct cache_material_data_metadata data_fields[2];
    struct cache_material_span source;
};
struct cache_material_root_projection {
    struct cache_material_span source, lightmaps;
    uint32_t material_count, lightmap_count;
};
struct cache_material_lightmap_projection {
    int16_t bitmap_index;
    uint16_t pad;
    uint32_t first_material, material_count;
    struct cache_material_span source;
};
struct cache_material_requirements {
    size_t lightmap_count, material_count, workspace_bytes, serialized_bytes;
};
struct cache_material_control {
    struct cache_bsp_view parent;
    uint64_t generation;
    uint32_t live, lightmap_count, material_count;
    struct cache_material_span published;
    void *workspace;
    size_t workspace_bytes;
};
struct cache_material_view {
    struct cache_bsp_view parent;
    const struct cache_material_control *control;
    uint64_t generation;
};

const char *cache_material_error_name(enum cache_material_error);

/* Partial Xbox LE projection only: root lightmaps, all their materials, numeric
 * shader references, vertex-buffer metadata and tag-data metadata. The rest of
 * each root/lightmap/material stays opaque. Root surface count and its 6-byte
 * element span are checked, without reading triangle contents. Float words retain their bits.
 * No geometry/shader body conversion, engine pools, or standalone cache save.
 * Source counts retain their maxima; workspace uses the actual aggregate.
 * measure performs complete selected validation without allocating or writing
 * source. Results, outputs, controls and source are truthful and disjoint.
 * A valid BSP parent is required; all selected blocks/data occupy its declared
 * BSP window. Names/instances occupy actual tag bytes, never the unloaded gap.
 * Hardware pointers identify the proper-kind 12-byte descriptor record; Data
 * is interpreted only as its pre-registration payload address for this probe.
 * Offset/base/Common/Lock words remain opaque numeric metadata. */
int cache_material_measure(const struct cache_bsp_view *, struct cache_material_requirements *,
                            struct cache_material_result *);

/* Initialize control to zero and keep its address accessible while its copied
 * parent pin is live. Workspace is aligned to the projection type and contains
 * two equal halves. On rejection, control/view/prior published bytes/source are
 * unchanged; scratch may change. A new publication advances generation.
 * Reusing a workspace must leave one new half disjoint from old publication;
 * otherwise WORKSPACE rejects rather than overwrite prior publication.
 * Source, published workspace and controls are immutable while in use; single
 * caller, no concurrent mutation. Controls may occupy a separate arena slot.
 * Each view validates its copied outer parent BEFORE child control/workspace
 * dereference, so released/rebound parent backing can be overwritten safely.
 * This does not authenticate handles, retain derived pointers, or own buffers. */
int cache_material_bind(struct cache_material_control *, const struct cache_bsp_view *,
                         void *workspace, size_t workspace_bytes, struct cache_material_view *,
                         struct cache_material_result *);
int cache_material_get_root(const struct cache_material_view *, struct cache_material_root_projection *,
                             struct cache_material_result *);
int cache_material_get_lightmap(const struct cache_material_view *, size_t,
                                 struct cache_material_lightmap_projection *, struct cache_material_result *);
int cache_material_get_material(const struct cache_material_view *, size_t,
                                 struct cache_material_projection *, struct cache_material_result *);

/* Selected raw spans serialize root, all lightmaps, then flattened materials.
 * Known projected numeric words are explicitly rewritten LE over opaque copies.
 * Encoded addresses are retained; payloads and referenced tag bodies are absent.
 * The full output capacity must be disjoint from TAG reservation, workspace,
 * controls/view/result/used. Rejection leaves output and used unchanged.
 * Active unload advances generation; inactive unload is idempotent. Exhaustion
 * rejects atomically and retains live ownership. Unload requires accessible
 * control; it must never be called on already-freed parent backing. */
int cache_material_serialize(const struct cache_material_view *, void *, size_t capacity,
                              size_t *used, struct cache_material_result *);
int cache_material_unload(struct cache_material_control *, struct cache_material_result *);

#endif
