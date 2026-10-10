#ifndef WII_CACHE_MATERIAL_PROBE_H
#define WII_CACHE_MATERIAL_PROBE_H

#include "cache_bsp_probe.h"

#define CACHE_MATERIAL_MAX_LIGHTMAPS 128u
#define CACHE_MATERIAL_MAX_PER_LIGHTMAP 2048u
#define CACHE_MATERIAL_MAX_VERTICES 64000u
#define CACHE_MATERIAL_BYTES 256u
#define CACHE_MATERIAL_LIGHTMAP_BYTES 32u
#define CACHE_MATERIAL_COMPRESSED_VERTEX_BYTES 32u

enum cache_material_error {
    CACHE_MATERIAL_OK, CACHE_MATERIAL_ARGUMENT, CACHE_MATERIAL_STATE,
    CACHE_MATERIAL_GENERATION, CACHE_MATERIAL_WORKSPACE, CACHE_MATERIAL_COUNT,
    CACHE_MATERIAL_SPAN, CACHE_MATERIAL_OVERFLOW, CACHE_MATERIAL_TYPE,
    CACHE_MATERIAL_STRING, CACHE_MATERIAL_DATUM, CACHE_MATERIAL_GROUP,
    CACHE_MATERIAL_RESOURCE, CACHE_MATERIAL_OVERLAP, CACHE_MATERIAL_CAPACITY,
    CACHE_MATERIAL_VALUE
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
    struct cache_material_span surfaces;
    uint32_t surface_count;
};
struct cache_material_surface_projection { uint16_t vertex_indices[3]; };
struct cache_material_compressed_vertex_projection {
    uint32_t position_bits[3];
    uint32_t normal_packed, binormal_packed, tangent_packed;
    uint32_t texcoord_bits[2];
};
struct cache_material_surface_vertices_projection {
    struct cache_material_compressed_vertex_projection vertices[3];
};
/* Corner-major x, y, z binary32 bit patterns as native words. */
struct cache_material_surface_positions_projection {
    uint32_t position_bits[3][3];
};
struct cache_material_vector_projection { float components[3]; };
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
 * each root/lightmap/material stays opaque. Root surfaces are 6-byte records;
 * their three unsigned index words may be inspected explicitly LE. An explicit
 * material surface can also resolve direct compressed environment ordinals;
 * global material ownership and lightmap limits stay separate. Float words retain their bits.
 * Compressed environment records expose eight LE32 words without decompression.
 * No complete native vertex/shader conversion, engine pools, or standalone cache save.
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
/* Ordinal addresses the whole root surface block, not a material-local vertex
 * buffer. Span coordinates are relative to the shared TAG slot. An invalid
 * ordinal, stale parent/child or aliased output rejects without publishing any
 * output bytes; output/result obey the same disjoint truthful-object contract. */
int cache_material_get_surface(const struct cache_material_view *, size_t ordinal,
                                struct cache_material_surface_projection *, struct cache_material_result *);
int cache_material_get_lightmap(const struct cache_material_view *, size_t,
                                 struct cache_material_lightmap_projection *, struct cache_material_result *);
int cache_material_get_material(const struct cache_material_view *, size_t,
                                 struct cache_material_projection *, struct cache_material_result *);
/* Select a surface within the material's validated signed source range. Local
 * ordinals must be below that material's count even when the corresponding root
 * ordinal exists. Index words remain unchanged; this getter does not read vertices.
 * Output/result and stale-view behavior match the whole-root surface getter. */
int cache_material_get_material_surface(const struct cache_material_view *, size_t material_index,
                                        size_t local_ordinal, struct cache_material_surface_projection *,
                                        struct cache_material_result *);
/* Ordinal selects the compressed environment records at compressed tag-data
 * address, bounded by that material's environment count. Hardware descriptor
 * Data/offset/base are not used. Position/texcoord bits and packed vectors are
 * preserved without float interpretation, decompression or triangle association.
 * Parent pins, output/result disjointness and rejection atomicity match getters. */
int cache_material_get_compressed_vertex(const struct cache_material_view *, size_t material_index,
                                         size_t ordinal, struct cache_material_compressed_vertex_projection *,
                                         struct cache_material_result *);
/* Select a material-local surface and resolve its three unsigned indices as
 * direct compressed environment record ordinals for that same material. All
 * indices must be below its environment count before vertex payload reads;
 * all three records are staged before output publication. No lightmap bound,
 * global material ownership, hardware offset/base or decompression is inferred.
 * Output/result and stale-view rules match the individual vertex getter. */
int cache_material_get_surface_vertices(const struct cache_material_view *, size_t material_index,
                                        size_t local_ordinal, struct cache_material_surface_vertices_projection *,
                                        struct cache_material_result *);
/* Renderer boundary for one material-local triangle: the three positions' raw
 * binary32 bits in corner order, as native words. Any infinity or NaN rejects
 * with CACHE_MATERIAL_VALUE (offset = corner * 3 + axis) before publication.
 * Zero signs and subnormals pass bit-exact; consumers store these words with
 * integer stores (no CPU float conversion) into GX F32 position arrays.
 * Output overlap is checked first; index bounds, parent pins, stale views and
 * whole-output atomicity otherwise match cache_material_get_surface_vertices. */
int cache_material_get_surface_positions(const struct cache_material_view *, size_t material_index,
                                         size_t local_ordinal, struct cache_material_surface_positions_projection *,
                                         struct cache_material_result *);
/* Pure by-value decoder for source signed 11/11/10-bit packed vectors. It uses
 * the source midpoint rule (2q+1)/2047 for the first two components and /1023
 * for the third, with source float reciprocals. Zero packed fields yield a
 * positive midpoint, not zero. No normalization, clamp or encoding is applied.
 * All packed words are valid; output is owned binary32 caller storage and must
 * be truthful and disjoint from result. No view or borrowed buffers are used. */
int cache_material_decode_packed_vector(uint32_t packed, struct cache_material_vector_projection *,
                                        struct cache_material_result *);

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
