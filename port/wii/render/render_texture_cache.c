/*
 * RENDER_TEXTURE_CACHE.C
 *
 * HWI-083: the environment renderer's bounded base map residency
 * (render_texture_cache.h). Placement is per region: blocks ordered by
 * offset, each a loading or resident entry's. A reservation looks for the
 * window of free bytes and evictable entries that fits with the least loss,
 * and waits for the GPU only if the window holds bytes it may still read.
 */
#include <string.h>

#include "render_texture_cache.h"

typedef struct wii_texture_cache cache_t;
typedef struct wii_texture_cache_entry entry_t;
typedef struct wii_texture_cache_block block_t;

/* a is after b, modulo 2^32 (frames and GPU sequences wrap) */
static int after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static uint32_t align(uint32_t bytes)
{
    return (bytes + (WII_TEXTURE_CACHE_ALIGN - 1u)) & ~(WII_TEXTURE_CACHE_ALIGN - 1u);
}

int wii_texture_cache_blocks_per_region(int entry_count)
{
    /* an entry has at most one block, and a reservation inserts its own
    before it removes the window's */
    return entry_count < 0 ? 0 : entry_count + 1;
}

const char *wii_texture_cache_refusal_name(int refusal)
{
    static const char *const names[NUMBER_OF_WII_TEXTURE_CACHE_REFUSALS] = {"none", "missing", "oversize", "invalid",
                                                                             "failures"};
    return refusal >= 0 && refusal < NUMBER_OF_WII_TEXTURE_CACHE_REFUSALS ? names[refusal] : "unknown";
}

int wii_texture_cache_init(cache_t *cache, entry_t *entries, int entry_count, block_t *blocks, int blocks_per_region,
                           unsigned char *const base[WII_TEXTURE_CACHE_REGIONS],
                           const uint32_t bytes[WII_TEXTURE_CACHE_REGIONS], uint32_t completed,
                           wii_texture_cache_wait_proc wait, void *wait_context)
{
    uint32_t generation = cache->generation + 1u;
    memset(cache, 0, sizeof(*cache));
    cache->generation = generation;
    cache->frame = 1;
    cache->completed = completed;
    if (entry_count < 0 || (entry_count && !entries) || !wait ||
        blocks_per_region < wii_texture_cache_blocks_per_region(entry_count) || !blocks)
        return 0;
    for (int r = 0; r < WII_TEXTURE_CACHE_REGIONS; ++r) {
        uint32_t usable = bytes[r] & ~(WII_TEXTURE_CACHE_ALIGN - 1u);
        if (usable && (!base[r] || ((uintptr_t)base[r] & (WII_TEXTURE_CACHE_ALIGN - 1u))))
            return 0;
    }
    memset(entries, 0, (size_t)entry_count * sizeof(*entries));
    for (int i = 0; i < entry_count; ++i) {
        entries[i].state = _wii_texture_cache_refused;
        entries[i].refusal = _wii_texture_cache_refusal_missing;
        entries[i].region = -1;
    }
    cache->entries = entries;
    cache->entry_count = entry_count;
    cache->block_capacity = blocks_per_region;
    for (int r = 0; r < WII_TEXTURE_CACHE_REGIONS; ++r) {
        struct wii_texture_cache_region *region = &cache->regions[r];
        region->bytes = bytes[r] & ~(WII_TEXTURE_CACHE_ALIGN - 1u);
        region->base = region->bytes ? base[r] : NULL;
        region->blocks = blocks + (size_t)r * (size_t)blocks_per_region;
        region->block_count = 0;
    }
    cache->wait = wait;
    cache->wait_context = wait_context;
    return 1;
}

void wii_texture_cache_entry_set(cache_t *cache, int index, long tag, long bitmap, int found, uint32_t bytes,
                                 long coverage)
{
    if (index < 0 || index >= cache->entry_count)
        return;
    entry_t *e = &cache->entries[index];
    uint32_t largest = 0;
    memset(e, 0, sizeof(*e));
    e->tag = tag;
    e->bitmap = bitmap;
    e->bytes = bytes;
    e->span = align(bytes);
    e->coverage = coverage;
    e->region = -1;
    for (int r = 0; r < WII_TEXTURE_CACHE_REGIONS; ++r)
        if (cache->regions[r].bytes > largest)
            largest = cache->regions[r].bytes;
    e->state = _wii_texture_cache_refused;
    if (!found)
        e->refusal = _wii_texture_cache_refusal_missing;
    else if (bytes == 0)
        e->refusal = _wii_texture_cache_refusal_invalid;
    else if (e->span < bytes || e->span > largest)
        e->refusal = _wii_texture_cache_refusal_oversize;
    else {
        e->state = _wii_texture_cache_absent;
        e->refusal = _wii_texture_cache_refusal_none;
    }
}

void wii_texture_cache_frame_begin(cache_t *cache)
{
    cache->frame++;
    if (cache->frame == 0)  /* 0 is "never wanted" */
        cache->frame = 1;
    for (int i = 0; i < cache->entry_count; ++i)
        cache->entries[i].demand = 0;
}

void wii_texture_cache_want(cache_t *cache, int index, unsigned long surfaces)
{
    if (index < 0 || index >= cache->entry_count || !surfaces)
        return;
    entry_t *e = &cache->entries[index];
    e->demand += surfaces;
    e->last_wanted = cache->frame;
}

static int loadable(const cache_t *cache, const entry_t *e)
{
    return e->state == _wii_texture_cache_absent ||
           (e->state == _wii_texture_cache_failed && !after(e->retry_frame, cache->frame));
}

int wii_texture_cache_next_wanted(const cache_t *cache)
{
    int best = -1;
    for (int i = 0; i < cache->entry_count; ++i) {
        const entry_t *e = &cache->entries[i];
        if (!e->demand || e->skip_frame == cache->frame || !loadable(cache, e))
            continue;
        if (best < 0 || e->demand > cache->entries[best].demand ||
            (e->demand == cache->entries[best].demand && e->coverage > cache->entries[best].coverage))
            best = i;
    }
    return best;
}

int wii_texture_cache_resident(const cache_t *cache, int index)
{
    return index >= 0 && index < cache->entry_count && cache->entries[index].state == _wii_texture_cache_resident;
}

void wii_texture_cache_used(cache_t *cache, int index, uint32_t sequence)
{
    if (wii_texture_cache_resident(cache, index))
        cache->entries[index].fence = sequence;
}

void wii_texture_cache_gpu_done(cache_t *cache, uint32_t sequence)
{
    if (after(sequence, cache->completed))
        cache->completed = sequence;
}

static void wait_gpu(cache_t *cache)
{
    cache->stats.waits++;
    wii_texture_cache_gpu_done(cache, cache->wait(cache->wait_context));
}

/* a window's loss: windows that evict nothing come first (those that need
no wait first among them); then the one whose newest evicted entry was
wanted least recently, then the fewest bytes evicted, then no wait, then the
tightest fit */
struct window {
    int region, first, end;  /* blocks [first, end) are removed */
    uint32_t start, size;
    int evicts, waits;
    uint32_t newest, evicted_bytes;
};

static int better(const cache_t *cache, const struct window *a, const struct window *b)
{
    if ((a->evicts > 0) != (b->evicts > 0))
        return a->evicts == 0;
    if (a->evicts) {
        if (a->newest != b->newest)
            return after(b->newest, a->newest);
        if (a->evicted_bytes != b->evicted_bytes)
            return a->evicted_bytes < b->evicted_bytes;
    }
    if (a->waits != b->waits)
        return a->waits < b->waits;
    (void)cache;
    return a->size < b->size;
}

/* the block's entry may be evicted into a window */
static int removable(const cache_t *cache, const block_t *block, int may_evict, uint32_t keep_frames)
{
    const entry_t *e = &cache->entries[block->entry];
    if (!may_evict || e->state != _wii_texture_cache_resident || e->demand)
        return 0;
    if (e->last_wanted == 0)
        return 1;
    return e->last_wanted != cache->frame && cache->frame - e->last_wanted >= keep_frames;
}

static int find_window(const cache_t *cache, uint32_t need, int may_evict, uint32_t keep_frames, struct window *best)
{
    int found = 0;
    for (int r = 0; r < WII_TEXTURE_CACHE_REGIONS; ++r) {
        const struct wii_texture_cache_region *region = &cache->regions[r];
        if (region->bytes < need)
            continue;
        for (int s = 0; s <= region->block_count; ++s) {
            struct window w;
            memset(&w, 0, sizeof(w));
            w.region = r;
            w.first = s;
            w.start = s == 0 ? 0 : region->blocks[s - 1].offset + region->blocks[s - 1].span;
            for (int k = s;; ++k) {
                uint32_t end = k < region->block_count ? region->blocks[k].offset : region->bytes;
                if (end - w.start >= need) {
                    w.end = k;
                    w.size = end - w.start;
                    if (!found || better(cache, &w, best))
                        *best = w;
                    found = 1;
                    break;
                }
                if (k == region->block_count)
                    break;
                const block_t *block = &region->blocks[k];
                if (!removable(cache, block, may_evict, keep_frames))
                    break;
                const entry_t *e = &cache->entries[block->entry];
                if (after(e->fence, cache->completed))
                    w.waits = 1;
                if (!w.evicts || after(e->last_wanted, w.newest))
                    w.newest = e->last_wanted;
                w.evicts++;
                w.evicted_bytes += block->span;
            }
        }
    }
    return found;
}

enum wii_texture_cache_result wii_texture_cache_reserve(cache_t *cache, int index, int may_evict, uint32_t keep_frames,
                                                        struct wii_texture_cache_ticket *ticket)
{
    struct window w;
    memset(&w, 0, sizeof(w));
    memset(ticket, 0, sizeof(*ticket));
    ticket->entry = -1;
    if (index < 0 || index >= cache->entry_count || !loadable(cache, &cache->entries[index]))
        return _wii_texture_cache_not_loadable;
    entry_t *e = &cache->entries[index];
    if (!find_window(cache, e->span, may_evict, keep_frames, &w)) {
        e->skip_frame = cache->frame;
        cache->stats.no_space++;
        return _wii_texture_cache_no_space;
    }
    struct wii_texture_cache_region *region = &cache->regions[w.region];
    /* evict what the window holds. Its bytes are reused at once, so if the
    GPU may still read any of them (a fence after the completed sequence) it
    is waited for before they are handed out. The tail of the last evicted
    block beyond the new one becomes free: after that wait, or with every
    fence complete, the GPU reads none of it either */
    for (int b = w.first; b < w.end; ++b) {
        entry_t *victim = &cache->entries[region->blocks[b].entry];
        victim->state = _wii_texture_cache_absent;
        victim->data = NULL;
        victim->region = -1;
        cache->stats.used -= region->blocks[b].span;
        cache->stats.evictions++;
    }
    memmove(&region->blocks[w.first + 1], &region->blocks[w.end],
            (size_t)(region->block_count - w.end) * sizeof(block_t));
    region->block_count += 1 - (w.end - w.first);
    region->blocks[w.first].entry = index;
    region->blocks[w.first].offset = w.start;
    region->blocks[w.first].span = e->span;
    if (w.waits)
        wait_gpu(cache);
    e->state = _wii_texture_cache_loading;
    e->region = w.region;
    e->offset = w.start;
    e->data = region->base + w.start;
    e->fence = cache->completed;
    cache->stats.used += e->span;
    if (cache->stats.used > cache->stats.used_peak)
        cache->stats.used_peak = cache->stats.used;
    cache->stats.reserves++;
    ticket->entry = index;
    ticket->generation = cache->generation;
    ticket->data = e->data;
    ticket->bytes = e->bytes;
    return _wii_texture_cache_ok;
}

/* the ticket names this generation's loading entry at its bytes */
static entry_t *ticket_entry(cache_t *cache, const struct wii_texture_cache_ticket *ticket)
{
    if (!ticket || ticket->generation != cache->generation || ticket->entry < 0 ||
        ticket->entry >= cache->entry_count)
        return NULL;
    entry_t *e = &cache->entries[ticket->entry];
    if (e->state != _wii_texture_cache_loading || e->data != ticket->data)
        return NULL;
    return e;
}

/* a loading entry's block, never bound by a draw, freed at once */
static void unplace(cache_t *cache, entry_t *e)
{
    struct wii_texture_cache_region *region = &cache->regions[e->region];
    for (int b = 0; b < region->block_count; ++b)
        if (region->blocks[b].entry == (int)(e - cache->entries)) {
            memmove(&region->blocks[b], &region->blocks[b + 1],
                    (size_t)(region->block_count - b - 1) * sizeof(block_t));
            region->block_count--;
            break;
        }
    cache->stats.used -= e->span;
    e->data = NULL;
    e->region = -1;
}

int wii_texture_cache_publish(cache_t *cache, const struct wii_texture_cache_ticket *ticket)
{
    entry_t *e = ticket_entry(cache, ticket);
    if (!e) {
        cache->stats.stale++;
        return 0;
    }
    e->state = _wii_texture_cache_resident;
    e->failures = 0;
    cache->stats.publishes++;
    cache->stats.bytes_published += e->bytes;
    return 1;
}

void wii_texture_cache_abandon(cache_t *cache, const struct wii_texture_cache_ticket *ticket, int transient)
{
    entry_t *e = ticket_entry(cache, ticket);
    if (!e) {
        cache->stats.stale++;
        return;
    }
    unplace(cache, e);
    cache->stats.abandons++;
    if (transient && ++e->failures < WII_TEXTURE_CACHE_MAX_FAILURES) {
        e->state = _wii_texture_cache_failed;
        e->retry_frame = cache->frame + WII_TEXTURE_CACHE_RETRY_FRAMES;
        return;
    }
    e->state = _wii_texture_cache_refused;
    e->refusal = transient ? _wii_texture_cache_refusal_failures : _wii_texture_cache_refusal_invalid;
}

int wii_texture_cache_check(const cache_t *cache)
{
    uint32_t used = 0, total = 0;
    for (int r = 0; r < WII_TEXTURE_CACHE_REGIONS; ++r) {
        const struct wii_texture_cache_region *region = &cache->regions[r];
        uint32_t at = 0;
        total += region->bytes;
        if (region->block_count < 0 || region->block_count > cache->block_capacity)
            return 0;
        for (int b = 0; b < region->block_count; ++b) {
            const block_t *block = &region->blocks[b];
            if (block->offset < at || block->span == 0 || (block->offset & (WII_TEXTURE_CACHE_ALIGN - 1u)) ||
                block->span > region->bytes || block->offset > region->bytes - block->span)
                return 0;
            at = block->offset + block->span;
            if (block->entry < 0 || block->entry >= cache->entry_count)
                return 0;
            const entry_t *e = &cache->entries[block->entry];
            if ((e->state != _wii_texture_cache_loading && e->state != _wii_texture_cache_resident) ||
                e->region != r || e->offset != block->offset || e->span != block->span ||
                e->data != region->base + block->offset || e->span < e->bytes)
                return 0;
            used += block->span;
        }
    }
    for (int i = 0; i < cache->entry_count; ++i) {
        const entry_t *e = &cache->entries[i];
        int placed = 0;
        if (e->region >= 0 && e->region < WII_TEXTURE_CACHE_REGIONS) {
            const struct wii_texture_cache_region *region = &cache->regions[e->region];
            for (int b = 0; b < region->block_count; ++b)
                placed += region->blocks[b].entry == i;
        }
        int live = e->state == _wii_texture_cache_loading || e->state == _wii_texture_cache_resident;
        if (placed != live || (!live && e->data))
            return 0;
    }
    return used == cache->stats.used && used <= total;
}
