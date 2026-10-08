#include "cache_material_fixture.h"
#include "cache_material_probe.h"
#include <stdlib.h>
#include <string.h>

enum { GUARD = 32, TAG_BYTES = 4096, SLOT_BYTES = 65536, BSP_AT = 8192,
       BSP_BYTES = 57344, ROOT_AT = BSP_AT + 24, LM_AT = BSP_AT + 800,
       MAT_AT = BSP_AT + 2048, VERTEX_TABLE = BSP_AT + 720,
       INDEX_TABLE = BSP_AT + 744, DATA_AT = BSP_AT + 8192,
       WORK_BYTES = 4096, SERIAL_BYTES = 4096 };
#define BASE CACHE_BSP_TAG_BASE
#define SCNR UINT32_C(0x73636e72)
#define SBSP UINT32_C(0x73627370)
#define SENV UINT32_C(0x73656e76)
#define SHDR UINT32_C(0x73686472)
#define SCENARIO_DATUM UINT32_C(0x12340000)
#define BSP_DATUM UINT32_C(0x23450001)
#define SHADER_DATUM UINT32_C(0xabcd0002)

struct context {
    FILE *report;
    const char *name;
    unsigned cases, checks, failures;
    int collect;
};
struct storage {
    unsigned char *placements[2], *snapshot, *work, *work_before, *serial, *golden;
    struct cache_arena_owner owner;
    struct cache_arena_handle handle;
};

static int check(struct context *context, int okay, const char *assertion)
{
    ++context->checks;
    if (okay)
        return 1;
    ++context->failures;
    if (fprintf(context->report, "CACHE_MATERIAL FAIL case=%s assertion=%s check=%u\n",
                context->name, assertion, context->checks) < 0 || fflush(context->report))
        return 0;
    return context->collect;
}
#define CHECK(value, label) do { if (!check(context, (value), (label))) return 1; } while (0)

static int abort_case(struct context *context, const char *reason)
{
    (void)fprintf(context->report, "CACHE_MATERIAL ABORT case=%s reason=%s\n", context->name, reason);
    (void)fflush(context->report);
    return 1;
}

static int all_bytes(const unsigned char *p, size_t size, unsigned char value)
{
    for (size_t i = 0; i < size; ++i)
        if (p[i] != value)
            return 0;
    return 1;
}

static void word(unsigned char *p, size_t at, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[at + i] = (unsigned char)(v >> (i * 8));
}

static void half(unsigned char *p, size_t at, uint16_t v)
{
    p[at] = (unsigned char)v;
    p[at + 1] = (unsigned char)(v >> 8);
}

static void author_material(unsigned char *p, size_t at, unsigned index)
{
    static const uint32_t bits[] = {UINT32_C(0x80000000), UINT32_C(0x00000001),
        UINT32_C(0x7f800000), UINT32_C(0x7fc12345), UINT32_C(0xff800000), UINT32_C(0x3f812345)};
    for (unsigned i = 0; i < 256; ++i)
        p[at + i] = (unsigned char)(i * 31 + index * 17 + 9);
    word(p, at, SENV);
    word(p, at + 4, BASE + 224);
    word(p, at + 8, UINT32_C(0x80000001));
    word(p, at + 12, SHADER_DATUM);
    half(p, at + 16, (uint16_t)(-7 - (int)index));
    half(p, at + 18, (uint16_t)(0xa5f0u + index));
    word(p, at + 20, index * 3);
    word(p, at + 24, 3);
    for (unsigned i = 0; i < 3; ++i)
        word(p, at + 28 + i * 4, bits[(index * 3 + i) % 6]);
    for (unsigned kind = 0; kind < 2; ++kind) {
        size_t b = at + 176 + kind * 20;
        half(p, b, (uint16_t)(kind ? 3 : 1));
        half(p, b + 2, (uint16_t)(0xb123u + index + kind));
        word(p, b + 4, kind ? 2 : 3);
        word(p, b + 8, UINT32_C(0xfffffff9) - index - kind);
        word(p, b + 12, UINT32_C(0x02468ace) + index + kind);
        word(p, b + 16, BASE + (kind ? INDEX_TABLE : VERTEX_TABLE) + index * 12);
        b = at + 216 + kind * 20;
        word(p, b, kind ? 113 : 0);
        word(p, b + 4, UINT32_C(0x81234567) + index + kind);
        word(p, b + 8, UINT32_C(0x89abcdef) + index + kind);
        word(p, b + 12, kind ? BASE + DATA_AT + index * 256 : UINT32_MAX);
        word(p, b + 16, UINT32_C(0xfedcba98) + index + kind);
    }
}

static void author(unsigned char *p, size_t slot_bytes)
{
    memset(p, 0x6b, slot_bytes);
    const uint32_t tag_header[] = {BASE + 36, SCENARIO_DATUM, UINT32_C(0x12345678), 3,
        0, UINT32_MAX, 0, UINT32_MAX, UINT32_C(0x74616773)};
    for (unsigned i = 0; i < 9; ++i)
        word(p, i * 4, tag_header[i]);
    const uint32_t groups[] = {SCNR, SBSP, SENV};
    const uint32_t datums[] = {SCENARIO_DATUM, BSP_DATUM, SHADER_DATUM};
    for (unsigned i = 0; i < 3; ++i) {
        size_t at = 36 + i * 32;
        word(p, at, groups[i]);
        word(p, at + 4, i == 2 ? SHDR : UINT32_MAX);
        word(p, at + 8, UINT32_MAX);
        word(p, at + 12, datums[i]);
        word(p, at + 16, BASE + 208 + i * 8);
        word(p, at + 20, i == 0 ? BASE + 512 : i == 1 ? 0 : BASE + 3000);
        word(p, at + 24, UINT32_C(0xdeadbeef));
        word(p, at + 28, UINT32_C(0xcafebabe));
    }
    memcpy(p + 208, "scene", 6);
    memcpy(p + 216, "bsp", 4);
    memcpy(p + 224, "shader", 7);
    word(p, 512 + 0x5a4, 1);
    word(p, 512 + 0x5a8, BASE + 2048);
    word(p, 512 + 0x5ac, UINT32_C(0x87654321));
    const uint32_t reference[] = {4096, (uint32_t)(slot_bytes - BSP_AT), BASE + BSP_AT,
        UINT32_C(0x98765432), SBSP, BASE + 216, 0, BSP_DATUM};
    for (unsigned i = 0; i < 8; ++i)
        word(p, 2048 + i * 4, reference[i]);
    const uint32_t bsp_header[] = {BASE + ROOT_AT, 2, BASE + VERTEX_TABLE,
        2, BASE + INDEX_TABLE, SBSP};
    for (unsigned i = 0; i < 6; ++i)
        word(p, BSP_AT + i * 4, bsp_header[i]);
    for (unsigned kind = 0; kind < 2; ++kind)
        for (unsigned i = 0; i < 2; ++i) {
            size_t at = (kind ? INDEX_TABLE : VERTEX_TABLE) + i * 12;
            word(p, at, UINT32_C(0xd1234567) + i + kind);
            word(p, at + 4, BASE + DATA_AT + i * 256);
            word(p, at + 8, UINT32_C(0xe2345678) + i + kind);
        }
    word(p, ROOT_AT + 248, 12);
    word(p, ROOT_AT + 252, BASE + BSP_AT + 1600);
    word(p, ROOT_AT + 260, 2);
    word(p, ROOT_AT + 264, BASE + LM_AT);
    word(p, ROOT_AT + 268, UINT32_C(0xfeedbabe));
    for (unsigned i = 0; i < 2; ++i) {
        memset(p + LM_AT + i * 32, 0xa5 + (int)i, 32);
        half(p, LM_AT + i * 32, i ? UINT16_MAX : 7);
        half(p, LM_AT + i * 32 + 2, (uint16_t)(0x9123u + i));
        word(p, LM_AT + i * 32 + 20, 1);
        word(p, LM_AT + i * 32 + 24, BASE + MAT_AT + i * 256);
        word(p, LM_AT + i * 32 + 28, UINT32_C(0xf1234567) + i);
        author_material(p, MAT_AT + i * 256, i);
    }
}

static int prepare(struct storage *storage, unsigned placement, unsigned offset, unsigned char **bytes,
                    struct cache_bsp_control *bsp, struct cache_bsp_view *view)
{
    struct cache_arena_result arena;
    struct cache_arena_plan plan;
    struct cache_bsp_result result;
    const struct cache_arena_request request = {SLOT_BYTES, 1};
    if (!cache_arena_owner_release(&storage->owner, &arena))
        return 0;
    memset(storage->placements[placement], 0xa7, SLOT_BYTES + GUARD * 2 + 8);
    *bytes = storage->placements[placement] + GUARD + offset;
    author(*bytes, SLOT_BYTES);
    memset(bsp, 0, sizeof(*bsp));
    return cache_arena_plan_build(&request, 1, *bytes, SLOT_BYTES, 0, &plan, &arena) &&
        cache_arena_owner_bind(&storage->owner, &plan, *bytes, SLOT_BYTES, &arena) &&
        cache_arena_owner_handle(&storage->owner, 0, &storage->handle, &arena) &&
        cache_bsp_bind(bsp, &storage->owner, &storage->handle, TAG_BYTES, 1048576, 0, view, &result);
}

static size_t golden(unsigned char *output, const unsigned char *source, unsigned lc, unsigned mc)
{
    memcpy(output, source + ROOT_AT, 648);
    memcpy(output + 648, source + LM_AT, lc * 32);
    memcpy(output + 648 + lc * 32, source + MAT_AT, mc * 256);
    return 648 + lc * 32 + mc * 256;
}

static int valid_case(struct context *context, struct storage *storage, unsigned placement, unsigned offset)
{
    context->name = "authored_offsets_placements_roundtrip";
    ++context->cases;
    unsigned char *bytes;
    struct cache_bsp_control bsp;
    struct cache_bsp_view parent;
    if (!prepare(storage, placement, offset, &bytes, &bsp, &parent))
        return abort_case(context, "prepare");
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    size_t expected = golden(storage->golden, bytes, 2, 2);
    memset(storage->work, 0xa7, WORK_BYTES + GUARD * 2);
    memset(storage->serial, 0xa7, SERIAL_BYTES + GUARD * 2 + 8);
    struct cache_material_control control = {0};
    struct cache_material_view view = {0}, stale;
    struct cache_material_requirements req;
    struct cache_material_result result;
    int measured = cache_material_measure(&parent, &req, &result);
    CHECK(measured, "measure");
    if (!measured)
        return abort_case(context, "measure_failed");
    CHECK(req.lightmap_count == 2 && req.material_count == 2 && req.workspace_bytes == 640 &&
          req.serialized_bytes == 1224, "independent_numeric_requirements");
    int bound = cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result);
    CHECK(bound, "bind");
    if (!bound)
        return abort_case(context, "bind_failed");
    struct cache_material_root_projection root;
    CHECK(cache_material_get_root(&view, &root, &result) && root.source.offset == ROOT_AT && root.source.bytes == 648 &&
          root.lightmaps.offset == LM_AT && root.lightmaps.bytes == 64 && root.material_count == 2 && root.lightmap_count == 2,
          "root_fields");
    for (unsigned i = 0; i < 2; ++i) {
        struct cache_material_lightmap_projection lm;
        struct cache_material_projection m = {0};
        CHECK(cache_material_get_lightmap(&view, i, &lm, &result) && lm.bitmap_index == (i ? -1 : 7) &&
              lm.pad == 0x9123u + i && lm.first_material == i && lm.material_count == 1 &&
              lm.source.offset == LM_AT + i * 32 && lm.source.bytes == 32, "lightmap_every_field");
        int obtained = cache_material_get_material(&view, i, &m, &result);
        CHECK(obtained, "material_get");
        if (!obtained)
            return abort_case(context, "material_get_failed");
        CHECK(m.shader_words[0] == SENV && m.shader_words[1] == BASE + 224 &&
              m.shader_words[2] == UINT32_C(0x80000001) && m.shader_words[3] == SHADER_DATUM &&
              m.permutation == -7 - (int)i && m.flags == 0xa5f0u + i && m.first_surface == (int32_t)(i * 3) &&
              m.surface_count == 3 && m.source.offset == MAT_AT + i * 256 && m.source.bytes == 256, "material_scalar_reference_fields");
        static const uint32_t bits[] = {UINT32_C(0x80000000), 1, UINT32_C(0x7f800000),
            UINT32_C(0x7fc12345), UINT32_C(0xff800000), UINT32_C(0x3f812345)};
        for (unsigned j = 0; j < 3; ++j)
            CHECK(m.centroid_bits[j] == bits[i * 3 + j], "nonzero_float_bits_no_native_float_cast");
        for (unsigned kind = 0; kind < 2; ++kind) {
            const struct cache_material_vertex_metadata *v = &m.vertex_buffers[kind];
            const struct cache_material_data_metadata *d = &m.data_fields[kind];
            CHECK(v->type == (kind ? 3 : 1) && v->pad == 0xb123u + i + kind && v->count == (kind ? 2 : 3) &&
                  v->offset == -7 - (int)i - (int)kind && v->base_address == UINT32_C(0x02468ace) + i + kind &&
                  v->hardware_format == BASE + (kind ? INDEX_TABLE : VERTEX_TABLE) + i * 12, "vertex_all_numeric_words");
            CHECK(d->size == (kind ? 113 : 0) && d->pad == UINT32_C(0x81234567) + i + kind &&
                  (uint32_t)d->file_offset == UINT32_C(0x89abcdef) + i + kind &&
                  d->address == (kind ? BASE + DATA_AT + i * 256 : UINT32_MAX) &&
                  d->definition == UINT32_C(0xfedcba98) + i + kind, "tagdata_all_numeric_words");
        }
    }
    size_t used = 99;
    CHECK(cache_material_serialize(&view, storage->serial + GUARD + offset, expected, &used, &result) && used == expected &&
          !memcmp(storage->serial + GUARD + offset, storage->golden, expected), "selected_order_independent_raw_golden_and_numeric_rewrite");
    for (unsigned i = 0; i < GUARD + offset; ++i)
        CHECK(storage->serial[i] == 0xa7, "serial_prefix_canary");
    for (size_t i = GUARD + offset + expected; i < SERIAL_BYTES + GUARD * 2 + 8; ++i)
        CHECK(storage->serial[i] == 0xa7, "serial_suffix_canary");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES), "full_source_immutable");
    for (unsigned i = 0; i < GUARD; ++i)
        CHECK(storage->work[i] == 0xa7 && storage->work[GUARD + WORK_BYTES + i] == 0xa7, "workspace_guards");
    stale = view;
    uint32_t old_offset = control.published.offset;
    CHECK(cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result) &&
          control.published.offset != old_offset && control.generation == 2, "transactional_alternate_half_publish");
    CHECK(!cache_material_get_root(&stale, &root, &result) && result.error == CACHE_MATERIAL_STATE, "stale_child_after_rebind");
    CHECK(cache_material_unload(&control, &result), "unload");
    CHECK(!cache_material_get_root(&view, &root, &result) && result.error == CACHE_MATERIAL_STATE, "stale_child_after_unload");
    uint64_t generation = control.generation;
    CHECK(cache_material_unload(&control, &result) && control.generation == generation, "inactive_idempotent");
    return 0;
}

struct mutation { const char *name; size_t at; uint32_t value; unsigned width; enum cache_material_error error; };
static int invalid_case(struct context *context, struct storage *storage, const struct mutation *mutation)
{
    context->name = mutation->name;
    ++context->cases;
    unsigned char *bytes;
    struct cache_bsp_control bsp;
    struct cache_bsp_view parent;
    if (!prepare(storage, 0, 3, &bytes, &bsp, &parent))
        return abort_case(context, "prepare");
    struct cache_material_control control = {0};
    struct cache_material_view view = {0};
    struct cache_material_result result;
    if (!cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result))
        return abort_case(context, "baseline_bind");
    struct cache_material_control before = control;
    struct cache_material_view view_before = view;
    memcpy(storage->work_before, storage->work, WORK_BYTES + GUARD * 2);
    /* Fault injection occurs before attempted re-acquisition; old views are
     * never used while source is altered. They resume only after restoration. */
    if (mutation->width == 2)
        half(bytes, mutation->at, (uint16_t)mutation->value);
    else
        word(bytes, mutation->at, mutation->value);
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result), "reject_malformed");
    CHECK(result.error == mutation->error, "precise_reason");
    CHECK(!memcmp(&control, &before, sizeof(control)) && !memcmp(&view, &view_before, sizeof(view)), "publication_controls_atomic");
    CHECK(!memcmp(storage->work, storage->work_before, WORK_BYTES + GUARD * 2), "prior_publication_and_scratch_unchanged_preflight_failure");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES), "malformed_source_immutable");
    struct cache_material_requirements req, req_before;
    memset(&req, 0x5a, sizeof(req));
    req_before = req;
    CHECK(!cache_material_measure(&parent, &req, &result) && !memcmp(&req, &req_before, sizeof(req)), "measure_output_atomic");
    return 0;
}

static int edges(struct context *context, struct storage *storage)
{
    context->name = "capacity_alias_empty_none_types_and_generation";
    ++context->cases;
    unsigned char *bytes;
    struct cache_bsp_control bsp;
    struct cache_bsp_view parent;
    if (!prepare(storage, 0, 0, &bytes, &bsp, &parent))
        return abort_case(context, "prepare");
    struct cache_material_control control = {0};
    struct cache_material_view view = {0};
    struct cache_material_result result;
    struct cache_material_requirements req;
    struct cache_material_projection m;
    struct cache_material_root_projection root;
    memset(storage->work, 0xa7, WORK_BYTES + GUARD * 2);
    memcpy(storage->work_before, storage->work, WORK_BYTES + GUARD * 2);
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD, 639, &view, &result) &&
          result.error == CACHE_MATERIAL_WORKSPACE && result.required == 640, "workspace_one_short_no_publication");
    CHECK(!memcmp(storage->work, storage->work_before, WORK_BYTES + GUARD * 2) && !control.live, "capacity_atomic");
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD + 1, WORK_BYTES, &view, &result) &&
          result.error == CACHE_MATERIAL_WORKSPACE, "workspace_unaligned_reject");
    CHECK(!cache_material_bind(&control, &parent, bytes, WORK_BYTES, &view, &result) && result.error == CACHE_MATERIAL_OVERLAP,
          "workspace_source_alias");
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD, SIZE_MAX, &view, &result) &&
          result.error == CACHE_MATERIAL_OVERFLOW, "workspace_address_wrap");
    CHECK(!cache_material_bind(&control, &parent, &control, sizeof(control), &view, &result) &&
          result.error == CACHE_MATERIAL_OVERLAP, "workspace_control_alias");
    CHECK(!cache_material_bind(&control, &parent, &parent, sizeof(parent), &view, &result) &&
          result.error == CACHE_MATERIAL_OVERLAP, "workspace_borrowed_parent_view_alias");
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES,
          (struct cache_material_view *)(void *)&control, &result) && result.error == CACHE_MATERIAL_OVERLAP, "control_view_alias");
    for (unsigned i = 0; i < 2; ++i) {
        word(bytes, MAT_AT + i * 256, UINT32_C(0xdeadbeef));
        word(bytes, MAT_AT + i * 256 + 4, UINT32_MAX);
        word(bytes, MAT_AT + i * 256 + 12, UINT32_MAX);
        word(bytes, MAT_AT + i * 256 + 192, 0);
        word(bytes, MAT_AT + i * 256 + 212, 0);
    }
    half(bytes, MAT_AT + 176, 0);
    half(bytes, MAT_AT + 196, 2);
    word(bytes, MAT_AT + 20, 1);
    word(bytes, MAT_AT + 256 + 20, 0);
    int bound = cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result);
    CHECK(bound, "NONE_non_group_non_name_and_null_hardware_accepted_with_uncompressed_types");
    if (!bound)
        return abort_case(context, "edge_bind");
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    memcpy(storage->work_before, storage->work, WORK_BYTES + GUARD * 2);
    struct cache_material_control live_before = control;
    struct cache_material_view live_view_before = view;
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD, 639, &view, &result) &&
          result.error == CACHE_MATERIAL_WORKSPACE && !memcmp(&control, &live_before, sizeof(control)) &&
          !memcmp(&view, &live_view_before, sizeof(view)) &&
          !memcmp(storage->work, storage->work_before, WORK_BYTES + GUARD * 2), "live_publication_capacity_failure_atomic");
    CHECK(cache_material_get_material(&view, 1, &m, &result) && m.first_surface == 0,
          "in_bounds_overlapping_out_of_order_surface_ranges_allowed");
    CHECK(cache_material_get_material(&view, 0, &m, &result) && m.shader_words[0] == UINT32_C(0xdeadbeef) &&
          m.shader_words[1] == UINT32_MAX && m.shader_words[3] == UINT32_MAX &&
          m.vertex_buffers[0].type == 0 && m.vertex_buffers[1].type == 2, "NONE_opaque_words_and_types_retained");
    size_t used = 71;
    memset(storage->serial, 0x5a, SERIAL_BYTES + GUARD * 2 + 8);
    CHECK(!cache_material_serialize(&view, storage->serial, 1223, &used, &result) &&
          result.error == CACHE_MATERIAL_CAPACITY && used == 71 && storage->serial[0] == 0x5a, "serializer_one_short_atomic");
    CHECK(!cache_material_serialize(&view, bytes + SLOT_BYTES - 1224, 1224, &used, &result) &&
          result.error == CACHE_MATERIAL_OVERLAP && used == 71, "serializer_full_TAG_reservation_overlap");
    CHECK(!cache_material_serialize(&view, storage->work + GUARD + 640, 1224, &used, &result) &&
          result.error == CACHE_MATERIAL_OVERLAP, "serializer_unused_workspace_overlap");
    CHECK(!cache_material_serialize(&view, &control, sizeof(control), &used, &result) &&
          result.error == CACHE_MATERIAL_OVERLAP, "serializer_control_overlap");
    CHECK(!cache_material_serialize(&view, storage->serial, SIZE_MAX, &used, &result) &&
          result.error == CACHE_MATERIAL_OVERFLOW && used == 71, "serializer_pointer_capacity_wrap");
    CHECK(!cache_material_get_material(&view, 2, &m, &result) && result.error == CACHE_MATERIAL_COUNT, "material_index_onepast");
    struct cache_material_lightmap_projection lm;
    CHECK(!cache_material_get_lightmap(&view, 2, &lm, &result) && result.error == CACHE_MATERIAL_COUNT, "lightmap_index_onepast");
    CHECK(!cache_material_get_root(&view, (struct cache_material_root_projection *)(void *)bytes, &result) &&
          result.error == CACHE_MATERIAL_OVERLAP, "getter_source_alias_reject");
    CHECK(!cache_material_get_material(&view, 0, (struct cache_material_projection *)(void *)(storage->work + GUARD), &result) &&
          result.error == CACHE_MATERIAL_OVERLAP, "getter_published_workspace_alias_reject");
    CHECK(all_bytes(storage->serial, SERIAL_BYTES + GUARD * 2 + 8, 0x5a), "full_serial_object_unchanged_on_all_failures");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES) &&
          !memcmp(storage->work, storage->work_before, WORK_BYTES + GUARD * 2), "full_source_and_publication_unchanged_on_alias_failures");
    struct cache_material_control before = control;
    control.generation = UINT64_MAX;
    before = control;
    CHECK(!cache_material_unload(&control, &result) && result.error == CACHE_MATERIAL_GENERATION &&
          !memcmp(&control, &before, sizeof(control)), "active_epoch_exhaustion_retains_ownership");
    CHECK(!cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result) &&
          result.error == CACHE_MATERIAL_GENERATION && !memcmp(&control, &before, sizeof(control)), "bind_epoch_exhaustion_atomic");
    memset(&control, 0, sizeof(control));
    for (unsigned i = 0; i < 2; ++i) {
        word(bytes, LM_AT + i * 32 + 20, 0);
        word(bytes, LM_AT + i * 32 + 24, UINT32_MAX);
    }
    CHECK(cache_material_measure(&parent, &req, &result) && req.material_count == 0 && req.lightmap_count == 2 &&
          req.workspace_bytes == 128 && req.serialized_bytes == 712, "empty_material_blocks_ignore_addresses");
    word(bytes, ROOT_AT + 260, 0);
    word(bytes, ROOT_AT + 264, UINT32_MAX);
    word(bytes, ROOT_AT + 248, UINT32_MAX);
    CHECK(!cache_material_measure(&parent, &req, &result) && result.error == CACHE_MATERIAL_COUNT,
          "empty_lightmaps_still_reject_negative_surface_count");
    word(bytes, ROOT_AT + 248, 131073);
    CHECK(!cache_material_measure(&parent, &req, &result) && result.error == CACHE_MATERIAL_COUNT,
          "empty_lightmaps_still_reject_overmaximum_surface_count");
    word(bytes, ROOT_AT + 248, 1);
    word(bytes, ROOT_AT + 252, BASE + TAG_BYTES);
    CHECK(!cache_material_measure(&parent, &req, &result) && result.error == CACHE_MATERIAL_SPAN,
          "empty_lightmaps_still_reject_bad_positive_surface_span");
    word(bytes, ROOT_AT + 248, 0);
    word(bytes, ROOT_AT + 252, UINT32_MAX);
    CHECK(cache_material_measure(&parent, &req, &result) && req.material_count == 0 && req.lightmap_count == 0 &&
          req.workspace_bytes == 48 && req.serialized_bytes == 648, "empty_root_does_not_follow_empty_block_addresses");
    CHECK(cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result) &&
          cache_material_get_root(&view, &root, &result) && !root.lightmaps.offset && !root.lightmaps.bytes, "empty_root_projection");
    CHECK(cache_material_serialize(&view, storage->serial, 648, &used, &result) && used == 648 &&
          !memcmp(storage->serial, bytes + ROOT_AT, 648), "empty_root_retains_unused_address");
    CHECK(cache_material_unload(&control, &result), "empty_unload");
    CHECK(cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result), "empty_rebind");
    struct cache_bsp_result bsp_result;
    CHECK(cache_bsp_unload(&bsp, &bsp_result) && storage->owner.live, "BSP_parent_unload_keeps_outer_owner_live");
    memset(&control, 0x9f, sizeof(control));
    CHECK(!cache_material_get_root(&view, &root, &result) && result.error == CACHE_MATERIAL_STATE,
          "BSP_parent_epoch_rejected_before_destroyed_material_control");
    return 0;
}

static int arena_backing(struct context *context, struct storage *storage)
{
    context->name = "children_in_separate_parent_arena_slot";
    ++context->cases;
    const size_t child_bytes = sizeof(struct cache_bsp_control) + sizeof(struct cache_material_control);
    const size_t arena_bytes = SLOT_BYTES + child_bytes + 64;
    unsigned char *backing = malloc(arena_bytes);
    if (!backing)
        return abort_case(context, "arena_child_allocation");
    int failed = 0;
    struct cache_arena_owner owner = {0};
    struct cache_arena_plan plan;
    struct cache_arena_result arena;
    struct cache_arena_handle tag;
    const struct cache_arena_request requests[] = {{SLOT_BYTES, 1}, {child_bytes, 8}};
    author(backing, SLOT_BYTES);
#define BACKING_CHECK(value, label) do { if (!check(context, (value), (label))) { failed = 1; goto cleanup; } } while (0)
    int prepared = cache_arena_plan_build(requests, 2, backing, arena_bytes, 0, &plan, &arena) &&
        cache_arena_owner_bind(&owner, &plan, backing, arena_bytes, &arena) &&
        cache_arena_owner_handle(&owner, 0, &tag, &arena);
    BACKING_CHECK(prepared, "two_parent_slots_bind");
    if (!prepared) { failed = abort_case(context, "two_slots_prepare"); goto cleanup; }
    struct cache_bsp_control *bsp = (void *)(backing + plan.slots[1].offset);
    struct cache_material_control *control = (void *)((unsigned char *)bsp + sizeof(*bsp));
    memset(bsp, 0, child_bytes);
    struct cache_bsp_view parent;
    struct cache_bsp_result bsp_result;
    prepared = cache_bsp_bind(bsp, &owner, &tag, TAG_BYTES, 1048576, 0, &parent, &bsp_result);
    BACKING_CHECK(prepared, "BSP_control_in_other_parent_slot");
    if (!prepared) { failed = abort_case(context, "arena_BSP_bind"); goto cleanup; }
    struct cache_material_view view;
    struct cache_material_result result;
    prepared = cache_material_bind(control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result);
    BACKING_CHECK(prepared, "material_control_in_other_parent_slot");
    if (!prepared) { failed = abort_case(context, "arena_material_bind"); goto cleanup; }
    struct cache_material_root_projection root;
    BACKING_CHECK(cache_material_get_root(&view, &root, &result), "live_arena_children_query");
    BACKING_CHECK(cache_arena_owner_release(&owner, &arena), "release_arena_before_overwrite");
    memset(backing, 0xae, arena_bytes);
    BACKING_CHECK(!cache_material_get_root(&view, &root, &result) && result.error == CACHE_MATERIAL_STATE,
                  "overwritten_entire_parent_arena_rejected_before_both_child_controls");
    BACKING_CHECK(cache_arena_owner_bind(&owner, &plan, backing, arena_bytes, &arena), "same_address_arena_rebind");
    BACKING_CHECK(!cache_material_get_root(&view, &root, &result) && result.error == CACHE_MATERIAL_STATE,
                  "same_address_rebound_arena_old_handle_rejects_before_children");
cleanup:
    if (owner.live && !cache_arena_owner_release(&owner, &arena))
        failed = abort_case(context, "arena_cleanup_release");
    free(backing);
    return failed;
#undef BACKING_CHECK
}

static int lifecycle(struct context *context, struct storage *storage)
{
    context->name = "parent_first_stale_backing_and_repeated_lifetime";
    ++context->cases;
    struct cache_material_result result;
    struct cache_arena_result arena;
    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        unsigned char *bytes;
        struct cache_bsp_control bsp;
        struct cache_bsp_view parent;
        if (!prepare(storage, cycle & 1u, cycle & 7u, &bytes, &bsp, &parent))
            return abort_case(context, "cycle_prepare");
        struct cache_material_control control = {0};
        struct cache_material_view view = {0};
        if (!cache_material_bind(&control, &parent, storage->work + GUARD, WORK_BYTES, &view, &result))
            return abort_case(context, "cycle_bind");
        size_t used;
        CHECK(cache_material_serialize(&view, storage->serial, SERIAL_BYTES, &used, &result) && used == 1224,
              "cycle_serialize");
        struct cache_material_root_projection root;
        memset(&root, 0x5a, sizeof(root));
        struct cache_material_root_projection before = root;
        CHECK(cache_arena_owner_release(&storage->owner, &arena), "outer_release");
        /* Deliberately destroy the now-inactive child and published workspace.
         * Stale-view queries must reject on the copied parent before either. */
        memset(&control, 0x8f, sizeof(control));
        memset(&bsp, 0x9e, sizeof(bsp));
        memset(storage->work, 0x7d, WORK_BYTES + GUARD * 2);
        CHECK(!cache_material_get_root(&view, &root, &result) && result.error == CACHE_MATERIAL_STATE &&
              !memcmp(&root, &before, sizeof(root)), "outer_release_reject_before_destroyed_child_workspace");
        used = 77;
        CHECK(!cache_material_serialize(&view, storage->serial, SERIAL_BYTES, &used, &result) &&
              result.error == CACHE_MATERIAL_STATE && used == 77, "stale_serialize_unchanged_used");
        if (!prepare(storage, (cycle + 1) & 1u, (cycle + 1) & 7u, &bytes, &bsp, &parent))
            return abort_case(context, "alternate_prepare");
        CHECK(!cache_material_get_root(&view, &root, &result) && result.error == CACHE_MATERIAL_STATE,
              "alternate_owner_placement_generation_rejects_old_view");
    }
    return 0;
}

static int limits(struct context *context, struct storage *storage)
{
    context->name = "source_maximum_lightmaps_materials_and_vertex_counts";
    ++context->cases;
    unsigned char *bytes;
    struct cache_bsp_control bsp;
    struct cache_bsp_view parent;
    if (!prepare(storage, 0, 0, &bytes, &bsp, &parent))
        return abort_case(context, "limits_prepare");
    word(bytes, ROOT_AT + 260, 128);
    for (unsigned i = 0; i < 128; ++i) {
        half(bytes, LM_AT + i * 32, UINT16_MAX);
        word(bytes, LM_AT + i * 32 + 20, 0);
        word(bytes, LM_AT + i * 32 + 24, UINT32_MAX);
    }
    struct cache_material_requirements req;
    struct cache_material_result result;
    CHECK(cache_material_measure(&parent, &req, &result) && req.lightmap_count == 128 && !req.material_count &&
          req.workspace_bytes == 5168 && req.serialized_bytes == 4744, "all128_lightmaps_no_capacity_cut");
    word(bytes, ROOT_AT + 260, 129);
    CHECK(!cache_material_measure(&parent, &req, &result) && result.error == CACHE_MATERIAL_COUNT, "lightmap129_reject");

    const size_t large_bytes = 4u * 1048576u;
    unsigned char *large = malloc(large_bytes);
    unsigned char *workspace = NULL;
    unsigned char *serialized = NULL;
    int failed = 0;
    if (!large)
        return abort_case(context, "large_allocation");
#define LIMIT_CHECK(value, label) do { if (!check(context, (value), (label))) { failed = 1; goto cleanup; } } while (0)
    author(large, large_bytes);
    word(large, ROOT_AT + 260, 1);
    word(large, LM_AT + 20, 2048);
    word(large, LM_AT + 24, BASE + MAT_AT);
    for (unsigned i = 0; i < 2048; ++i) {
        author_material(large, MAT_AT + i * 256, 0);
        word(large, MAT_AT + i * 256 + 12, UINT32_MAX);
        word(large, MAT_AT + i * 256 + 180, 0);
        word(large, MAT_AT + i * 256 + 200, 0);
        word(large, MAT_AT + i * 256 + 192, 0);
        word(large, MAT_AT + i * 256 + 212, 0);
        word(large, MAT_AT + i * 256 + 236, 0);
        word(large, MAT_AT + i * 256 + 248, UINT32_MAX);
    }
    word(large, MAT_AT + 180, 64000);
    word(large, MAT_AT + 236, 2048000);
    word(large, MAT_AT + 248, BASE + 1048576);
    struct cache_arena_owner owner = {0};
    struct cache_arena_handle handle;
    struct cache_arena_plan plan;
    struct cache_arena_result arena;
    const struct cache_arena_request request = {large_bytes, 1};
    int prepared = cache_arena_plan_build(&request, 1, large, large_bytes, 0, &plan, &arena) &&
        cache_arena_owner_bind(&owner, &plan, large, large_bytes, &arena) &&
        cache_arena_owner_handle(&owner, 0, &handle, &arena);
    LIMIT_CHECK(prepared, "large_owner_bind");
    if (!prepared) { failed = abort_case(context, "large_owner"); goto cleanup; }
    memset(&bsp, 0, sizeof(bsp));
    prepared = cache_bsp_bind(&bsp, &owner, &handle, TAG_BYTES, 8u * 1048576u, 0, &parent, &(struct cache_bsp_result){0});
    LIMIT_CHECK(prepared, "large_bsp_bind");
    if (!prepared) { failed = abort_case(context, "large_bsp"); goto cleanup; }
    prepared = cache_material_measure(&parent, &req, &result);
    LIMIT_CHECK(prepared && req.material_count == 2048 && req.workspace_bytes == 524376 &&
                req.serialized_bytes == 524968, "2048_materials_and64000_vertices_measure");
    if (!prepared) { failed = abort_case(context, "large_measure"); goto cleanup; }
    workspace = malloc(req.workspace_bytes);
    serialized = malloc(req.serialized_bytes);
    if (!workspace || !serialized) { failed = abort_case(context, "large_projection_allocation"); goto cleanup; }
    struct cache_material_control control = {0};
    struct cache_material_view view = {0};
    prepared = cache_material_bind(&control, &parent, workspace, req.workspace_bytes, &view, &result);
    LIMIT_CHECK(prepared, "2048_materials_complete_publication");
    if (!prepared) { failed = abort_case(context, "large_publication"); goto cleanup; }
    struct cache_material_projection m;
    LIMIT_CHECK(cache_material_get_material(&view, 0, &m, &result) && m.vertex_buffers[0].count == 64000,
                "64000_count_retained");
    LIMIT_CHECK(cache_material_get_material(&view, 2047, &m, &result) && m.source.offset == MAT_AT + 2047u * 256u,
                "last_source_maximum_material_not_truncated");
    size_t used;
    LIMIT_CHECK(cache_material_serialize(&view, serialized, req.serialized_bytes, &used, &result) &&
                used == req.serialized_bytes && !memcmp(serialized + 680, large + MAT_AT, 2048u * 256u),
                "all2048_roundtrip_raw_materials");
    LIMIT_CHECK(cache_material_unload(&control, &result), "large_child_unload");
    word(large, MAT_AT + 180, 64001);
    LIMIT_CHECK(!cache_material_measure(&parent, &req, &result) && result.error == CACHE_MATERIAL_COUNT,
                "vertex64001_reject");
    word(large, MAT_AT + 180, 64000);
    word(large, LM_AT + 20, 2049);
    LIMIT_CHECK(!cache_material_measure(&parent, &req, &result) && result.error == CACHE_MATERIAL_COUNT,
                "material2049_reject");
cleanup:
    if (owner.live && !cache_arena_owner_release(&owner, &arena))
        failed = abort_case(context, "large_cleanup_release");
    free(serialized);
    free(workspace);
    free(large);
    return failed;
#undef LIMIT_CHECK
}

int cache_material_fixture(FILE *report, int collect)
{
    if (!report)
        return 1;
    struct context context_value = {report, "setup", 0, 0, 0, collect};
    struct context *context = &context_value;
    struct storage storage = {0};
    storage.placements[0] = malloc(SLOT_BYTES + GUARD * 2 + 8);
    storage.placements[1] = malloc(SLOT_BYTES + GUARD * 2 + 8);
    storage.snapshot = malloc(SLOT_BYTES);
    storage.work = malloc(WORK_BYTES + GUARD * 2);
    storage.work_before = malloc(WORK_BYTES + GUARD * 2);
    storage.serial = malloc(SERIAL_BYTES + GUARD * 2 + 8);
    storage.golden = malloc(SERIAL_BYTES);
    int aborted = 0;
    if (!storage.placements[0] || !storage.placements[1] || !storage.snapshot || !storage.work ||
        !storage.work_before || !storage.serial || !storage.golden) {
        aborted = abort_case(context, "allocation");
        goto cleanup;
    }
    if (fprintf(report, "CACHE_MATERIAL BEGIN scope=authored_partial_root_lightmap_material_no_geometry\n") < 0) {
        aborted = 1;
        goto cleanup;
    }
    for (unsigned placement = 0; placement < 2 && !aborted; ++placement)
        for (unsigned offset = 0; offset < 8 && !aborted; ++offset)
            aborted = valid_case(context, &storage, placement, offset);
    static const struct mutation mutations[] = {
        {"negative_lightmaps", ROOT_AT + 260, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"overmaximum_lightmaps", ROOT_AT + 260, 129, 4, CACHE_MATERIAL_COUNT},
        {"lightmaps_gap", ROOT_AT + 264, BASE + TAG_BYTES, 4, CACHE_MATERIAL_SPAN},
        {"lightmaps_in_tag_window", ROOT_AT + 264, BASE + 2800, 4, CACHE_MATERIAL_SPAN},
        {"lightmaps_root_overlap", ROOT_AT + 264, BASE + ROOT_AT, 4, CACHE_MATERIAL_OVERLAP},
        {"negative_materials", LM_AT + 20, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"overmaximum_materials", LM_AT + 20, 2049, 4, CACHE_MATERIAL_COUNT},
        {"material_span_onepast", LM_AT + 24, BASE + SLOT_BYTES - 255, 4, CACHE_MATERIAL_SPAN},
        {"materials_root_overlap", LM_AT + 24, BASE + ROOT_AT, 4, CACHE_MATERIAL_OVERLAP},
        {"materials_table_overlap", LM_AT + 24, BASE + LM_AT, 4, CACHE_MATERIAL_OVERLAP},
        {"two_material_blocks_overlap", LM_AT + 32 + 24, BASE + MAT_AT, 4, CACHE_MATERIAL_OVERLAP},
        {"shader_full_datum_mismatch", MAT_AT + 12, UINT32_C(0xbbcd0002), 4, CACHE_MATERIAL_DATUM},
        {"shader_ordinal_onepast", MAT_AT + 12, UINT32_C(0xabcd0003), 4, CACHE_MATERIAL_DATUM},
        {"shader_primary_record_mismatch", MAT_AT, SHDR, 4, CACHE_MATERIAL_GROUP},
        {"shader_ancestor_missing", 36 + 2 * 32 + 4, UINT32_MAX, 4, CACHE_MATERIAL_GROUP},
        {"shader_record_name_gap", MAT_AT + 4, BASE + TAG_BYTES, 4, CACHE_MATERIAL_STRING},
        {"shader_instance_name_gap", 36 + 2 * 32 + 16, BASE + TAG_BYTES, 4, CACHE_MATERIAL_STRING},
        {"shader_record_name_unterminated", MAT_AT + 4, BASE + TAG_BYTES - 1, 4, CACHE_MATERIAL_STRING},
        {"negative_surface_first", MAT_AT + 20, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"negative_surface_count", MAT_AT + 24, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"surface_range_past_root", MAT_AT + 20, 10, 4, CACHE_MATERIAL_COUNT},
        {"negative_root_surface_count", ROOT_AT + 248, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"root_surface_count_overmax", ROOT_AT + 248, 131073, 4, CACHE_MATERIAL_COUNT},
        {"root_surface_span_gap", ROOT_AT + 252, BASE + TAG_BYTES, 4, CACHE_MATERIAL_SPAN},
        {"root_surface_span_one_short", ROOT_AT + 252, BASE + SLOT_BYTES - 71, 4, CACHE_MATERIAL_SPAN},
        {"negative_environment_count", MAT_AT + 180, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"negative_lightmap_count", MAT_AT + 200, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"environment_count_overmax", MAT_AT + 180, 64001, 4, CACHE_MATERIAL_COUNT},
        {"lightmap_count_overmax", MAT_AT + 200, 64001, 4, CACHE_MATERIAL_COUNT},
        {"environment_wrong_type", MAT_AT + 176, 2, 2, CACHE_MATERIAL_TYPE},
        {"lightmap_wrong_type", MAT_AT + 196, 1, 2, CACHE_MATERIAL_TYPE},
        {"environment_negative_type", MAT_AT + 176, UINT16_MAX, 2, CACHE_MATERIAL_TYPE},
        {"hardware_record_unaligned", MAT_AT + 192, BASE + VERTEX_TABLE + 1, 4, CACHE_MATERIAL_RESOURCE},
        {"hardware_wrong_kind_table", MAT_AT + 192, BASE + INDEX_TABLE, 4, CACHE_MATERIAL_RESOURCE},
        {"lightmap_hardware_wrong_table", MAT_AT + 212, BASE + VERTEX_TABLE, 4, CACHE_MATERIAL_RESOURCE},
        {"hardware_record_onepast", MAT_AT + 192, BASE + VERTEX_TABLE + 24, 4, CACHE_MATERIAL_RESOURCE},
        {"hardware_payload_truncated", VERTEX_TABLE + 4, BASE + SLOT_BYTES - 95, 4, CACHE_MATERIAL_SPAN},
        {"hardware_payload_tag_not_BSP", VERTEX_TABLE + 4, BASE + 3000, 4, CACHE_MATERIAL_SPAN},
        {"negative_uncompressed_size", MAT_AT + 216, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"negative_compressed_size", MAT_AT + 236, UINT32_MAX, 4, CACHE_MATERIAL_COUNT},
        {"compressed_size_source_overmax", MAT_AT + 236, 2560001, 4, CACHE_MATERIAL_COUNT},
        {"compressed_payload_truncated", MAT_AT + 248, BASE + SLOT_BYTES - 112, 4, CACHE_MATERIAL_SPAN},
        {"compressed_payload_in_tag", MAT_AT + 248, BASE + 3000, 4, CACHE_MATERIAL_SPAN},
        {"compressed_minimum_one_short", MAT_AT + 236, 111, 4, CACHE_MATERIAL_SPAN}
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]) && !aborted; ++i)
        aborted = invalid_case(context, &storage, &mutations[i]);
    if (!aborted)
        aborted = edges(context, &storage);
    if (!aborted)
        aborted = lifecycle(context, &storage);
    if (!aborted)
        aborted = arena_backing(context, &storage);
    if (!aborted)
        aborted = limits(context, &storage);
cleanup:
    if (storage.owner.live) {
        struct cache_arena_result arena;
        if (!cache_arena_owner_release(&storage.owner, &arena))
            aborted = abort_case(context, "fixture_cleanup_release");
    }
    free(storage.golden);
    free(storage.serial);
    free(storage.work_before);
    free(storage.work);
    free(storage.snapshot);
    free(storage.placements[1]);
    free(storage.placements[0]);
    if (fprintf(report, "CACHE_MATERIAL SUMMARY cases=%u checks=%u failures=%u aborted=%d material=%lu root=%lu lightmap=%lu control=%lu view=%lu lifetime=parent_first geometry=not_qualified\n",
                context->cases, context->checks, context->failures, aborted,
                (unsigned long)sizeof(struct cache_material_projection), (unsigned long)sizeof(struct cache_material_root_projection),
                (unsigned long)sizeof(struct cache_material_lightmap_projection), (unsigned long)sizeof(struct cache_material_control),
                (unsigned long)sizeof(struct cache_material_view)) < 0 || fflush(report))
        return 1;
    return aborted || context->failures;
}
