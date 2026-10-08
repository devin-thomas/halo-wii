#ifndef WII_CACHE_ARENA_PLAN_H
#define WII_CACHE_ARENA_PLAN_H
#include <stddef.h>
#include <stdint.h>

#define CACHE_ARENA_MAX_SLOTS 16

enum cache_arena_error
{
    CACHE_ARENA_OK,
    CACHE_ARENA_ARGUMENT,
    CACHE_ARENA_COUNT,
    CACHE_ARENA_ALIGNMENT,
    CACHE_ARENA_OVERFLOW,
    CACHE_ARENA_CAPACITY,
    CACHE_ARENA_PLAN,
    CACHE_ARENA_STATE,
    CACHE_ARENA_GENERATION,
    CACHE_ARENA_HANDLE,
    CACHE_ARENA_SPAN
};

struct cache_arena_request
{
    size_t size;
    size_t alignment;
};

struct cache_arena_slot
{
    size_t offset;
    size_t size;
    size_t alignment;
};

struct cache_arena_plan
{
    uintptr_t span_address;
    size_t capacity;
    size_t slot_count;
    struct cache_arena_slot slots[CACHE_ARENA_MAX_SLOTS];
    size_t data_end;
    size_t reserve_offset;
    size_t reserve_size;
    size_t required;
};

struct cache_arena_result
{
    enum cache_arena_error error;
    size_t slot_index;
    size_t required;
};

struct cache_arena_owner
{
    unsigned char *span;
    size_t capacity;
    uint64_t generation;
    int live;
    struct cache_arena_plan plan;
};

struct cache_arena_handle
{
    uint64_t generation;
    size_t slot_index;
};

const char *cache_arena_error_name(enum cache_arena_error);

/* Inputs describe truthful accessible storage. Outputs/results/control objects
 * must be separate from data and each other. Planning never reads or writes
 * storage. Alignment uses the actual span address, not a guessed target.
 * Reserve follows data_end, and required charges all padding/data/reserve.
 * Unused capacity beyond required remains uncharged. Rejection is atomic. */
int cache_arena_plan_build(const struct cache_arena_request *, size_t count, void *span,
                           size_t capacity, size_t reserve, struct cache_arena_plan *,
                           struct cache_arena_result *);

/* The complete immutable plan must be bound to the same live caller span and
 * capacity. Initialize owner to zero; its control address remains live across
 * release/rebind. Successful bind and active release each advance generation.
 * Exhaustion rejects atomically; failed release retains the live span, which
 * the caller must retain. An inactive release is idempotent. No allocator or
 * engine ownership is implied; the caller frees storage after release. */
int cache_arena_owner_bind(struct cache_arena_owner *, const struct cache_arena_plan *, void *span,
                           size_t capacity, struct cache_arena_result *);
int cache_arena_owner_release(struct cache_arena_owner *, struct cache_arena_result *);
int cache_arena_owner_handle(const struct cache_arena_owner *, size_t slot_index,
                             struct cache_arena_handle *, struct cache_arena_result *);

/* Handles are numeric generation/index pairs, never retained native pointers.
 * Use each handle only with its originating persistent owner control; these
 * pairs do not authenticate one owner against another with the same generation.
 * Resolve only within a live matching slot; reserve is never a slot. A zero
 * length at the slot's end is allowed, without permission to dereference it.
 * Owner/plan remain immutable while resolved views are used; discard all views
 * before release/rebind. Previously returned pointers are not lifetime-safe. */
int cache_arena_owner_resolve(const struct cache_arena_owner *, const struct cache_arena_handle *,
                              size_t offset, size_t length, unsigned char **,
                              struct cache_arena_result *);
#endif
