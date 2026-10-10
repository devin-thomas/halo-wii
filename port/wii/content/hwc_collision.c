/* HWC1 collision BSP loader (HWI-008E). See hwc_collision.h. */
#include "hwc_collision.h"

#include <string.h>

_Static_assert(sizeof(struct hwc_bsp) == 68, "hwc_bsp matches HWC_BSP_FORMAT");
_Static_assert(sizeof(struct hwc_bsp3d_node) == 12 && sizeof(struct hwc_plane) == 16 &&
                   sizeof(struct hwc_leaf) == 8 && sizeof(struct hwc_bsp2d_reference) == 8 &&
                   sizeof(struct hwc_bsp2d_node) == 20 && sizeof(struct hwc_surface) == 12 &&
                   sizeof(struct hwc_edge) == 24 && sizeof(struct hwc_vertex) == 16,
               "collision records match their formats");

static const char *const array_formats[HWC_ARRAYS] = {"iii", "ffff", "Hhi", "ii", "fffii", "iiBbh", "iiiiii", "fffi"};
/* Per-BSP maxima of the engine's tag block definitions. */
static const uint32_t array_maxima[HWC_ARRAYS] = {131072, 65536, 65536, 131072, 65535, 131072, 262144, 131072};
static const uint32_t record_bytes[1 + HWC_ARRAYS] = {68, 12, 16, 8, 8, 20, 12, 24, 16};

#define SIGN_MASK 0x7FFFFFFFu

static int below(int32_t value, uint32_t count) { return value >= 0 && (uint32_t)value < count; }
/* A plane index whose sign bit means "flipped". */
static int plane_ok(int32_t value, uint32_t planes) { return ((uint32_t)value & SIGN_MASK) < planes; }
/* A child: a node index, or with the sign bit set the low bits name a leaf
 * or surface. NONE (-1) is allowed only where none_allowed. */
static int child_ok(int32_t value, uint32_t nodes, uint32_t others, int none_allowed)
{
    if (value >= 0)
        return (uint32_t)value < nodes;
    if (value == -1 && none_allowed)
        return 1;
    return ((uint32_t)value & SIGN_MASK) < others;
}

static enum content_error check_bsp(const struct hwc_collision *c, const struct hwc_bsp *b)
{
    uint32_t n[HWC_ARRAYS];
    for (uint32_t k = 0; k < HWC_ARRAYS; ++k)
        n[k] = b->arrays[k].count;
    const struct hwc_bsp3d_node *nodes = c->bsp3d_nodes + b->arrays[HWC_BSP3D_NODES].first;
    for (uint32_t i = 0; i < n[HWC_BSP3D_NODES]; ++i)
        if (!below(nodes[i].plane, n[HWC_PLANES]) ||
            !child_ok(nodes[i].back_child, n[HWC_BSP3D_NODES], n[HWC_LEAVES], 1) ||
            !child_ok(nodes[i].front_child, n[HWC_BSP3D_NODES], n[HWC_LEAVES], 1))
            return CONTENT_RANGE;
    const struct hwc_leaf *leaves = c->leaves + b->arrays[HWC_LEAVES].first;
    for (uint32_t i = 0; i < n[HWC_LEAVES]; ++i) {
        int32_t count = leaves[i].bsp2d_reference_count, first = leaves[i].first_bsp2d_reference;
        if (count < 0 || (count > 0 && (first < 0 || (uint64_t)first + (uint64_t)count > n[HWC_BSP2D_REFERENCES])))
            return CONTENT_RANGE;
    }
    const struct hwc_bsp2d_reference *refs = c->bsp2d_references + b->arrays[HWC_BSP2D_REFERENCES].first;
    for (uint32_t i = 0; i < n[HWC_BSP2D_REFERENCES]; ++i)
        if (!plane_ok(refs[i].plane, n[HWC_PLANES]) ||
            !child_ok(refs[i].bsp2d_node, n[HWC_BSP2D_NODES], n[HWC_SURFACES], 0))
            return CONTENT_RANGE;
    const struct hwc_bsp2d_node *nodes2d = c->bsp2d_nodes + b->arrays[HWC_BSP2D_NODES].first;
    for (uint32_t i = 0; i < n[HWC_BSP2D_NODES]; ++i)
        if (!child_ok(nodes2d[i].left_child, n[HWC_BSP2D_NODES], n[HWC_SURFACES], 0) ||
            !child_ok(nodes2d[i].right_child, n[HWC_BSP2D_NODES], n[HWC_SURFACES], 0))
            return CONTENT_RANGE;
    const struct hwc_surface *surfaces = c->surfaces + b->arrays[HWC_SURFACES].first;
    for (uint32_t i = 0; i < n[HWC_SURFACES]; ++i)
        if (!plane_ok(surfaces[i].plane, n[HWC_PLANES]) || !below(surfaces[i].first_edge, n[HWC_EDGES]))
            return CONTENT_RANGE;
    const struct hwc_edge *edges = c->edges + b->arrays[HWC_EDGES].first;
    for (uint32_t i = 0; i < n[HWC_EDGES]; ++i) {
        const struct hwc_edge *e = &edges[i];
        if (!below(e->start_vertex, n[HWC_VERTICES]) || !below(e->end_vertex, n[HWC_VERTICES]) ||
            !below(e->forward_edge, n[HWC_EDGES]) || !below(e->reverse_edge, n[HWC_EDGES]) ||
            !(e->left_surface == -1 || below(e->left_surface, n[HWC_SURFACES])) ||
            !(e->right_surface == -1 || below(e->right_surface, n[HWC_SURFACES])))
            return CONTENT_RANGE;
    }
    const struct hwc_vertex *vertices = c->vertices + b->arrays[HWC_VERTICES].first;
    for (uint32_t i = 0; i < n[HWC_VERTICES]; ++i)
        if (!below(vertices[i].first_edge, n[HWC_EDGES]))
            return CONTENT_RANGE;
    return CONTENT_OK;
}

static enum content_error check(const struct hwc_collision *c)
{
    uint64_t cursor[HWC_ARRAYS] = {0};
    for (uint32_t i = 0; i < c->bsp_count; ++i) {
        const struct hwc_bsp *b = &c->bsps[i];
        for (uint32_t k = 0; k < HWC_ARRAYS; ++k) {
            if (b->arrays[k].first != cursor[k])
                return CONTENT_RANGE;
            if (b->arrays[k].count > array_maxima[k])
                return CONTENT_COUNT;
            cursor[k] += b->arrays[k].count;
            if (cursor[k] > c->counts[k])
                return CONTENT_RANGE;
        }
    }
    for (uint32_t k = 0; k < HWC_ARRAYS; ++k)
        if (cursor[k] != c->counts[k])
            return CONTENT_RANGE;
    for (uint32_t i = 0; i < c->bsp_count; ++i) {
        enum content_error error = check_bsp(c, &c->bsps[i]);
        if (error != CONTENT_OK)
            return error;
    }
    return CONTENT_OK;
}

static void set_array(struct hwc_collision *c, uint32_t k, void *array)
{
    switch (k) {
    case HWC_BSP3D_NODES: c->bsp3d_nodes = array; break;
    case HWC_PLANES: c->planes = array; break;
    case HWC_LEAVES: c->leaves = array; break;
    case HWC_BSP2D_REFERENCES: c->bsp2d_references = array; break;
    case HWC_BSP2D_NODES: c->bsp2d_nodes = array; break;
    case HWC_SURFACES: c->surfaces = array; break;
    case HWC_EDGES: c->edges = array; break;
    default: c->vertices = array; break;
    }
}

enum content_error hwc_load(const unsigned char *data, uint32_t bytes, struct hwc_collision *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    struct content_layout bsp, layouts[HWC_ARRAYS];
    if (content_layout_init(&bsp, HWC_BSP_FORMAT) != sizeof(struct hwc_bsp))
        return CONTENT_ARGUMENT;
    for (uint32_t k = 0; k < HWC_ARRAYS; ++k)
        if (content_layout_init(&layouts[k], array_formats[k]) != record_bytes[k + 1])
            return CONTENT_ARGUMENT;
    struct content_section s[1 + HWC_ARRAYS];
    enum content_error error = content_sections_parse(data, bytes, "HWC1", record_bytes, 1 + HWC_ARRAYS, s);
    if (error != CONTENT_OK)
        return error;
    struct hwc_collision c;
    memset(&c, 0, sizeof(c));
    c.bsp_count = s[0].count;
    uint64_t sizes[1 + HWC_ARRAYS];
    for (uint32_t k = 0; k < 1 + HWC_ARRAYS; ++k)
        sizes[k] = s[k].bytes;
    error = content_arena_reserve(&c.arena, content_arena_size(sizes, 1 + HWC_ARRAYS));
    if (error != CONTENT_OK)
        return error;
    c.bsps = content_arena_take(&c.arena, s[0].bytes);
    content_records_decode(&bsp, s[0].data, c.bsp_count, c.bsps, sizeof(struct hwc_bsp));
    for (uint32_t k = 0; k < HWC_ARRAYS; ++k) {
        c.counts[k] = s[k + 1].count;
        void *array = content_arena_take(&c.arena, s[k + 1].bytes);
        content_records_decode(&layouts[k], s[k + 1].data, c.counts[k], array, record_bytes[k + 1]);
        set_array(&c, k, array);
    }
    error = check(&c);
    if (error != CONTENT_OK) {
        content_arena_release(&c.arena);
        return error;
    }
    *out = c;
    return CONTENT_OK;
}

void hwc_release(struct hwc_collision *collision)
{
    if (collision == NULL)
        return;
    content_arena_release(&collision->arena);
    memset(collision, 0, sizeof(*collision));
}

void hwc_digest(const struct hwc_collision *c, unsigned char digest[32])
{
    struct content_layout layout;
    struct content_sha256 sha;
    content_sha256_init(&sha);
    content_layout_init(&layout, HWC_BSP_FORMAT);
    content_records_digest(&sha, &layout, c->bsps, c->bsp_count, sizeof(struct hwc_bsp));
    const void *arrays[HWC_ARRAYS] = {c->bsp3d_nodes, c->planes, c->leaves, c->bsp2d_references,
                                      c->bsp2d_nodes, c->surfaces, c->edges, c->vertices};
    for (uint32_t k = 0; k < HWC_ARRAYS; ++k) {
        content_layout_init(&layout, array_formats[k]);
        content_records_digest(&sha, &layout, arrays[k], c->counts[k], record_bytes[k + 1]);
    }
    content_sha256_final(&sha, digest);
}
