#include "cache_bsp_fixture.h"
#include "cache_bsp_probe.h"
#include <stdlib.h>
#include <string.h>

enum { SLOT_BYTES = 8192, STORAGE_BYTES = 8320, GUARD = 32, TAG_BYTES = 3072,
       SCENARIO = 512, REFS = 2048, BSP = 4096, BSP_BYTES = 2048, ROOT = BSP + 24,
       TAG_VERTICES = 256, TAG_INDICES = 280,
       VERTICES = BSP + 800, LIGHTMAPS = BSP + 840, DATA = BSP + 1800 };
#define BASE CACHE_BSP_TAG_BASE
#define SCENARIO_DATUM UINT32_C(0x12340000)
#define BSP_DATUM UINT32_C(0x23450001)
#define BSP_GROUP UINT32_C(0x73627370)

struct fixture_context
{
    FILE *report;
    const char *name;
    unsigned cases, checks, failures;
    int collect;
};
struct fixture_storage
{
    unsigned char *placements[2], *snapshot, *serialized;
    struct cache_arena_owner owner;
    struct cache_arena_handle handle;
};

static int check(struct fixture_context *context, int condition, const char *assertion)
{
    ++context->checks;
    if (condition)
        return 1;
    ++context->failures;
    if (fprintf(context->report, "CACHE_BSP FAIL case=%s assertion=%s check=%u\n",
                context->name, assertion, context->checks) < 0 || fflush(context->report))
        return 0;
    return context->collect;
}
#define CHECK(condition, assertion) do { if (!check(context, (condition), (assertion))) return 1; } while (0)

static int abort_case(struct fixture_context *context, const char *reason)
{
    (void)fprintf(context->report, "CACHE_BSP ABORT case=%s reason=%s\n", context->name, reason);
    (void)fflush(context->report);
    return 1;
}

static void word(unsigned char *bytes, size_t offset, uint32_t value)
{
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[offset + byte] = (unsigned char)(value >> (byte * 8));
}

static void author(unsigned char *bytes)
{
    memset(bytes, 0x6d, SLOT_BYTES);
    const uint32_t header[9] = {BASE + 36, SCENARIO_DATUM, UINT32_C(0xa5b6c7d8), 2, 2,
        BASE + TAG_VERTICES, 1, BASE + TAG_INDICES, UINT32_C(0x74616773)};
    for (unsigned index = 0; index < 9; ++index)
        word(bytes, index * 4, header[index]);
    const uint32_t instances[16] = {UINT32_C(0x73636e72), UINT32_MAX, UINT32_MAX, SCENARIO_DATUM,
        BASE + 128, BASE + SCENARIO, UINT32_C(0x99887766), UINT32_C(0x55443322),
        BSP_GROUP, UINT32_MAX, UINT32_MAX, BSP_DATUM, BASE + 136, 0,
        UINT32_C(0x88776655), UINT32_C(0x44332211)};
    for (unsigned index = 0; index < 16; ++index)
        word(bytes, 36 + index * 4, instances[index]);
    /* These Data words are resource offsets, not encoded tag addresses. */
    word(bytes, TAG_VERTICES + 4, UINT32_MAX);
    word(bytes, TAG_VERTICES + 16, 1);
    word(bytes, TAG_INDICES + 4, BASE + SLOT_BYTES + 1);
    word(bytes, SCENARIO + 0x5a4, 1);
    word(bytes, SCENARIO + 0x5a8, BASE + REFS);
    word(bytes, SCENARIO + 0x5ac, UINT32_C(0xabcdef01));
    for (unsigned index = 0; index < 16; ++index)
    {
        const uint32_t reference[8] = {4096, BSP_BYTES, BASE + BSP, UINT32_C(0x12345678),
            BSP_GROUP, BASE + 136, UINT32_C(0xffffffff), BSP_DATUM};
        for (unsigned field = 0; field < 8; ++field)
            word(bytes, REFS + index * 32 + field * 4, reference[field]);
    }
    for (unsigned index = 0; index < BSP_BYTES; ++index)
        bytes[BSP + index] = (unsigned char)(index * 37 + 11);
    const uint32_t bsp_header[6] = {BASE + ROOT, 2, BASE + VERTICES, 1, BASE + LIGHTMAPS, BSP_GROUP};
    for (unsigned index = 0; index < 6; ++index)
        word(bytes, BSP + index * 4, bsp_header[index]);
    for (unsigned index = 0; index < 3; ++index)
    {
        size_t offset = index < 2 ? VERTICES + index * 12 : LIGHTMAPS;
        word(bytes, offset, UINT32_C(0xfedcba98) + index);
        word(bytes, offset + 4, BASE + DATA + index);
        word(bytes, offset + 8, UINT32_C(0x87654321) + index);
    }
}

static int prepare(struct fixture_storage *storage, unsigned placement, unsigned offset,
                   unsigned char **bytes)
{
    struct cache_arena_result result;
    if (!cache_arena_owner_release(&storage->owner, &result))
        return 0;
    memset(storage->placements[placement], 0xa7, STORAGE_BYTES);
    *bytes = storage->placements[placement] + GUARD + offset;
    author(*bytes);
    struct cache_arena_request request = {SLOT_BYTES, 1};
    struct cache_arena_plan plan;
    return cache_arena_plan_build(&request, 1, *bytes, SLOT_BYTES, 0, &plan, &result) &&
           cache_arena_owner_bind(&storage->owner, &plan, *bytes, SLOT_BYTES, &result) &&
           cache_arena_owner_handle(&storage->owner, 0, &storage->handle, &result);
}

static int success_case(struct fixture_context *context, struct fixture_storage *storage,
                        unsigned placement, unsigned offset)
{
    context->name = "offset_placement_roundtrip";
    ++context->cases;
    unsigned char *bytes;
    if (!prepare(storage, placement, offset, &bytes))
        return abort_case(context, "prepare");
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    memset(storage->serialized, 0xa7, BSP_BYTES + GUARD * 2);
    struct cache_bsp_control control = {0};
    struct cache_bsp_view view;
    struct cache_bsp_result result;
    struct cache_bsp_reference reference;
    int selected = cache_bsp_select(&storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &reference, &result);
    CHECK(selected, "select");
    if (!selected)
        return abort_case(context, "cannot_follow_failed_select");
    CHECK(reference.file_offset == 4096 && reference.file_size == BSP_BYTES &&
          reference.slot_offset == BSP && reference.rounded_bytes == BSP_BYTES, "numeric_reference_extent");
    CHECK(reference.group == BSP_GROUP && reference.datum == BSP_DATUM && reference.scenario_datum == SCENARIO_DATUM &&
          reference.unused_word == UINT32_C(0x12345678) && reference.name_length_word == UINT32_MAX, "reference_opaque_words");
    int bound = cache_bsp_bind(&control, &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &view, &result);
    CHECK(bound, "bind");
    if (!bound)
        return abort_case(context, "cannot_follow_failed_bind");
    struct cache_bsp_header header;
    struct cache_address_span span;
    CHECK(cache_bsp_get_header(&view, &header, &result) && header.root_address == BASE + ROOT &&
          header.vertex_count == 2 && header.index_count == 1 && header.signature == BSP_GROUP, "header_six_words");
    CHECK(cache_bsp_get_root(&view, &span, &result) && span.offset == ROOT && span.length == 648, "root_shared_slot_coordinates");
    CHECK(cache_bsp_lookup(&view, &reference, &result) && reference.datum == BSP_DATUM, "lookup_full_identity");
    for (unsigned index = 0; index < 3; ++index)
    {
        struct cache_bsp_descriptor descriptor;
        CHECK(cache_bsp_get_descriptor(&view, index < 2 ? 0 : 1, index < 2 ? index : 0, &descriptor, &result) &&
              descriptor.words[0] == UINT32_C(0xfedcba98) + index && descriptor.words[1] == BASE + DATA + index &&
              descriptor.words[2] == UINT32_C(0x87654321) + index && descriptor.raw.length == 12, "descriptor_words_identity");
    }
    CHECK(cache_bsp_resolve(&view, BASE + TAG_BYTES - 1, 1, 1, &span, &result) &&
          span.offset == TAG_BYTES - 1, "last_tag_byte");
    CHECK(cache_bsp_resolve(&view, BASE + BSP + BSP_BYTES - 1, 1, 1, &span, &result) &&
          span.offset == BSP + BSP_BYTES - 1, "last_bsp_byte");
    CHECK(!cache_bsp_resolve(&view, BASE + TAG_BYTES, 1, 1, &span, &result) && result.error == CACHE_BSP_SPAN, "tag_onepast_gap_rejected");
    CHECK(!cache_bsp_resolve(&view, BASE + BSP + BSP_BYTES, 1, 1, &span, &result) && result.error == CACHE_BSP_SPAN, "bsp_onepast_rejected");
    CHECK(!cache_bsp_resolve(&view, BASE + TAG_BYTES - 1, 2, 1, &span, &result), "cross_valid_window_rejected");
    CHECK(cache_bsp_resolve(&view, UINT32_MAX, 0, 0, &span, &result) && !span.offset && !span.length, "empty_ignores_pointer_stride");
    CHECK(cache_bsp_resolve(&view, BASE + BSP + BSP_BYTES, 0, 12, &span, &result), "empty_onepast");
    CHECK(!cache_bsp_resolve(&view, BASE, -1, 1, &span, &result) && result.error == CACHE_BSP_COUNT, "negative_resolve_count");
    CHECK(!cache_bsp_resolve(&view, BASE, INT64_MAX, SIZE_MAX, &span, &result) && result.error == CACHE_BSP_OVERFLOW, "resolve_multiply_overflow");
    size_t used = 99;
    CHECK(cache_bsp_serialize(&view, storage->serialized + GUARD, BSP_BYTES, &used, &result) && used == BSP_BYTES &&
          !memcmp(storage->serialized + GUARD, storage->snapshot + BSP, BSP_BYTES), "whole_bsp_lossless_encoded_and_opaque");
    for (unsigned index = 0; index < GUARD; ++index)
        CHECK(storage->serialized[index] == 0xa7 && storage->serialized[GUARD + BSP_BYTES + index] == 0xa7, "serialization_guards");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES), "entire_source_unchanged");
    unsigned char *placement_bytes = storage->placements[placement];
    for (unsigned index = 0; index < GUARD + offset; ++index)
        CHECK(placement_bytes[index] == 0xa7, "leading_source_guards");
    for (unsigned index = GUARD + offset + SLOT_BYTES; index < STORAGE_BYTES; ++index)
        CHECK(placement_bytes[index] == 0xa7, "trailing_source_guards");
    CHECK(cache_bsp_unload(&control, &result), "child_release");
    CHECK(!cache_bsp_get_root(&view, &span, &result) && result.error == CACHE_BSP_STATE, "stale_child_release");
    CHECK(storage->owner.live, "child_release_keeps_parent_owned");
    uint64_t generation = control.generation;
    CHECK(cache_bsp_unload(&control, &result) && control.generation == generation, "inactive_release_idempotent");
    return 0;
}

struct mutation { const char *name; size_t offset; uint32_t value; enum cache_bsp_error error; };
static int failure_case(struct fixture_context *context, struct fixture_storage *storage,
                        const struct mutation *mutation)
{
    context->name = mutation->name;
    ++context->cases;
    unsigned char *bytes;
    if (!prepare(storage, 0, 3, &bytes))
        return abort_case(context, "prepare");
    word(bytes, mutation->offset, mutation->value);
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    struct cache_bsp_control control = {0};
    unsigned char before[sizeof(control)];
    memcpy(before, &control, sizeof(control));
    struct cache_bsp_view view;
    memset(&view, 0x5a, sizeof(view));
    unsigned char view_before[sizeof(view)];
    memcpy(view_before, &view, sizeof(view));
    struct cache_bsp_result result;
    CHECK(!cache_bsp_bind(&control, &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &view, &result), "reject");
    CHECK(result.error == mutation->error, "structured_reason");
    CHECK(!memcmp(&control, before, sizeof(control)), "control_failure_atomic");
    CHECK(!memcmp(&view, view_before, sizeof(view)), "view_failure_atomic");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES), "entire_source_failure_unchanged");
    return 0;
}

static int edges(struct fixture_context *context, struct fixture_storage *storage)
{
    unsigned char *bytes;
    struct cache_bsp_result result;
    struct cache_arena_result arena;
    struct cache_bsp_reference reference;
    struct cache_bsp_control controls[2] = {{0}, {0}};
    struct cache_bsp_view views[2] = {{0}, {0}}, stale;
    struct cache_address_span span;
    context->name = "tag_header_empty_tables_ignore_addresses";
    ++context->cases;
    if (!prepare(storage, 0, 0, &bytes))
        return abort_case(context, "prepare");
    word(bytes, 16, 0);
    word(bytes, 20, UINT32_MAX);
    word(bytes, 24, 0);
    word(bytes, 28, 1);
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    CHECK(cache_bsp_select(&storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &reference, &result),
          "tag_empty_tables_select");
    CHECK(cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result),
          "tag_empty_tables_bind");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES), "tag_empty_tables_preserve_unused_addresses_and_source");
    CHECK(cache_bsp_unload(&controls[0], &result), "tag_empty_tables_release");
    context->name = "source_limits_zero_arrays_rounding";
    ++context->cases;
    if (!prepare(storage, 0, 0, &bytes))
        return abort_case(context, "prepare");
    word(bytes, SCENARIO + 0x5a4, 16);
    CHECK(cache_bsp_select(&storage->owner, &storage->handle, TAG_BYTES, 32768, 15, &reference, &result) &&
          reference.ordinal == 15, "source_count16_last_ordinal");
    CHECK(!cache_bsp_select(&storage->owner, &storage->handle, TAG_BYTES, 32768, 16, &reference, &result), "ordinal16_excluded");
    word(bytes, REFS, 0);
    word(bytes, REFS + 4, 2047);
    word(bytes, BSP + 4, 0);
    word(bytes, BSP + 8, UINT32_MAX);
    word(bytes, BSP + 12, 0);
    word(bytes, BSP + 16, 1);
    CHECK(cache_bsp_select(&storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &reference, &result) &&
          reference.file_offset == 0 && reference.file_size == 2047 && reference.rounded_bytes == 2048,
          "file_offset0_allowed_and512_rounding");
    int bound = cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result);
    CHECK(bound, "zero_counts_ignore_unused_addresses");
    if (!bound)
        return abort_case(context, "zero_counts_bind");
    struct cache_bsp_header header;
    CHECK(cache_bsp_get_header(&views[0], &header, &result) && header.vertex_address == UINT32_MAX &&
          header.index_address == 1, "unused_header_addresses_preserved");
    CHECK(!cache_bsp_get_descriptor(&views[0], 0, 0, &(struct cache_bsp_descriptor){0}, &result) &&
          result.error == CACHE_BSP_COUNT, "empty_descriptor_access_rejected");
    CHECK(cache_bsp_unload(&controls[0], &result), "release");
    word(bytes, REFS + 8, BASE + SLOT_BYTES - 2047);
    CHECK(!cache_bsp_select(&storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &reference, &result) &&
          result.error == CACHE_BSP_SPAN, "declared_fits_but_rounded_read_exceeds_slot");
    context->name = "independent_epochs_failure_atomic";
    ++context->cases;
    author(bytes);
    for (unsigned index = 0; index < 2; ++index)
        if (!cache_bsp_bind(&controls[index], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[index], &result))
            return abort_case(context, "bind");
    stale = views[0];
    CHECK(cache_bsp_unload(&controls[0], &result), "first_child_release");
    CHECK(cache_bsp_get_root(&views[1], &span, &result), "other_child_stays_live");
    CHECK(!cache_bsp_get_root(&stale, &span, &result) && result.error == CACHE_BSP_STATE, "released_child_stale");
    CHECK(cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result), "first_child_rebind");
    CHECK(!cache_bsp_get_root(&stale, &span, &result), "old_child_epoch_not_revived");
    stale = views[0];
    CHECK(cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result), "live_child_rebind");
    CHECK(!cache_bsp_get_root(&stale, &span, &result), "live_rebind_invalidates_previous_epoch");
    unsigned char saved[sizeof(controls[0])], saved_view[sizeof(views[0])];
    memcpy(saved, &controls[0], sizeof(saved));
    memcpy(saved_view, &views[0], sizeof(saved_view));
    CHECK(!cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 16, &views[0], &result), "failed_rebind");
    CHECK(!memcmp(saved, &controls[0], sizeof(saved)) && !memcmp(saved_view, &views[0], sizeof(saved_view)), "failed_rebind_keeps_previous_control_view");
    CHECK(cache_bsp_get_root(&views[0], &span, &result), "failed_rebind_previous_view_live");
    context->name = "serialization_failures_atomic";
    ++context->cases;
    memset(storage->serialized, 0xa7, BSP_BYTES + GUARD * 2);
    size_t used = 77;
    CHECK(!cache_bsp_serialize(&views[0], storage->serialized + GUARD, BSP_BYTES - 1, &used, &result) &&
          result.error == CACHE_BSP_CAPACITY && used == 77, "short_destination");
    for (unsigned index = 0; index < BSP_BYTES + GUARD * 2; ++index)
        CHECK(storage->serialized[index] == 0xa7, "short_destination_entire_snapshot");
    memcpy(storage->snapshot, bytes, SLOT_BYTES);
    CHECK(!cache_bsp_serialize(&views[0], bytes + BSP, BSP_BYTES, &used, &result) &&
          result.error == CACHE_BSP_OVERLAP && used == 77, "output_source_overlap");
    CHECK(!memcmp(bytes, storage->snapshot, SLOT_BYTES), "overlap_no_source_mutation");
    context->name = "generation_exhaustion";
    ++context->cases;
    CHECK(cache_bsp_unload(&controls[0], &result), "prepare_exhaustion_unload");
    controls[0].generation = UINT64_MAX - 1;
    CHECK(cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result) &&
          controls[0].generation == UINT64_MAX, "last_generation_bind");
    memcpy(saved, &controls[0], sizeof(saved));
    memcpy(saved_view, &views[0], sizeof(saved_view));
    CHECK(!cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result) &&
          result.error == CACHE_BSP_GENERATION, "bind_generation_exhaustion");
    CHECK(!cache_bsp_unload(&controls[0], &result) && result.error == CACHE_BSP_GENERATION, "active_release_generation_exhaustion");
    CHECK(!memcmp(saved, &controls[0], sizeof(saved)) && !memcmp(saved_view, &views[0], sizeof(saved_view)), "exhaustion_control_view_atomic");
    CHECK(cache_bsp_get_root(&views[0], &span, &result), "exhaustion_retains_live_ownership");
    controls[0] = (struct cache_bsp_control){0};
    controls[0].generation = UINT64_MAX;
    CHECK(cache_bsp_unload(&controls[0], &result) && controls[0].generation == UINT64_MAX, "inactive_exhaustion_idempotent");
    context->name = "parent_release_before_child_control_access";
    ++context->cases;
    stale = views[1];
    CHECK(cache_arena_owner_release(&storage->owner, &arena), "parent_release");
    stale.control = NULL;
    /* A stale view retains a non-null dangling backing address. No dereference
     * is allowed after the independent parent pin fails. */
    stale.control = (const struct cache_bsp_control *)storage->placements[0];
    CHECK(!cache_bsp_get_root(&stale, &span, &result) && result.error == CACHE_BSP_STATE, "parent_checked_before_poisoned_child_backing");
    if (!prepare(storage, 1, 7, &bytes))
        return abort_case(context, "parent_rebind");
    CHECK(!cache_bsp_get_root(&stale, &span, &result) && result.error == CACHE_BSP_STATE, "rebound_parent_old_pin_not_revived");
    context->name = "repeated_cycles";
    ++context->cases;
    controls[0] = (struct cache_bsp_control){0};
    for (unsigned cycle = 0; cycle < 16; ++cycle)
    {
        CHECK(cache_bsp_bind(&controls[0], &storage->owner, &storage->handle, TAG_BYTES, 32768, 0, &views[0], &result), "cycle_bind");
        stale = views[0];
        CHECK(cache_bsp_get_root(&views[0], &span, &result), "cycle_live_view");
        CHECK(cache_bsp_unload(&controls[0], &result), "cycle_unload");
        CHECK(!cache_bsp_get_root(&stale, &span, &result) && result.error == CACHE_BSP_STATE, "cycle_stale_state");
    }
    CHECK(controls[0].generation == 32 && storage->owner.live, "cycles_preserve_parent_advance_child32");
    return 0;
}

int wii_cache_bsp_fixture(FILE *report, int collect_failures)
{
    if (!report)
        return 1;
    struct fixture_context context = {report, "allocation", 0, 0, 0, collect_failures};
    struct fixture_storage storage = {0};
    int aborted = 0;
    if (fprintf(report, "CACHE_BSP BEGIN scope=synthetic_two_windows_header_root_extent_descriptor_identity_only\n") < 0)
        return 1;
    storage.placements[0] = malloc(STORAGE_BYTES);
    storage.placements[1] = malloc(STORAGE_BYTES);
    storage.snapshot = malloc(SLOT_BYTES);
    storage.serialized = malloc(BSP_BYTES + GUARD * 2);
    if (!storage.placements[0] || !storage.placements[1] || !storage.snapshot || !storage.serialized)
    {
        aborted = abort_case(&context, "allocation");
        goto done;
    }
    for (unsigned placement = 0; placement < 2 && !aborted; ++placement)
        for (unsigned offset = 0; offset < 8 && !aborted; ++offset)
            aborted = success_case(&context, &storage, placement, offset);
    const struct mutation mutations[] = {
        {"tag_signature", 32, 0, CACHE_BSP_SIGNATURE},
        {"tag_count_negative", 12, UINT32_MAX, CACHE_BSP_COUNT},
        {"tag_vertex_count_negative", 16, UINT32_MAX, CACHE_BSP_COUNT},
        {"tag_index_count_negative", 24, UINT32_MAX, CACHE_BSP_COUNT},
        {"tag_vertex_span_overrun", 20, BASE + TAG_BYTES - 23, CACHE_BSP_SPAN},
        {"tag_index_span_overrun", 28, BASE + TAG_BYTES - 11, CACHE_BSP_SPAN},
        {"tag_vertex_header_overlap", 20, BASE, CACHE_BSP_OVERLAP},
        {"tag_index_header_overlap", 28, BASE, CACHE_BSP_OVERLAP},
        {"tag_vertex_instances_overlap", 20, BASE + 36, CACHE_BSP_OVERLAP},
        {"tag_index_instances_overlap", 28, BASE + 36, CACHE_BSP_OVERLAP},
        {"tag_descriptor_tables_overlap", 28, BASE + TAG_VERTICES + 12, CACHE_BSP_OVERLAP},
        {"tag_vertex_scenario_overlap", 20, BASE + SCENARIO, CACHE_BSP_OVERLAP},
        {"tag_index_scenario_overlap", 28, BASE + SCENARIO, CACHE_BSP_OVERLAP},
        {"tag_vertex_bsp_reference_overlap", 20, BASE + REFS, CACHE_BSP_OVERLAP},
        {"tag_index_bsp_reference_overlap", 28, BASE + REFS, CACHE_BSP_OVERLAP},
        {"scenario_full_datum", 4, UINT32_C(0x12350000), CACHE_BSP_DATUM},
        {"scenario_group", 36, BSP_GROUP, CACHE_BSP_GROUP},
        {"scenario_root_outside_tags", 56, BASE + TAG_BYTES - 1, CACHE_BSP_SPAN},
        {"scenario_root_index_table_overlap", 56, BASE + 36, CACHE_BSP_OVERLAP},
        {"bsp_count_negative", SCENARIO + 0x5a4, UINT32_MAX, CACHE_BSP_COUNT},
        {"bsp_count17_runtime_limit", SCENARIO + 0x5a4, 17, CACHE_BSP_COUNT},
        {"bsp_count_zero", SCENARIO + 0x5a4, 0, CACHE_BSP_DATUM},
        {"bsp_reference_table_gap", SCENARIO + 0x5a8, BASE + TAG_BYTES, CACHE_BSP_SPAN},
        {"bsp_reference_table_root_overlap", SCENARIO + 0x5a8, BASE + SCENARIO + 100, CACHE_BSP_OVERLAP},
        {"reference_full_datum", REFS + 28, UINT32_C(0x23460001), CACHE_BSP_DATUM},
        {"reference_none_datum", REFS + 28, UINT32_MAX, CACHE_BSP_DATUM},
        {"reference_group", REFS + 16, UINT32_C(0x6269746d), CACHE_BSP_GROUP},
        {"instance_group", 68, UINT32_C(0x6269746d), CACHE_BSP_GROUP},
        {"instance_already_loaded", 88, BASE + ROOT, CACHE_BSP_STATE},
        {"file_offset_negative", REFS, UINT32_MAX, CACHE_BSP_SPAN},
        {"file_size_negative", REFS + 4, UINT32_MAX, CACHE_BSP_SPAN},
        {"file_size_short_header", REFS + 4, 23, CACHE_BSP_SPAN},
        {"file_size_huge_rounding_hostile", REFS + 4, INT32_MAX, CACHE_BSP_SPAN},
        {"file_offset_signed_add_hostile", REFS, INT32_MAX, CACHE_BSP_SPAN},
        {"file_extent_declared_map", REFS, 32768 - 2047, CACHE_BSP_SPAN},
        {"bsp_address_inside_tags", REFS + 8, BASE + TAG_BYTES - 1, CACHE_BSP_SPAN},
        {"bsp_address_below_base", REFS + 8, BASE - 1, CACHE_BSP_SPAN},
        {"bsp_address_wrap_hostile", REFS + 8, UINT32_MAX, CACHE_BSP_SPAN},
        {"header_signature", BSP + 20, 0, CACHE_BSP_SIGNATURE},
        {"root_before_bsp", BSP, BASE + BSP - 1, CACHE_BSP_SPAN},
        {"root_end_overrun", BSP, BASE + BSP + BSP_BYTES - 647, CACHE_BSP_SPAN},
        {"root_header_overlap", BSP, BASE + BSP, CACHE_BSP_OVERLAP},
        {"root_descriptor_overlap", BSP, BASE + VERTICES, CACHE_BSP_OVERLAP},
        {"vertex_count_negative", BSP + 4, UINT32_MAX, CACHE_BSP_COUNT},
        {"index_count_negative", BSP + 12, UINT32_MAX, CACHE_BSP_COUNT},
        {"vertex_count_huge", BSP + 4, INT32_MAX, CACHE_BSP_COUNT},
        {"descriptor_table_overrun", BSP + 8, BASE + BSP + BSP_BYTES - 23, CACHE_BSP_SPAN},
        {"descriptor_tables_overlap", BSP + 16, BASE + VERTICES, CACHE_BSP_OVERLAP},
        {"descriptor_header_overlap", BSP + 8, BASE + BSP, CACHE_BSP_OVERLAP},
        {"descriptor_data_gap", VERTICES + 4, BASE + TAG_BYTES, CACHE_BSP_SPAN},
        {"descriptor_data_tags", VERTICES + 4, BASE + 128, CACHE_BSP_SPAN},
        {"descriptor_data_onepast", LIGHTMAPS + 4, BASE + BSP + BSP_BYTES, CACHE_BSP_SPAN}
    };
    for (size_t index = 0; index < sizeof(mutations) / sizeof(mutations[0]) && !aborted; ++index)
        aborted = failure_case(&context, &storage, &mutations[index]);
    if (!aborted)
        aborted = edges(&context, &storage);
done:
    if (storage.owner.live)
    {
        struct cache_arena_result result;
        if (!cache_arena_owner_release(&storage.owner, &result))
            aborted = abort_case(&context, "owner_release");
    }
    free(storage.placements[0]);
    free(storage.placements[1]);
    free(storage.snapshot);
    free(storage.serialized);
    if (fprintf(report, "CACHE_BSP SUMMARY cases=%u checks=%u failures=%u aborted=%u control_size=%lu view_size=%lu reference_size=%lu header_size=%lu descriptor_size=%lu dynamic_projection_bytes=0 root=648 full_geometry=not_qualified\n",
                context.cases, context.checks, context.failures, aborted != 0,
                (unsigned long)sizeof(struct cache_bsp_control), (unsigned long)sizeof(struct cache_bsp_view),
                (unsigned long)sizeof(struct cache_bsp_reference), (unsigned long)sizeof(struct cache_bsp_header),
                (unsigned long)sizeof(struct cache_bsp_descriptor)) < 0 ||
        fprintf(report, "CACHE_BSP END result=%u\n", aborted || context.failures ? 1 : 0) < 0 ||
        fflush(report))
        return 1;
    return aborted || context.failures;
}
