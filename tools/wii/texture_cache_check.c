/*
 * HWI-083: the environment renderer's base map cache
 * (port/wii/render/render_texture_cache.c, the unit render_gx.c links), on
 * the host. Each case drives the production cache directly. The GPU is
 * simulated: a draw binds resident entries under a sequence number, and a
 * byte the GPU may still read is one inside an entry whose sequence has not
 * completed. Prints "ok <cases> cases" and exits 0, or names the first failure.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render_texture_cache.h"

#define MAX_ENTRIES 64
#define REGION_MAX 8192u
#define UNIT 32u

static int cases, failures;

#define CHECK(condition)                                                                         \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            fprintf(stderr, "%s:%d: %s: failed: %s\n", __FILE__, __LINE__, current, #condition); \
            failures++;                                                                          \
            return;                                                                              \
        }                                                                                        \
    } while (0)

static const char *current = "";

/* the fixture: two regions of the arena, the tables, a simulated GPU */
static _Alignas(32) unsigned char arena[2][REGION_MAX];
static struct wii_texture_cache cache;
static struct wii_texture_cache_entry entries[MAX_ENTRIES];
static struct wii_texture_cache_block blocks[2 * (MAX_ENTRIES + 1)];
static uint32_t submitted, completed_seen;
static int waits;
/* per 32 bytes of each region: the sequence of the last draw that bound an
entry over them (the GPU reads them until that sequence completes) */
static uint32_t reads_until[2][REGION_MAX / UNIT];

static uint32_t gpu_wait(void *context)
{
    (void)context;
    waits++;
    completed_seen = submitted;
    return submitted;
}

static int setup(int count, uint32_t bytes0, uint32_t bytes1)
{
    unsigned char *base[2] = {arena[0], arena[1]};
    uint32_t bytes[2] = {bytes0, bytes1};
    submitted = completed_seen = 0;
    waits = 0;
    memset(reads_until, 0, sizeof(reads_until));
    return wii_texture_cache_init(&cache, entries, count, blocks, MAX_ENTRIES + 1, base, bytes, 0, gpu_wait, NULL);
}

/* a draw of every resident entry with demand (as render_gx.c binds them) */
static void draw(void)
{
    uint32_t sequence = ++submitted;
    for (int i = 0; i < cache.entry_count; ++i) {
        const struct wii_texture_cache_entry *e = &cache.entries[i];
        if (!wii_texture_cache_resident(&cache, i) || !e->demand)
            continue;
        wii_texture_cache_used(&cache, i, sequence);
        for (uint32_t at = e->offset; at < e->offset + e->span; at += UNIT)
            reads_until[e->region][at / UNIT] = sequence;
    }
}

/* the GPU finished (render_gx.c's draw_done) */
static void gpu_done(void)
{
    completed_seen = submitted;
    wii_texture_cache_gpu_done(&cache, submitted);
}

/* the ticket's bytes: none the GPU may still read */
static int ticket_safe(const struct wii_texture_cache_ticket *t)
{
    for (int r = 0; r < 2; ++r) {
        const unsigned char *base = arena[r];
        if (t->data < base || t->data >= base + REGION_MAX)
            continue;
        uint32_t offset = (uint32_t)(t->data - base);
        for (uint32_t at = offset; at < offset + t->bytes; at += UNIT)
            if ((int32_t)(reads_until[r][at / UNIT] - cache.completed) > 0)
                return 0;
        return 1;
    }
    return 0;
}

static enum wii_texture_cache_result load(int index, int may_evict, uint32_t keep, int publish)
{
    struct wii_texture_cache_ticket ticket;
    enum wii_texture_cache_result result = wii_texture_cache_reserve(&cache, index, may_evict, keep, &ticket);
    if (result == _wii_texture_cache_ok) {
        /* the content is written here, then checked; only then published */
        memset(ticket.data, 0xA0 + index, ticket.bytes);
        if (publish)
            wii_texture_cache_publish(&cache, &ticket);
    }
    return result;
}

static void test_init_refuses_unusable_arguments(void)
{
    unsigned char *base[2] = {arena[0], arena[1]};
    unsigned char *odd[2] = {arena[0] + 4, NULL};
    uint32_t bytes[2] = {1024, 0};
    current = "init";
    cases++;
    CHECK(!wii_texture_cache_init(&cache, entries, 4, blocks, 1, base, bytes, 0, gpu_wait, NULL));
    CHECK(!wii_texture_cache_init(&cache, entries, 4, blocks, 5, base, bytes, 0, NULL, NULL));
    CHECK(!wii_texture_cache_init(&cache, entries, 4, blocks, 5, odd, bytes, 0, gpu_wait, NULL));
    CHECK(cache.entry_count == 0);
    CHECK(wii_texture_cache_init(&cache, entries, 4, blocks, 5, base, bytes, 0, gpu_wait, NULL));
    CHECK(wii_texture_cache_blocks_per_region(4) == 5);
}

static void test_memory_limit_and_refusals(void)
{
    current = "memory_limit";
    cases++;
    CHECK(setup(8, 1024, 512));
    wii_texture_cache_entry_set(&cache, 0, 100, 0, 1, 600, 50);   /* fits region 0 only */
    wii_texture_cache_entry_set(&cache, 1, 101, 0, 1, 400, 40);
    wii_texture_cache_entry_set(&cache, 2, 102, 0, 1, 500, 30);   /* 512 aligned */
    wii_texture_cache_entry_set(&cache, 3, 103, 0, 1, 1100, 20);  /* larger than every region: oversize */
    wii_texture_cache_entry_set(&cache, 4, 104, 0, 0, 0, 10);     /* not staged */
    wii_texture_cache_entry_set(&cache, 5, 105, 0, 1, 0, 10);     /* empty file */
    wii_texture_cache_entry_set(&cache, 6, 106, 0, 1, 0xFFFFFFFFu, 10);  /* beyond MAX_FILE_BYTES */
    wii_texture_cache_entry_set(&cache, 7, 107, 0, 1, 100, 5);
    CHECK(cache.entries[3].refusal == _wii_texture_cache_refusal_oversize);
    CHECK(cache.entries[4].refusal == _wii_texture_cache_refusal_missing);
    CHECK(cache.entries[5].refusal == _wii_texture_cache_refusal_invalid);
    CHECK(cache.entries[6].refusal == _wii_texture_cache_refusal_oversize);
    CHECK(load(3, 1, 0, 1) == _wii_texture_cache_not_loadable);
    CHECK(load(4, 1, 0, 1) == _wii_texture_cache_not_loadable);
    CHECK(load(0, 0, 0, 1) == _wii_texture_cache_ok);
    CHECK(load(1, 0, 0, 1) == _wii_texture_cache_ok);
    CHECK(load(2, 0, 0, 1) == _wii_texture_cache_ok);
    CHECK(cache.stats.used == 608 + 416 + 512);
    CHECK(load(7, 0, 0, 1) == _wii_texture_cache_no_space);  /* 1536 of 1536 used */
    CHECK(cache.stats.used <= 1536 && cache.stats.used_peak <= 1536);
    CHECK(wii_texture_cache_check(&cache));
    /* every resident entry's bytes lie inside its region */
    for (int i = 0; i < 3; ++i) {
        const struct wii_texture_cache_entry *e = &cache.entries[i];
        CHECK(e->data >= arena[e->region] && e->data + e->span <= arena[e->region] + (e->region ? 512 : 1024));
        CHECK(((uintptr_t)e->data & 31u) == 0);
    }
    /* a resident entry's bytes are never disturbed by the next reservation */
    CHECK(cache.entries[0].data[0] == 0xA0 && cache.entries[1].data[399] == 0xA1);
}

static void test_eviction_prefers_unwanted_and_oldest(void)
{
    current = "eviction";
    cases++;
    CHECK(setup(5, 1024, 0));
    for (int i = 0; i < 5; ++i)
        wii_texture_cache_entry_set(&cache, i, 200 + i, 0, 1, 256, 10 - i);
    for (int i = 0; i < 4; ++i)
        CHECK(load(i, 0, 0, 1) == _wii_texture_cache_ok);
    /* frames: 0 and 1 wanted long ago, 2 recently, 3 now; 4 wanted now */
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 0, 1);
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 1, 1);
    for (int f = 0; f < 40; ++f)
        wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 2, 1);
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 3, 5);
    wii_texture_cache_want(&cache, 4, 9);
    CHECK(wii_texture_cache_next_wanted(&cache) == 4);
    /* nothing may be evicted without may_evict */
    CHECK(load(4, 0, 30, 1) == _wii_texture_cache_no_space);
    CHECK(wii_texture_cache_next_wanted(&cache) == -1);  /* not offered again this frame */
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 3, 5);
    wii_texture_cache_want(&cache, 4, 9);
    CHECK(load(4, 1, 30, 1) == _wii_texture_cache_ok);
    CHECK(!wii_texture_cache_resident(&cache, 0));        /* the least recently wanted went */
    CHECK(wii_texture_cache_resident(&cache, 1) && wii_texture_cache_resident(&cache, 2));
    CHECK(wii_texture_cache_resident(&cache, 3));         /* visible: never evicted */
    CHECK(cache.stats.evictions == 1);
    CHECK(wii_texture_cache_check(&cache));
    /* entry 2 was wanted 2 frames ago: kept with keep 30, taken with keep 1 */
    wii_texture_cache_entry_set(&cache, 0, 200, 0, 1, 500, 10);  /* now needs two blocks' bytes */
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 0, 1);
    wii_texture_cache_want(&cache, 3, 1);
    wii_texture_cache_want(&cache, 4, 1);
    CHECK(load(0, 1, 30, 1) == _wii_texture_cache_no_space);
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 0, 1);
    wii_texture_cache_want(&cache, 3, 1);
    wii_texture_cache_want(&cache, 4, 1);
    CHECK(load(0, 1, 1, 1) == _wii_texture_cache_ok);
    CHECK(wii_texture_cache_resident(&cache, 3) && wii_texture_cache_resident(&cache, 4));
    CHECK(!wii_texture_cache_resident(&cache, 1) && !wii_texture_cache_resident(&cache, 2));
    CHECK(wii_texture_cache_check(&cache));
}

/* the pose checks' settle (keep_frames 0) may evict anything not visible
now, and still nothing visible */
static void test_visible_entries_are_never_evicted(void)
{
    current = "visible_kept";
    cases++;
    CHECK(setup(3, 512, 0));
    for (int i = 0; i < 3; ++i)
        wii_texture_cache_entry_set(&cache, i, 250 + i, 0, 1, 256, 1);
    CHECK(load(0, 0, 0, 1) == _wii_texture_cache_ok);
    CHECK(load(1, 0, 0, 1) == _wii_texture_cache_ok);
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 0, 1);
    wii_texture_cache_want(&cache, 1, 1);
    wii_texture_cache_want(&cache, 2, 1);
    CHECK(load(2, 1, 0, 1) == _wii_texture_cache_no_space);
    CHECK(wii_texture_cache_resident(&cache, 0) && wii_texture_cache_resident(&cache, 1));
    /* wanted earlier in this frame without demand now is still this frame's */
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 2, 1);
    cache.entries[0].last_wanted = cache.frame;
    CHECK(load(2, 1, 0, 1) == _wii_texture_cache_ok);
    CHECK(wii_texture_cache_resident(&cache, 0) && !wii_texture_cache_resident(&cache, 1));
    CHECK(wii_texture_cache_check(&cache));
}

static void test_duplicate_references_share_one_entry(void)
{
    current = "duplicates";
    cases++;
    CHECK(setup(3, 1024, 0));
    wii_texture_cache_entry_set(&cache, 0, 300, 1, 1, 256, 1);
    wii_texture_cache_entry_set(&cache, 1, 301, 1, 1, 256, 100);
    wii_texture_cache_frame_begin(&cache);
    /* three materials share entry 0, one has entry 1 */
    wii_texture_cache_want(&cache, 0, 20);
    wii_texture_cache_want(&cache, 0, 20);
    wii_texture_cache_want(&cache, 0, 20);
    wii_texture_cache_want(&cache, 1, 50);
    wii_texture_cache_want(&cache, 2, 0);  /* a material with nothing visible wants nothing */
    CHECK(cache.entries[0].demand == 60);
    CHECK(wii_texture_cache_next_wanted(&cache) == 0);
    CHECK(load(0, 1, 30, 1) == _wii_texture_cache_ok);
    CHECK(wii_texture_cache_next_wanted(&cache) == 1);
    CHECK(load(0, 1, 30, 1) == _wii_texture_cache_not_loadable);  /* already resident: one copy */
    CHECK(cache.stats.reserves == 1 && cache.stats.used == 256);
    CHECK(cache.entries[2].last_wanted == 0);
}

static void test_gpu_fence_waits_before_reuse(void)
{
    struct wii_texture_cache_ticket ticket;
    current = "fence";
    cases++;
    CHECK(setup(3, 512, 0));
    for (int i = 0; i < 3; ++i)
        wii_texture_cache_entry_set(&cache, i, 400 + i, 0, 1, 512, 3 - i);
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 0, 1);
    CHECK(load(0, 1, 0, 1) == _wii_texture_cache_ok);
    draw();  /* sequence 1 binds entry 0; the GPU has not completed it */
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 1, 1);
    CHECK(wii_texture_cache_reserve(&cache, 1, 1, 0, &ticket) == _wii_texture_cache_ok);
    CHECK(waits == 1);                 /* the bytes of entry 0 were waited for */
    CHECK(cache.completed == 1);
    CHECK(ticket_safe(&ticket));
    CHECK(!wii_texture_cache_resident(&cache, 0) && !wii_texture_cache_resident(&cache, 1));  /* not yet checked */
    CHECK(wii_texture_cache_publish(&cache, &ticket));
    draw();
    gpu_done();                        /* the frame's GX_DrawDone */
    wii_texture_cache_frame_begin(&cache);
    wii_texture_cache_want(&cache, 2, 1);
    CHECK(load(2, 1, 0, 1) == _wii_texture_cache_ok);
    CHECK(waits == 1);                 /* complete: no wait */
    CHECK(wii_texture_cache_check(&cache));
}

static void test_failed_loads_are_given_back_and_bounded(void)
{
    struct wii_texture_cache_ticket ticket;
    current = "failed_loads";
    cases++;
    CHECK(setup(2, 1024, 0));
    wii_texture_cache_entry_set(&cache, 0, 500, 0, 1, 512, 1);
    wii_texture_cache_entry_set(&cache, 1, 501, 0, 1, 512, 1);
    for (int attempt = 0; attempt < WII_TEXTURE_CACHE_MAX_FAILURES; ++attempt) {
        wii_texture_cache_frame_begin(&cache);
        wii_texture_cache_want(&cache, 0, 1);
        CHECK(wii_texture_cache_next_wanted(&cache) == 0);
        CHECK(wii_texture_cache_reserve(&cache, 0, 1, 0, &ticket) == _wii_texture_cache_ok);
        wii_texture_cache_abandon(&cache, &ticket, 1);  /* a short read */
        CHECK(cache.stats.used == 0);
        CHECK(!wii_texture_cache_resident(&cache, 0));
        CHECK(wii_texture_cache_check(&cache));
        /* backing off: not offered again until retry_frame */
        wii_texture_cache_frame_begin(&cache);
        wii_texture_cache_want(&cache, 0, 1);
        if (attempt + 1 < WII_TEXTURE_CACHE_MAX_FAILURES) {
            CHECK(cache.entries[0].state == _wii_texture_cache_failed);
            CHECK(wii_texture_cache_next_wanted(&cache) == -1);
            CHECK(load(0, 1, 0, 1) == _wii_texture_cache_not_loadable);
            for (uint32_t f = 0; f < WII_TEXTURE_CACHE_RETRY_FRAMES; ++f)
                wii_texture_cache_frame_begin(&cache);
        }
    }
    CHECK(cache.entries[0].state == _wii_texture_cache_refused);
    CHECK(cache.entries[0].refusal == _wii_texture_cache_refusal_failures);
    CHECK(wii_texture_cache_next_wanted(&cache) == -1);
    /* content that fails its checks is refused at once, its bytes back */
    CHECK(wii_texture_cache_reserve(&cache, 1, 1, 0, &ticket) == _wii_texture_cache_ok);
    wii_texture_cache_abandon(&cache, &ticket, 0);
    CHECK(cache.entries[1].refusal == _wii_texture_cache_refusal_invalid);
    CHECK(!wii_texture_cache_publish(&cache, &ticket));  /* an abandoned ticket never publishes */
    CHECK(cache.stats.used == 0 && cache.stats.abandons == WII_TEXTURE_CACHE_MAX_FAILURES + 1);
    CHECK(wii_texture_cache_check(&cache));
}

static void test_stale_generation_never_publishes(void)
{
    struct wii_texture_cache_ticket ticket;
    current = "stale_generation";
    cases++;
    CHECK(setup(1, 1024, 0));
    wii_texture_cache_entry_set(&cache, 0, 600, 0, 1, 256, 1);
    CHECK(wii_texture_cache_reserve(&cache, 0, 0, 0, &ticket) == _wii_texture_cache_ok);
    uint32_t old_generation = cache.generation;
    CHECK(setup(1, 1024, 0));  /* a BSP transition: the next generation over the same memory */
    CHECK(cache.generation == old_generation + 1);
    wii_texture_cache_entry_set(&cache, 0, 600, 0, 1, 256, 1);
    CHECK(!wii_texture_cache_publish(&cache, &ticket));
    CHECK(cache.stats.stale == 1);
    CHECK(!wii_texture_cache_resident(&cache, 0));
    /* the same entry reserved again in this generation: the old ticket still never publishes */
    struct wii_texture_cache_ticket fresh;
    CHECK(wii_texture_cache_reserve(&cache, 0, 0, 0, &fresh) == _wii_texture_cache_ok);
    CHECK(!wii_texture_cache_publish(&cache, &ticket));
    CHECK(!wii_texture_cache_resident(&cache, 0));
    CHECK(wii_texture_cache_publish(&cache, &fresh));
    CHECK(wii_texture_cache_resident(&cache, 0));
    CHECK(wii_texture_cache_check(&cache));
}

static uint32_t seed = 12345u;

static uint32_t random_next(void)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7FFFu;
}

/* random frames: visibility, loads with and without publishing, failures,
draws and GPU completion at random; every invariant after every step, and no
reserved byte the GPU may still read */
static void test_random_frames_keep_every_invariant(void)
{
    int total_waits = 0;
    current = "random";
    for (int round = 0; round < 40; ++round) {
        int count = 8 + (int)(random_next() % 40);
        uint32_t region0 = (1024u + (uint32_t)(random_next() % 7168u)) & ~31u, region1 = (uint32_t)(random_next() % 4096u) & ~31u;
        cases++;
        CHECK(setup(count, region0, region1));
        for (int i = 0; i < count; ++i) {
            int found = random_next() % 16 != 0;
            uint32_t bytes = 32u + random_next() % 2400u;
            wii_texture_cache_entry_set(&cache, i, 700 + i, i & 3, found, bytes, (long)(random_next() % 100));
        }
        for (int frame = 0; frame < 300; ++frame) {
            wii_texture_cache_frame_begin(&cache);
            int visible = 1 + (int)(random_next() % 6);
            for (int v = 0; v < visible; ++v) {
                int index = (int)(random_next() % (uint32_t)count);
                wii_texture_cache_want(&cache, index, 1 + random_next() % 50);
            }
            for (int loads = 0; loads < 3; ++loads) {
                int e = wii_texture_cache_next_wanted(&cache);
                if (e < 0)
                    break;
                struct wii_texture_cache_ticket ticket;
                unsigned char visible_resident[MAX_ENTRIES];
                for (int i = 0; i < count; ++i)
                    visible_resident[i] = cache.entries[i].demand && wii_texture_cache_resident(&cache, i);
                enum wii_texture_cache_result result =
                    wii_texture_cache_reserve(&cache, e, 1, random_next() % 4, &ticket);
                CHECK(wii_texture_cache_check(&cache));
                for (int i = 0; i < count; ++i)
                    CHECK(!visible_resident[i] || wii_texture_cache_resident(&cache, i));
                if (result != _wii_texture_cache_ok)
                    continue;
                CHECK(ticket_safe(&ticket));
                CHECK(!wii_texture_cache_resident(&cache, e));
                memset(ticket.data, 0x5A, ticket.bytes);
                uint32_t outcome = random_next() % 10;
                if (outcome == 0)
                    wii_texture_cache_abandon(&cache, &ticket, 1);
                else if (outcome == 1)
                    wii_texture_cache_abandon(&cache, &ticket, 0);
                else
                    CHECK(wii_texture_cache_publish(&cache, &ticket));
                CHECK(wii_texture_cache_check(&cache));
            }
            /* the frame's draws: never a byte that is not resident */
            for (int i = 0; i < count; ++i)
                if (cache.entries[i].demand && cache.entries[i].state == _wii_texture_cache_loading)
                    CHECK(!wii_texture_cache_resident(&cache, i));
            draw();
            if (random_next() % 3 == 0)
                gpu_done();  /* the boot mode's frames complete out of this side's sight */
            CHECK(cache.stats.used <= region0 + region1);
        }
        CHECK(cache.stats.publishes > 0);
        CHECK((unsigned long)waits == cache.stats.waits);
        total_waits += waits;
    }
    /* the rounds did reuse bytes a draw had not completed (and waited) */
    CHECK(total_waits > 0);
}

int main(void)
{
    test_init_refuses_unusable_arguments();
    test_memory_limit_and_refusals();
    test_eviction_prefers_unwanted_and_oldest();
    test_visible_entries_are_never_evicted();
    test_duplicate_references_share_one_entry();
    test_gpu_fence_waits_before_reuse();
    test_failed_loads_are_given_back_and_bounded();
    test_stale_generation_never_publishes();
    test_random_frames_keep_every_invariant();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok %d cases\n", cases);
    return 0;
}
