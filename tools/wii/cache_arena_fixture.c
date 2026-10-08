#include "cache_arena_fixture.h"
#include "cache_arena_plan.h"
#include <stdlib.h>
#include <string.h>

enum
{
    ARENA_CAPACITY = 8192,
    ARENA_STORAGE = 8240,
    ARENA_GUARD = 16
};

struct arena_context
{
    FILE *report;
    const char *name;
    unsigned cases, checks, failures;
    int collect;
};

struct arena_storage
{
    unsigned char *placement[2];
    unsigned char *expected;
    struct cache_arena_owner owner;
};

static int arena_check(struct arena_context *context, int condition, const char *assertion)
{
    ++context->checks;
    if (condition)
        return 1;
    ++context->failures;
    if (fprintf(context->report, "CACHE_ARENA FAIL case=%s assertion=%s check=%u\n", context->name,
                assertion, context->checks) < 0 ||
        fflush(context->report) != 0)
        return 0;
    return context->collect;
}

#define CHECK(condition, assertion)                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!arena_check(context, (condition), (assertion)))                                       \
            return 1;                                                                              \
    } while (0)

static int arena_abort(struct arena_context *context, const char *reason)
{
    (void)fprintf(context->report, "CACHE_ARENA ABORT case=%s reason=%s\n", context->name, reason);
    (void)fflush(context->report);
    return 1;
}

static size_t independent_padding(uintptr_t address, size_t alignment)
{
    return (alignment - address % alignment) % alignment;
}

static int planning(struct arena_context *context, struct arena_storage *storage)
{
    context->name = "actual_pointer_alignment_and_charged_reserve";
    const struct cache_arena_request requests[] = {{17, 1}, {31, 16}, {0, 4096}, {67, 8}};
    memset(storage->expected, 0x5a, ARENA_STORAGE);
    for (unsigned placement = 0; placement < 2; ++placement)
    {
        for (unsigned offset = 0; offset < 8; ++offset)
        {
            ++context->cases;
            memset(storage->placement[placement], 0x5a, ARENA_STORAGE);
            unsigned char *span = storage->placement[placement] + ARENA_GUARD + offset;
            struct cache_arena_plan plan;
            struct cache_arena_result result;
            int accepted =
                cache_arena_plan_build(requests, 4, span, ARENA_CAPACITY, 257, &plan, &result);
            CHECK(accepted && result.error == CACHE_ARENA_OK,
                  "complete_plan_at_two_real_placements");
            if (!accepted)
                return arena_abort(context, "aligned_plan_failed");
            size_t cursor = 0;
            for (size_t index = 0; index < 4; ++index)
            {
                cursor += independent_padding((uintptr_t)span + cursor, requests[index].alignment);
                CHECK(plan.slots[index].offset == cursor &&
                          plan.slots[index].size == requests[index].size &&
                          plan.slots[index].alignment == requests[index].alignment,
                      "independent_actual_alignment_offset_size");
                cursor += requests[index].size;
            }
            CHECK(plan.span_address == (uintptr_t)span && plan.slot_count == 4 &&
                      plan.data_end == cursor && plan.reserve_offset == cursor &&
                      plan.reserve_size == 257 && plan.required == cursor + 257 &&
                      result.required == cursor + 257,
                  "padding_data_and_trailing_reserve_charged_exactly");
            CHECK(memcmp(storage->placement[placement], storage->expected, ARENA_STORAGE) == 0,
                  "planning_never_touches_storage");
            if (fprintf(context->report,
                        "CACHE_ARENA PLAN placement=%u offset=%u alignment_padding=%lu data=%lu "
                        "reserve=257 charged=%lu capacity=%u\n",
                        placement, offset, (unsigned long)(cursor - 115), (unsigned long)cursor,
                        (unsigned long)plan.required, ARENA_CAPACITY) < 0)
                return 1;

            for (size_t capacity = plan.required - 1; capacity <= plan.required + 1; ++capacity)
            {
                ++context->cases;
                struct cache_arena_plan output, before;
                memset(&output, 0xa5, sizeof(output));
                memcpy(&before, &output, sizeof(before));
                accepted =
                    cache_arena_plan_build(requests, 4, span, capacity, 257, &output, &result);
                CHECK(accepted == (capacity >= plan.required), "exact_total_capacity_boundary");
                if (!accepted)
                    CHECK(result.error == CACHE_ARENA_CAPACITY &&
                              memcmp(&output, &before, sizeof(output)) == 0,
                          "short_plan_rejection_atomic_output");
            }
        }
    }
    return 0;
}

static int rejected_plans(struct arena_context *context, struct arena_storage *storage)
{
    context->name = "malformed_request_count_alignment_and_overflow";
    unsigned char *span = storage->placement[0] + ARENA_GUARD;
    struct cache_arena_request requests[17];
    for (size_t index = 0; index < 17; ++index)
        requests[index] = (struct cache_arena_request){1, 1};
    const size_t bad_alignments[] = {0, 3, 6, 4097, 8192, SIZE_MAX};
    struct cache_arena_result result;
    for (size_t index = 0; index < sizeof(bad_alignments) / sizeof(bad_alignments[0]); ++index)
    {
        ++context->cases;
        struct cache_arena_plan output, before;
        memset(&output, 0xa5, sizeof(output));
        memcpy(&before, &output, sizeof(before));
        requests[0].alignment = bad_alignments[index];
        CHECK(!cache_arena_plan_build(requests, 1, span, 64, 0, &output, &result) &&
                  result.error == CACHE_ARENA_ALIGNMENT &&
                  memcmp(&output, &before, sizeof(output)) == 0,
              "invalid_alignment_atomic_plan");
    }
    requests[0].alignment = 1;
    struct cache_arena_plan output, before;
    memset(&output, 0xa5, sizeof(output));
    memcpy(&before, &output, sizeof(before));
    ++context->cases;
    CHECK(!cache_arena_plan_build(requests, 17, span, 64, 0, &output, &result) &&
              result.error == CACHE_ARENA_COUNT && memcmp(&output, &before, sizeof(output)) == 0,
          "seventeen_slots_rejected_without_output");
    CHECK(cache_arena_plan_build(requests, 16, span, 16, 0, &output, &result) &&
              output.required == 16 && output.slot_count == 16,
          "sixteen_slots_exact_capacity");
    ++context->cases;
    CHECK(cache_arena_plan_build(NULL, 0, span, 16, 16, &output, &result) &&
              output.slot_count == 0 && output.data_end == 0 && output.reserve_offset == 0 &&
              output.required == 16,
          "reserve_only_plan_count_zero");
    CHECK(cache_arena_plan_build(NULL, 0, span, 0, 0, &output, &result) && output.required == 0,
          "empty_plan_zero_capacity");

    const struct cache_arena_request overflow[] = {{1, 1}, {SIZE_MAX, 1}};
    memset(&output, 0xa5, sizeof(output));
    memcpy(&before, &output, sizeof(before));
    ++context->cases;
    CHECK(!cache_arena_plan_build(overflow, 2, span, 64, 0, &output, &result) &&
              result.error == CACHE_ARENA_OVERFLOW && memcmp(&output, &before, sizeof(output)) == 0,
          "slot_size_addition_overflow_atomic");
    CHECK(!cache_arena_plan_build(requests, 1, span, 64, SIZE_MAX, &output, &result) &&
              result.error == CACHE_ARENA_OVERFLOW && memcmp(&output, &before, sizeof(output)) == 0,
          "trailing_reserve_addition_overflow_atomic");
    CHECK(!cache_arena_plan_build(requests, 1, span, SIZE_MAX, 0, &output, &result) &&
              result.error == CACHE_ARENA_OVERFLOW && memcmp(&output, &before, sizeof(output)) == 0,
          "numeric_span_address_wrap_before_any_io");
    CHECK(!cache_arena_plan_build(NULL, 1, span, 64, 0, &output, &result) &&
              result.error == CACHE_ARENA_ARGUMENT && memcmp(&output, &before, sizeof(output)) == 0,
          "missing_requests_atomic_rejection");
    return 0;
}

static int capacity_prefixes(struct arena_context *context, struct arena_storage *storage)
{
    context->name = "every_small_capacity_prefix_rejects_atomic_plan";
    const struct cache_arena_request requests[] = {{7, 8}, {11, 16}};
    for (unsigned placement = 0; placement < 2; ++placement)
    {
        for (unsigned offset = 0; offset < 8; ++offset)
        {
            unsigned char *span = storage->placement[placement] + ARENA_GUARD + offset;
            size_t required = independent_padding((uintptr_t)span, 8) + 7;
            required += independent_padding((uintptr_t)span + required, 16) + 11 + 5;
            for (size_t capacity = 0; capacity <= required + 1; ++capacity)
            {
                ++context->cases;
                struct cache_arena_plan output, before;
                memset(&output, 0xa5, sizeof(output));
                memcpy(&before, &output, sizeof(before));
                struct cache_arena_result result;
                int accepted =
                    cache_arena_plan_build(requests, 2, span, capacity, 5, &output, &result);
                CHECK(accepted == (capacity >= required), "every_prefix_capacity_acceptance");
                if (accepted)
                    CHECK(output.required == required, "capacity_independent_complete_charge");
                else
                    CHECK(result.error == CACHE_ARENA_CAPACITY &&
                              memcmp(&output, &before, sizeof(output)) == 0,
                          "every_prefix_capacity_preserves_full_output");
            }
        }
    }
    return 0;
}

static int reject_resolution(struct arena_context *context, const struct cache_arena_owner *owner,
                             const struct cache_arena_handle *handle, size_t offset, size_t length,
                             enum cache_arena_error error)
{
    ++context->cases;
    unsigned char *pointer = (unsigned char *)owner;
    unsigned char *before = pointer;
    struct cache_arena_result result;
    int accepted = cache_arena_owner_resolve(owner, handle, offset, length, &pointer, &result);
    CHECK(!accepted && result.error == error && pointer == before,
          "invalid_view_atomic_pointer_output");
    return 0;
}

static int ownership(struct arena_context *context, struct arena_storage *storage)
{
    context->name = "generation_handles_alternate_real_placement_64_cycles";
    const struct cache_arena_request requests[] = {{17, 16}, {31, 32}, {67, 8}};
    struct cache_arena_handle stale = {0, 0};
    uint64_t expected_generation = 0;
    for (unsigned cycle = 0; cycle < 64; ++cycle)
    {
        ++context->cases;
        unsigned placement = cycle % 2;
        unsigned offset = cycle % 8;
        memset(storage->placement[placement], 0x5a, ARENA_STORAGE);
        memcpy(storage->expected, storage->placement[placement], ARENA_STORAGE);
        unsigned char *span = storage->placement[placement] + ARENA_GUARD + offset;
        struct cache_arena_plan plan;
        struct cache_arena_result result;
        int accepted =
            cache_arena_plan_build(requests, 3, span, ARENA_CAPACITY, 257, &plan, &result);
        CHECK(accepted, "cycle_complete_plan");
        if (!accepted)
            return arena_abort(context, "cycle_plan_failed");
        accepted = cache_arena_owner_bind(&storage->owner, &plan, span, ARENA_CAPACITY, &result);
        CHECK(accepted && storage->owner.live && storage->owner.generation == ++expected_generation,
              "bind_advances_epoch_and_binds_exact_span");
        if (!accepted)
            return arena_abort(context, "cycle_bind_failed");
        if (cycle && reject_resolution(context, &storage->owner, &stale, 0, 1, CACHE_ARENA_HANDLE))
            return 1;

        for (size_t index = 0; index < 3; ++index)
        {
            struct cache_arena_handle handle;
            accepted = cache_arena_owner_handle(&storage->owner, index, &handle, &result);
            CHECK(accepted && handle.generation == expected_generation &&
                      handle.slot_index == index,
                  "slot_handle_has_live_generation_and_index");
            if (!accepted)
                return arena_abort(context, "cycle_handle_failed");
            unsigned char *pointer = NULL;
            accepted = cache_arena_owner_resolve(&storage->owner, &handle, 0, requests[index].size,
                                                 &pointer, &result);
            CHECK(accepted && pointer == span + plan.slots[index].offset &&
                      (uintptr_t)pointer % requests[index].alignment == 0,
                  "resolved_view_exact_pointer_alignment");
            if (!accepted || !pointer)
                return arena_abort(context, "cycle_resolve_failed");
            memset(pointer, (int)(0x20 + cycle + index), requests[index].size);
            memset(storage->expected + ARENA_GUARD + offset + plan.slots[index].offset,
                   (int)(0x20 + cycle + index), requests[index].size);
            accepted = cache_arena_owner_resolve(&storage->owner, &handle, requests[index].size, 0,
                                                 &pointer, &result);
            CHECK(accepted && pointer == span + plan.slots[index].offset + requests[index].size,
                  "zero_length_slot_end_view_no_dereference");
            if (reject_resolution(context, &storage->owner, &handle, requests[index].size, 1,
                                  CACHE_ARENA_SPAN) ||
                reject_resolution(context, &storage->owner, &handle, SIZE_MAX, 0,
                                  CACHE_ARENA_SPAN) ||
                reject_resolution(context, &storage->owner, &handle, 1, SIZE_MAX, CACHE_ARENA_SPAN))
                return 1;
            if (!index)
                stale = handle;
        }
        CHECK(memcmp(storage->placement[placement], storage->expected, ARENA_STORAGE) == 0,
              "whole_placement_guards_padding_reserve_and_surplus_canaries");
        struct cache_arena_owner before;
        memcpy(&before, &storage->owner, sizeof(before));
        CHECK(!cache_arena_owner_bind(&storage->owner, &plan, span, ARENA_CAPACITY, &result) &&
                  result.error == CACHE_ARENA_STATE &&
                  memcmp(&before, &storage->owner, sizeof(before)) == 0,
              "active_rebind_rejects_without_epoch_or_control_change");
        struct cache_arena_handle bad = stale;
        bad.slot_index = 3;
        if (reject_resolution(context, &storage->owner, &bad, 0, 0, CACHE_ARENA_HANDLE))
            return 1;
        CHECK(cache_arena_owner_release(&storage->owner, &result) && !storage->owner.live &&
                  !storage->owner.span && storage->owner.generation == ++expected_generation,
              "release_clears_binding_and_advances_epoch");
        if (reject_resolution(context, &storage->owner, &stale, 0, 1, CACHE_ARENA_STATE))
            return 1;
        CHECK(cache_arena_owner_release(&storage->owner, &result) &&
                  storage->owner.generation == expected_generation,
              "inactive_release_idempotent");
        CHECK(memcmp(storage->placement[placement], storage->expected, ARENA_STORAGE) == 0,
              "release_does_not_write_or_free_caller_storage");
    }
    return 0;
}

static int binding_validation(struct arena_context *context, struct arena_storage *storage)
{
    context->name = "complete_plan_binding_atomic_validation";
    const struct cache_arena_request request = {16, 16};
    unsigned char *span = storage->placement[0] + ARENA_GUARD;
    struct cache_arena_plan plan;
    struct cache_arena_result result;
    int accepted = cache_arena_plan_build(&request, 1, span, 64, 8, &plan, &result);
    CHECK(accepted, "validation_baseline_complete_plan");
    if (!accepted)
        return arena_abort(context, "validation_plan_failed");
    for (unsigned mutation = 0; mutation < 8; ++mutation)
    {
        ++context->cases;
        struct cache_arena_plan changed = plan;
        switch (mutation)
        {
        case 0:
            ++changed.slots[0].offset;
            break;
        case 1:
            ++changed.data_end;
            break;
        case 2:
            ++changed.reserve_offset;
            break;
        case 3:
            ++changed.required;
            break;
        case 4:
            ++changed.capacity;
            break;
        case 5:
            ++changed.span_address;
            break;
        case 6:
            changed.slot_count = 17;
            break;
        default:
            changed.slots[0].alignment = 3;
            break;
        }
        struct cache_arena_owner before;
        memcpy(&before, &storage->owner, sizeof(before));
        CHECK(!cache_arena_owner_bind(&storage->owner, &changed, span, 64, &result) &&
                  result.error == CACHE_ARENA_PLAN &&
                  memcmp(&before, &storage->owner, sizeof(before)) == 0,
              "corrupt_complete_plan_rejects_atomic_owner");
    }
    ++context->cases;
    struct cache_arena_owner before;
    memcpy(&before, &storage->owner, sizeof(before));
    CHECK(!cache_arena_owner_bind(&storage->owner, &plan, storage->placement[1] + ARENA_GUARD, 64,
                                  &result) &&
              result.error == CACHE_ARENA_PLAN &&
              memcmp(&before, &storage->owner, sizeof(before)) == 0,
          "old_placement_plan_cannot_bind_alternate_span");
    return 0;
}

static int generation_exhaustion(struct arena_context *context)
{
    context->name = "generation_exhaustion_retains_live_storage";
    unsigned char span[64];
    memset(span, 0x5a, sizeof(span));
    struct cache_arena_owner owner;
    memset(&owner, 0, sizeof(owner));
    owner.generation = UINT64_MAX - 2;
    const struct cache_arena_request request = {1, 1};
    struct cache_arena_plan plan;
    struct cache_arena_result result;
    int planned = cache_arena_plan_build(&request, 1, span, sizeof(span), 0, &plan, &result);
    CHECK(planned, "exhaustion_complete_plan");
    if (!planned)
        return arena_abort(context, "exhaustion_plan_failed");
    ++context->cases;
    CHECK(cache_arena_owner_bind(&owner, &plan, span, sizeof(span), &result) &&
              owner.generation == UINT64_MAX - 1,
          "last_releasable_generation_bind");
    CHECK(cache_arena_owner_release(&owner, &result) && owner.generation == UINT64_MAX &&
              !owner.live,
          "last_release_reaches_exhausted_inactive_generation");
    struct cache_arena_owner before;
    memcpy(&before, &owner, sizeof(before));
    CHECK(!cache_arena_owner_bind(&owner, &plan, span, sizeof(span), &result) &&
              result.error == CACHE_ARENA_GENERATION && memcmp(&before, &owner, sizeof(owner)) == 0,
          "exhausted_rebind_atomic_rejection");

    memset(&owner, 0, sizeof(owner));
    owner.generation = UINT64_MAX - 1;
    ++context->cases;
    CHECK(cache_arena_owner_bind(&owner, &plan, span, sizeof(span), &result) &&
              owner.generation == UINT64_MAX,
          "terminal_live_generation_bind");
    memcpy(&before, &owner, sizeof(before));
    CHECK(!cache_arena_owner_release(&owner, &result) && result.error == CACHE_ARENA_GENERATION &&
              memcmp(&before, &owner, sizeof(owner)) == 0 && owner.span == span && owner.live,
          "failed_release_retains_exact_live_owner_and_span");
    struct cache_arena_handle handle;
    int accepted = cache_arena_owner_handle(&owner, 0, &handle, &result);
    CHECK(accepted, "terminal_live_handle_remains_valid");
    if (!accepted)
        return arena_abort(context, "terminal_handle_failed");
    unsigned char *pointer = NULL;
    CHECK(cache_arena_owner_resolve(&owner, &handle, 0, 1, &pointer, &result) && pointer == span,
          "failed_release_does_not_revoke_still_live_owner");
    CHECK(span[0] == 0x5a, "generation_controls_do_not_modify_storage");
    /* No view escapes: the terminal local control and its span end together. */
    return 0;
}

static int fragmentation(struct arena_context *context, struct arena_storage *storage)
{
    context->name = "controlled_free_intervals_not_system_allocator_fragmentation";
    const struct
    {
        size_t offset, size;
    } holes[] = {{0, 16}, {32, 16}, {64, 16}};
    size_t aggregate = 0, largest = 0;
    for (size_t index = 0; index < 3; ++index)
    {
        aggregate += holes[index].size;
        if (holes[index].size > largest)
            largest = holes[index].size;
    }
    CHECK(aggregate == 48 && largest == 16 && aggregate >= 32 && largest < 32,
          "independent_aggregate_free_vs_largest_interval");
    const struct cache_arena_request request = {32, 1};
    struct cache_arena_result result;
    for (size_t index = 0; index < 3; ++index)
    {
        ++context->cases;
        struct cache_arena_plan output, before;
        memset(&output, 0xa5, sizeof(output));
        memcpy(&before, &output, sizeof(before));
        CHECK(!cache_arena_plan_build(&request, 1, storage->placement[0] + holes[index].offset,
                                      holes[index].size, 0, &output, &result) &&
                  result.error == CACHE_ARENA_CAPACITY &&
                  memcmp(&output, &before, sizeof(output)) == 0,
              "aggregate_free_cannot_satisfy_one_contiguous_request");
    }
    ++context->cases;
    struct cache_arena_plan output;
    CHECK(cache_arena_plan_build(&request, 1, storage->placement[1], 48, 0, &output, &result) &&
              output.required == 32,
          "same_aggregate_as_one_contiguous_interval_fits");
    if (fprintf(context->report,
                "CACHE_ARENA FRAGMENTATION authored_holes=3 aggregate_free=48 largest_free_span=16 "
                "request=32 contiguous_equivalent_fits=1 system_allocator_measured=0\n") < 0)
        return 1;
    return 0;
}

int wii_cache_arena_fixture(FILE *report, int collect)
{
    if (!report)
        return 1;
    struct arena_context context = {report, "initialization", 0, 0, 0, collect};
    struct arena_storage storage;
    memset(&storage, 0, sizeof(storage));
    int aborted = 0;
    storage.placement[0] = malloc(ARENA_STORAGE);
    storage.placement[1] = malloc(ARENA_STORAGE);
    storage.expected = malloc(ARENA_STORAGE);
    if (!storage.placement[0] || !storage.placement[1] || !storage.expected)
    {
        aborted = arena_abort(&context, "fixture_placement_allocation_failed");
        goto done;
    }
    if (planning(&context, &storage) || rejected_plans(&context, &storage) ||
        capacity_prefixes(&context, &storage) || ownership(&context, &storage) ||
        binding_validation(&context, &storage) || generation_exhaustion(&context) ||
        fragmentation(&context, &storage))
        aborted = 1;

done:
    if (storage.owner.live)
    {
        struct cache_arena_result result;
        if (!cache_arena_owner_release(&storage.owner, &result))
        {
            aborted = 1;
            (void)fprintf(report, "CACHE_ARENA ABORT reason=fixture_cleanup_release_failed\n");
        }
    }
    free(storage.expected);
    free(storage.placement[1]);
    free(storage.placement[0]);
    if (fprintf(report,
                "CACHE_ARENA SUMMARY cases=%u checks=%u failures=%u aborted=%d "
                "scope=generic_budget_owner_and_synthetic_placements engine_capacity_reduced=0 "
                "system_fragmentation_measured=0\n",
                context.cases, context.checks, context.failures, aborted) < 0 ||
        fflush(report) != 0)
        return 1;
    return aborted || context.failures ? 1 : 0;
}
