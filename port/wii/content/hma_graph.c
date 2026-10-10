/* HMA1 model animation loader (HWI-008E). See hma_graph.h.
 *
 * Layout rules follow tools/wii/content_animation.py and
 * source/models/model_animations.c. */
#include "hma_graph.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(struct hma_record) == 80, "hma_record matches HMA_ANIMATION_FORMAT");
_Static_assert(offsetof(struct hma_record, node_list_checksum) == 16, "hma_record layout");
_Static_assert(offsetof(struct hma_record, compressed_data_offset) == 44, "hma_record layout");
_Static_assert(offsetof(struct hma_animation, record) == 0, "records decode in place");

static const uint32_t record_bytes[5] = {80, 4, 1, 1, 1};
static const uint32_t frame_info_floats[4] = {0, 2, 3, 4};

/* ---- unit runs: a frame or default record as runs of 2- and 4-byte fields ---- */

#define MAX_RUNS (2u * HMA_MAX_NODES + 2u)

struct runs {
    uint32_t count, bytes;
    unsigned char width[MAX_RUNS];
    uint16_t units[MAX_RUNS];
};

static void add_run(struct runs *r, unsigned width, unsigned units)
{
    if (r->count > 0 && r->width[r->count - 1] == width) {
        r->units[r->count - 1] = (uint16_t)(r->units[r->count - 1] + units);
    } else {
        r->width[r->count] = (unsigned char)width;
        r->units[r->count] = (uint16_t)units;
        r->count++;
    }
    r->bytes += width * units;
}

static int flag(const uint32_t words[2], uint32_t node) { return (int)((words[node / 32] >> (node % 32)) & 1u); }

/* flagged: the frame record (nodes with each flag); otherwise the defaults
 * (nodes without). Order per node: rotation 4 x s16, translation 3 x f32,
 * scale f32. */
static void build_runs(const struct hma_record *a, int flagged, struct runs *r)
{
    memset(r, 0, sizeof(*r));
    for (uint32_t node = 0; node < (uint32_t)a->node_count; ++node) {
        if (flag(a->rotation_flags, node) == flagged)
            add_run(r, 2, 4);
        if (flag(a->translation_flags, node) == flagged)
            add_run(r, 4, 3);
        if (flag(a->scale_flags, node) == flagged)
            add_run(r, 4, 1);
    }
}

static uint32_t flag_count(const uint32_t words[2], uint32_t nodes)
{
    uint32_t n = 0;
    for (uint32_t node = 0; node < nodes; ++node)
        n += (uint32_t)flag(words, node);
    return n;
}

/* Converts units of width bytes: big-endian to native (to_native) or native
 * to little-endian. */
static void convert_units(const unsigned char *in, unsigned char *out, uint32_t width, uint32_t units, int to_native)
{
    for (uint32_t u = 0; u < units; ++u, in += width, out += width) {
        if (width == 2) {
            if (to_native) {
                uint16_t v = (uint16_t)content_be16(in);
                memcpy(out, &v, 2);
            } else {
                uint16_t v;
                memcpy(&v, in, 2);
                content_put_le16(out, v);
            }
        } else if (to_native) {
            uint32_t v = content_be32(in);
            memcpy(out, &v, 4);
        } else {
            uint32_t v;
            memcpy(&v, in, 4);
            content_put_le32(out, v);
        }
    }
}

static void convert_records(const struct runs *r, const unsigned char *in, unsigned char *out, uint32_t records,
                            int to_native)
{
    for (uint32_t i = 0; i < records; ++i)
        for (uint32_t k = 0; k < r->count; ++k) {
            uint32_t bytes = (uint32_t)r->width[k] * r->units[k];
            convert_units(in, out, r->width[k], r->units[k], to_native);
            in += bytes;
            out += bytes;
        }
}

/* ---- compressed blocks ---------------------------------------------------------- */

#define MAX_SPANS 12u

struct span {
    int64_t offset;
    uint32_t width, units;
};

static uint32_t word_at(const unsigned char *p, int native)
{
    if (!native)
        return content_be32(p);
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int add_span(struct span *spans, uint32_t *n, int64_t offset, uint32_t width, uint64_t units)
{
    if (units == 0)
        return 1;
    if (units > 0xFFFFFFFFu)
        return 0;
    spans[*n].offset = offset;
    spans[*n].width = width;
    spans[*n].units = (uint32_t)units;
    ++*n;
    return 1;
}

/* The arrays of a compressed block (header and node headers read big-endian,
 * or native for a decoded block), which must tile it exactly. */
static enum content_error compressed_spans(const unsigned char *block, uint32_t bytes, int native,
                                           const struct hma_record *a, struct span *spans, uint32_t *count)
{
    if (bytes < HMA_COMPRESSED_HEADER_BYTES)
        return CONTENT_RANGE;
    int32_t header[11];
    for (int i = 0; i < 11; ++i)
        header[i] = (int32_t)word_at(block + 4 * i, native);
    uint32_t nodes = (uint32_t)a->node_count;
    uint32_t counts[3] = {flag_count(a->rotation_flags, nodes), flag_count(a->translation_flags, nodes),
                          flag_count(a->scale_flags, nodes)};
    int64_t header_at[3] = {HMA_COMPRESSED_HEADER_BYTES, header[3], header[7]};
    uint32_t keys[3] = {0, 0, 0};
    uint32_t n = 0;
    add_span(spans, &n, 0, 4, 11);
    for (int k = 0; k < 3; ++k) {
        if (counts[k] == 0)
            continue;
        if (header_at[k] < 0 || header_at[k] + 4 * (int64_t)counts[k] > (int64_t)bytes)
            return CONTENT_RANGE;
        for (uint32_t i = 0; i < counts[k]; ++i) {
            uint32_t word = word_at(block + header_at[k] + 4 * i, native);
            uint32_t first = word >> HMA_KEYFRAME_COUNT_BITS, frames = word & ((1u << HMA_KEYFRAME_COUNT_BITS) - 1u);
            if (frames != 0 && first + frames > keys[k])
                keys[k] = first + frames;
        }
        add_span(spans, &n, header_at[k], 4, counts[k]);
    }
    if (!add_span(spans, &n, header[0], 2, keys[0]) || !add_span(spans, &n, header[1], 2, 3u * (uint64_t)nodes) ||
        !add_span(spans, &n, header[2], 2, 3u * (uint64_t)keys[0]) || !add_span(spans, &n, header[4], 2, keys[1]) ||
        !add_span(spans, &n, header[5], 4, 3u * (uint64_t)nodes) ||
        !add_span(spans, &n, header[6], 4, 3u * (uint64_t)keys[1]) || !add_span(spans, &n, header[8], 2, keys[2]) ||
        !add_span(spans, &n, header[9], 4, counts[2]) || !add_span(spans, &n, header[10], 4, keys[2]))
        return CONTENT_RANGE;
    /* sort by offset (insertion; at most 12) */
    for (uint32_t i = 1; i < n; ++i)
        for (uint32_t j = i; j > 0 && spans[j].offset < spans[j - 1].offset; --j) {
            struct span t = spans[j];
            spans[j] = spans[j - 1];
            spans[j - 1] = t;
        }
    int64_t end = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (spans[i].offset != end)
            return CONTENT_RANGE;   /* overlap, gap or outside */
        end += (int64_t)spans[i].width * spans[i].units;
        if (end > (int64_t)bytes)
            return CONTENT_RANGE;
    }
    if (end != (int64_t)bytes)
        return CONTENT_RANGE;
    *count = n;
    return CONTENT_OK;
}

static void convert_block(const struct span *spans, uint32_t n, const unsigned char *in, unsigned char *out,
                          int to_native)
{
    for (uint32_t i = 0; i < n; ++i)
        convert_units(in + spans[i].offset, out + spans[i].offset, spans[i].width, spans[i].units, to_native);
}

/* ---- validation and decode ------------------------------------------------------- */

static enum content_error check_animation(const struct hma_record *a, uint32_t index, const uint32_t cursor[4],
                                         const struct content_section *s)
{
    if (a->index != index)
        return CONTENT_RANGE;
    if (a->pad != 0)
        return CONTENT_RESERVED;
    if (a->node_count < 0 || (uint32_t)a->node_count > HMA_MAX_NODES || a->frame_count < 0)
        return CONTENT_COUNT;
    if (a->frame_info_type < 0 || a->frame_info_type > 3)
        return CONTENT_ENUM;
    uint32_t nodes = (uint32_t)a->node_count;
    uint32_t expected_frame = 8u * flag_count(a->rotation_flags, nodes) + 12u * flag_count(a->translation_flags, nodes) +
                              4u * flag_count(a->scale_flags, nodes);
    if ((uint32_t)a->frame_size != expected_frame || a->frame_size < 0)
        return CONTENT_COUNT;
    const uint32_t offsets[4] = {a->frame_info_offset, a->default_offset, a->frame_offset, a->compressed_offset};
    const uint32_t sizes[4] = {a->frame_info_bytes, a->default_bytes, a->frame_bytes, a->compressed_bytes};
    for (int k = 0; k < 4; ++k)
        if (offsets[k] != cursor[k] || sizes[k] > s[k + 1].bytes - offsets[k] || offsets[k] > s[k + 1].bytes)
            return CONTENT_RANGE;
    if ((uint64_t)a->frame_info_bytes != 4u * (uint64_t)a->frame_count * frame_info_floats[a->frame_info_type])
        return CONTENT_COUNT;
    uint64_t all_frames = (uint64_t)a->frame_count * (uint32_t)a->frame_size;
    if (a->flags & HMA_COMPRESSED_BIT) {
        if ((a->frame_bytes != 0 && a->frame_bytes != all_frames) || (int64_t)a->frame_bytes != a->compressed_data_offset ||
            a->default_bytes != 0)
            return CONTENT_COUNT;
        struct span spans[MAX_SPANS];
        uint32_t n;
        return compressed_spans(s[4].data + a->compressed_offset, a->compressed_bytes, 0, a, spans, &n);
    }
    struct runs defaults;
    build_runs(a, 0, &defaults);
    if (a->frame_bytes != all_frames || a->default_bytes != defaults.bytes || a->compressed_bytes != 0)
        return CONTENT_COUNT;
    return CONTENT_OK;
}

enum content_error hma_load(const unsigned char *data, uint32_t bytes, struct hma_graph *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    struct content_layout layout;
    if (content_layout_init(&layout, HMA_ANIMATION_FORMAT) != sizeof(struct hma_record))
        return CONTENT_ARGUMENT;
    struct content_section s[5];
    enum content_error error = content_sections_parse(data, bytes, "HMA1", record_bytes, 5, s);
    if (error != CONTENT_OK)
        return error;
    struct hma_graph g;
    memset(&g, 0, sizeof(g));
    g.animation_count = s[0].count;
    uint64_t compressed_total = 0;
    for (uint32_t i = 0; i < g.animation_count; ++i)
        compressed_total += (content_be32(s[0].data + 80u * i + 76) + 3u) & ~3u;
    const uint64_t sizes[5] = {(uint64_t)g.animation_count * sizeof(struct hma_animation), s[1].bytes, s[2].bytes,
                               s[3].bytes, compressed_total};
    error = content_arena_reserve(&g.arena, content_arena_size(sizes, 5));
    if (error != CONTENT_OK)
        return error;
    g.animations = content_arena_take(&g.arena, g.animation_count * (uint32_t)sizeof(struct hma_animation));
    unsigned char *frame_info = content_arena_take(&g.arena, s[1].bytes);
    unsigned char *defaults = content_arena_take(&g.arena, s[2].bytes);
    unsigned char *frames = content_arena_take(&g.arena, s[3].bytes);
    unsigned char *compressed = content_arena_take(&g.arena, (uint32_t)compressed_total);
    content_records_decode(&layout, s[0].data, g.animation_count, g.animations, sizeof(struct hma_animation));
    uint32_t cursor[4] = {0, 0, 0, 0}, packed = 0;
    for (uint32_t i = 0; i < g.animation_count && error == CONTENT_OK; ++i) {
        struct hma_animation *a = &g.animations[i];
        const struct hma_record *r = &a->record;
        error = check_animation(r, i, cursor, s);
        if (error != CONTENT_OK)
            break;
        cursor[0] += r->frame_info_bytes;
        cursor[1] += r->default_bytes;
        cursor[2] += r->frame_bytes;
        cursor[3] += r->compressed_bytes;
        a->frame_info = (const float *)(const void *)(frame_info + r->frame_info_offset);
        convert_units(s[1].data + r->frame_info_offset, frame_info + r->frame_info_offset, 4, r->frame_info_bytes / 4,
                      1);
        struct runs runs;
        build_runs(r, 0, &runs);
        a->defaults = defaults + r->default_offset;
        if (r->default_bytes != 0)
            convert_records(&runs, s[2].data + r->default_offset, defaults + r->default_offset, 1, 1);
        build_runs(r, 1, &runs);
        a->frames = frames + r->frame_offset;
        if (r->frame_bytes != 0)
            convert_records(&runs, s[3].data + r->frame_offset, frames + r->frame_offset,
                            r->frame_bytes / (uint32_t)r->frame_size, 1);
        a->compressed = NULL;
        if (r->flags & HMA_COMPRESSED_BIT) {
            struct span spans[MAX_SPANS];
            uint32_t n = 0;
            compressed_spans(s[4].data + r->compressed_offset, r->compressed_bytes, 0, r, spans, &n);
            convert_block(spans, n, s[4].data + r->compressed_offset, compressed + packed, 1);
            a->compressed = compressed + packed;
            packed += (r->compressed_bytes + 3u) & ~3u;
            g.compressed_count++;
            if (r->frame_bytes == 0)
                g.expanded_bytes += (uint64_t)r->frame_count * (uint32_t)r->frame_size;
        }
    }
    if (error == CONTENT_OK && (cursor[0] != s[1].bytes || cursor[1] != s[2].bytes || cursor[2] != s[3].bytes ||
                                cursor[3] != s[4].bytes))
        error = CONTENT_RANGE;
    if (error != CONTENT_OK) {
        content_arena_release(&g.arena);
        return error;
    }
    *out = g;
    return CONTENT_OK;
}

void hma_release(struct hma_graph *graph)
{
    if (graph == NULL)
        return;
    content_arena_release(&graph->arena);
    memset(graph, 0, sizeof(*graph));
}

static void digest_units(struct content_sha256 *sha, const unsigned char *in, uint32_t width, uint32_t units)
{
    unsigned char buffer[512];
    while (units > 0) {
        uint32_t n = units < sizeof(buffer) / width ? units : (uint32_t)(sizeof(buffer) / width);
        convert_units(in, buffer, width, n, 0);
        content_sha256_update(sha, buffer, n * width);
        in += n * width;
        units -= n;
    }
}

void hma_digest(const struct hma_graph *g, unsigned char digest[32])
{
    struct content_layout layout;
    content_layout_init(&layout, HMA_ANIMATION_FORMAT);
    struct content_sha256 sha;
    content_sha256_init(&sha);
    for (uint32_t i = 0; i < g->animation_count; ++i) {
        const struct hma_animation *a = &g->animations[i];
        const struct hma_record *r = &a->record;
        content_records_digest(&sha, &layout, r, 1, sizeof(*r));
        digest_units(&sha, (const unsigned char *)a->frame_info, 4, r->frame_info_bytes / 4);
        struct runs runs;
        build_runs(r, 0, &runs);
        for (uint32_t k = 0, at = 0; r->default_bytes != 0 && k < runs.count; at += runs.width[k] * runs.units[k], ++k)
            digest_units(&sha, a->defaults + at, runs.width[k], runs.units[k]);
        build_runs(r, 1, &runs);
        uint32_t frame_count = r->frame_size ? r->frame_bytes / (uint32_t)r->frame_size : 0;
        for (uint32_t f = 0; f < frame_count; ++f)
            for (uint32_t k = 0, at = f * (uint32_t)r->frame_size; k < runs.count;
                 at += runs.width[k] * runs.units[k], ++k)
                digest_units(&sha, a->frames + at, runs.width[k], runs.units[k]);
        if (a->compressed != NULL) {
            struct span spans[MAX_SPANS];
            uint32_t n = 0;
            compressed_spans(a->compressed, r->compressed_bytes, 1, r, spans, &n);
            for (uint32_t k = 0; k < n; ++k)   /* spans tile the block in offset order */
                digest_units(&sha, a->compressed + spans[k].offset, spans[k].width, spans[k].units);
        }
    }
    content_sha256_final(&sha, digest);
}
