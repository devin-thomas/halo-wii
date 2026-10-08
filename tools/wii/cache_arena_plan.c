#include "cache_arena_plan.h"
#include <string.h>

static int reset_result(struct cache_arena_result *result)
{
    if (!result)
        return 0;
    result->error = CACHE_ARENA_OK;
    result->slot_index = SIZE_MAX;
    result->required = 0;
    return 1;
}

static int reject(struct cache_arena_result *result, enum cache_arena_error error)
{
    result->error = error;
    return 0;
}

static int valid_alignment(size_t alignment)
{
    return alignment && alignment <= 4096 && !(alignment & (alignment - 1));
}

const char *cache_arena_error_name(enum cache_arena_error error)
{
    static const char *const names[] = {"ok",         "argument", "count", "alignment",
                                        "overflow",   "capacity", "plan",  "state",
                                        "generation", "handle",   "span"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

int cache_arena_plan_build(const struct cache_arena_request *requests, size_t count, void *span,
                           size_t capacity, size_t reserve, struct cache_arena_plan *output,
                           struct cache_arena_result *result)
{
    if (!reset_result(result))
        return 0;
    if (!output || !span || (count && !requests))
        return reject(result, CACHE_ARENA_ARGUMENT);
    if (count > CACHE_ARENA_MAX_SLOTS)
        return reject(result, CACHE_ARENA_COUNT);

    uintptr_t base = (uintptr_t)span;
    if (capacity > UINTPTR_MAX - base)
        return reject(result, CACHE_ARENA_OVERFLOW);

    struct cache_arena_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.span_address = base;
    plan.capacity = capacity;
    plan.slot_count = count;
    size_t cursor = 0;

    for (size_t index = 0; index < count; ++index)
    {
        result->slot_index = index;
        size_t alignment = requests[index].alignment;
        if (!valid_alignment(alignment))
            return reject(result, CACHE_ARENA_ALIGNMENT);

        /* Align the actual address while checking each addition before it happens. */
        size_t remainder = (size_t)((base + cursor) & (alignment - 1));
        size_t padding = remainder ? alignment - remainder : 0;
        if (padding > SIZE_MAX - cursor)
            return reject(result, CACHE_ARENA_OVERFLOW);
        size_t offset = cursor + padding;
        if (requests[index].size > SIZE_MAX - offset)
            return reject(result, CACHE_ARENA_OVERFLOW);
        size_t end = offset + requests[index].size;
        result->required = end;
        if (end > capacity)
            return reject(result, CACHE_ARENA_CAPACITY);

        plan.slots[index].offset = offset;
        plan.slots[index].size = requests[index].size;
        plan.slots[index].alignment = alignment;
        cursor = end;
    }

    if (reserve > SIZE_MAX - cursor)
        return reject(result, CACHE_ARENA_OVERFLOW);
    plan.data_end = cursor;
    plan.reserve_offset = cursor;
    plan.reserve_size = reserve;
    plan.required = cursor + reserve;
    result->required = plan.required;
    if (plan.required > capacity)
        return reject(result, CACHE_ARENA_CAPACITY);

    result->slot_index = SIZE_MAX;
    *output = plan;
    return 1;
}

static int complete_plan(const struct cache_arena_plan *plan, void *span, size_t capacity,
                         struct cache_arena_plan *canonical, struct cache_arena_result *result)
{
    if (plan->slot_count > CACHE_ARENA_MAX_SLOTS || plan->span_address != (uintptr_t)span ||
        plan->capacity != capacity)
        return reject(result, CACHE_ARENA_PLAN);

    struct cache_arena_request requests[CACHE_ARENA_MAX_SLOTS];
    for (size_t index = 0; index < plan->slot_count; ++index)
    {
        requests[index].size = plan->slots[index].size;
        requests[index].alignment = plan->slots[index].alignment;
    }
    if (!cache_arena_plan_build(requests, plan->slot_count, span, capacity, plan->reserve_size,
                                canonical, result))
        return reject(result, CACHE_ARENA_PLAN);

    if (plan->data_end != canonical->data_end ||
        plan->reserve_offset != canonical->reserve_offset || plan->required != canonical->required)
        return reject(result, CACHE_ARENA_PLAN);
    for (size_t index = 0; index < plan->slot_count; ++index)
    {
        if (plan->slots[index].offset != canonical->slots[index].offset)
        {
            result->slot_index = index;
            return reject(result, CACHE_ARENA_PLAN);
        }
    }
    return 1;
}

int cache_arena_owner_bind(struct cache_arena_owner *owner, const struct cache_arena_plan *plan,
                           void *span, size_t capacity, struct cache_arena_result *result)
{
    if (!reset_result(result))
        return 0;
    if (!owner || !plan || !span)
        return reject(result, CACHE_ARENA_ARGUMENT);
    if (owner->live || owner->span || owner->capacity)
        return reject(result, CACHE_ARENA_STATE);
    if (owner->generation == UINT64_MAX)
        return reject(result, CACHE_ARENA_GENERATION);

    struct cache_arena_plan canonical;
    if (!complete_plan(plan, span, capacity, &canonical, result))
        return 0;

    struct cache_arena_owner next = *owner;
    next.span = span;
    next.capacity = capacity;
    next.generation = owner->generation + 1;
    next.live = 1;
    next.plan = canonical;
    *owner = next;
    return 1;
}

int cache_arena_owner_release(struct cache_arena_owner *owner, struct cache_arena_result *result)
{
    if (!reset_result(result))
        return 0;
    if (!owner)
        return reject(result, CACHE_ARENA_ARGUMENT);
    if (!owner->live)
        return 1;
    if (owner->generation == UINT64_MAX)
        return reject(result, CACHE_ARENA_GENERATION);

    uint64_t generation = owner->generation + 1;
    memset(owner, 0, sizeof(*owner));
    owner->generation = generation;
    return 1;
}

int cache_arena_owner_handle(const struct cache_arena_owner *owner, size_t slot_index,
                             struct cache_arena_handle *output, struct cache_arena_result *result)
{
    if (!reset_result(result))
        return 0;
    if (!owner || !output)
        return reject(result, CACHE_ARENA_ARGUMENT);
    if (!owner->live || !owner->span)
        return reject(result, CACHE_ARENA_STATE);
    if (slot_index >= owner->plan.slot_count)
        return reject(result, CACHE_ARENA_HANDLE);

    struct cache_arena_handle handle = {owner->generation, slot_index};
    *output = handle;
    return 1;
}

int cache_arena_owner_resolve(const struct cache_arena_owner *owner,
                              const struct cache_arena_handle *handle, size_t offset, size_t length,
                              unsigned char **output, struct cache_arena_result *result)
{
    if (!reset_result(result))
        return 0;
    if (!owner || !handle || !output)
        return reject(result, CACHE_ARENA_ARGUMENT);
    if (!owner->live || !owner->span)
        return reject(result, CACHE_ARENA_STATE);
    if (handle->generation != owner->generation || handle->slot_index >= owner->plan.slot_count)
        return reject(result, CACHE_ARENA_HANDLE);

    result->slot_index = handle->slot_index;
    const struct cache_arena_slot *slot = &owner->plan.slots[handle->slot_index];
    if (offset > slot->size || length > slot->size - offset)
        return reject(result, CACHE_ARENA_SPAN);
    *output = owner->span + slot->offset + offset;
    return 1;
}
