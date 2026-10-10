/* HWI-007 memory strategy diagnostic (wii_memory_strategy).
 *
 * 1. Measures the Wii memory map after libogc initialisation: arenas, the
 *    executable image, heap, main stack peak, GX FIFO and framebuffers, and
 *    what fits of the current upstream reservations (tag cache, game state,
 *    sound and texture caches) in the actual arenas.
 * 2. With an owned-data manifest on SD, runs load/use/unload cycles: tags and
 *    one structure BSP are streamed into the upstream 22 MiB tag slot in MEM2;
 *    the whole tag graph is walked through the upstream validator schema
 *    (cache_schema_graph.c), every pointer field it names is relocated to its
 *    native MEM2 address and the graph is walked again through the native
 *    pointers. Both canonical reports must equal the host's (manifest).
 * 3. Game-state ownership: every upstream game-state allocation is made in
 *    the 20 MiB state slot with the actual engine data.c/memory_pool.c,
 *    representative state is restored from the previous cycle's (or launch's)
 *    pointer-free image, updated, and saved again (game_state_image.c).
 * Engine units follow ADR-018: -fsigned-char, and FPSCR NI is cleared before
 * any engine code runs. No game data is embedded. */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/ios.h>
#include <fat.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "build_id.h"
#include "../../../tools/wii/cache_arena_plan.h"
#include "../../../tools/wii/cache_stream_io.h"
#include "../../../tools/wii/cache_bsp_probe.h"
#include "../../../tools/wii/cache_schema_graph.h"
#include "../../../tools/wii/game_state_image.h"
#include "../../linux/include/halo_port_capacity.h"

#define DIR "sd:/halo-wii-memory-strategy"
#define MANIFEST_PATH DIR "/strategy.txt"
#define LOG_PATH DIR "/strategy.log"
#define RUNS_PATH DIR "/runs.txt"
#define STATE_PATH DIR "/state.bin"
#define FAULT_PATH DIR "/fault.bin"
#define RUNS_PREFIX "halo-wii-memory-strategy-v1 "
#define FIFO_BYTES (256u * 1024u)
#define RESERVE_BYTES ((size_t)0x200000)
#define SOUND_CACHE_BYTES ((size_t)0x400000)  /* upstream sound cache (ADR-016: 4 MiB) */
#define XBOX_TEXTURE_CACHE_BYTES ((size_t)0x580 * 0x4000)
#define CYCLES 4
#define MAIN_STACK_BYTES 0x20000u /* libogc default_stacks.o s_mainStack (link map) */
#define PAINT_BYTES (100u * 1024u)
#define PAINT ((uint32_t)0x5a17c0deu)

enum { TAG_SLOT, STATE_SLOT, SOUND_SLOT, IO0_SLOT, IO1_SLOT, SLOT_COUNT };

extern u8 __Arena1Lo[], __Arena1Hi[], __Arena2Lo[], __Arena2Hi[];
extern u8 __bss_start[], __bss_end[], __sbss_start[];
extern u8 __app_start[];

struct manifest {
    char tag_file[96], bsp_file[96];
    uint32_t tag_bytes, tag_crc32, bsp_bytes, bsp_crc32, map_bytes, bsp_ordinal;
    uint32_t graph_digest, edge_digest, pointer_fields, null_fields, reachable, residual;
};

static FILE *record;
static int log_failed;
static int failures;

static void out(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *format, ...)
{
    va_list args;
    if (!record)
        return;
    va_start(args, format);
    if (vfprintf(record, format, args) < 0)
        log_failed = 1;
    va_end(args);
    fflush(record);
}

static void check(int ok, const char *what)
{
    if (!ok) {
        ++failures;
        out("FAIL %s\n", what);
    }
}

/* ---------- FPSCR (ADR-018) */

static uint32_t fpscr(void)
{
    double value;
    uint64_t bits;
    __asm__ volatile("mffs %0" : "=f"(value));
    memcpy(&bits, &value, sizeof(bits));
    return (uint32_t)bits;
}

static void clear_ni(void)
{
    __asm__ volatile("mtfsb0 29"); /* FPSCR[NI] (IBM bit 29, value 0x4): IEEE subnormals */
}

/* ---------- stack high-water by painting the unused main stack below the entry frame */

static uintptr_t paint_top, paint_bottom;

__attribute__((noinline)) static void paint_stack(uintptr_t entry)
{
    paint_top = (entry - 512) & ~(uintptr_t)3;
    paint_bottom = paint_top - PAINT_BYTES;
    for (volatile uint32_t *word = (uint32_t *)paint_bottom; (uintptr_t)word < paint_top; ++word)
        *word = PAINT;
}

static uintptr_t stack_low_water(void)
{
    const uint32_t *word = (const uint32_t *)paint_bottom;
    while ((uintptr_t)word < paint_top && *word == PAINT)
        ++word;
    return (uintptr_t)word;
}

/* ---------- storage and manifest */

static int read_text(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    size_t used = fread(buffer, 1, capacity - 1, file);
    int ok = !ferror(file) && fgetc(file) == EOF;
    if (fclose(file) != 0)
        ok = 0;
    buffer[used] = 0;
    return ok && memchr(buffer, 0, used) == NULL;
}

static int read_runs(unsigned *previous)
{
    char text[64];
    *previous = 0;
    FILE *probe = fopen(RUNS_PATH, "rb");
    if (!probe)
        return errno == ENOENT;
    fclose(probe);
    if (!read_text(RUNS_PATH, text, sizeof(text)) || strncmp(text, RUNS_PREFIX, sizeof(RUNS_PREFIX) - 1))
        return 0;
    char *stop;
    const char *digits = text + sizeof(RUNS_PREFIX) - 1;
    errno = 0;
    unsigned long count = strtoul(digits, &stop, 10);
    if (errno || stop == digits || strcmp(stop, "\n") || count >= UINT32_MAX)
        return 0;
    *previous = (unsigned)count;
    return 1;
}

static int init_storage(unsigned *previous)
{
    if (!fatInitDefault())
        return 0;
    if (mkdir(DIR, 0777) != 0 && errno != EEXIST)
        return -1;
    if (!read_runs(previous))
        return -1;
    FILE *file = fopen(RUNS_PATH, "w");
    if (!file)
        return -1;
    int written = fprintf(file, RUNS_PREFIX "%u\n", *previous + 1);
    if (fclose(file) != 0 || written < 0)
        return -1;
    unsigned readback;
    if (!read_runs(&readback) || readback != *previous + 1)
        return -1;
    record = fopen(LOG_PATH, "a");
    return record ? 1 : -1;
}

/* Strict "key value" lines: every key exactly once, nothing else. */
static int parse_manifest(const char *text, struct manifest *m)
{
    static const char *const keys[] = {"tag_file", "bsp_file", "tag_bytes", "tag_crc32", "bsp_bytes", "bsp_crc32",
        "map_bytes", "bsp_ordinal", "graph_digest", "edge_digest", "pointer_fields", "null_fields", "reachable",
        "residual"};
    enum { KEYS = sizeof(keys) / sizeof(keys[0]) };
    uint32_t *numbers[KEYS] = {NULL, NULL, &m->tag_bytes, &m->tag_crc32, &m->bsp_bytes, &m->bsp_crc32,
        &m->map_bytes, &m->bsp_ordinal, &m->graph_digest, &m->edge_digest, &m->pointer_fields, &m->null_fields,
        &m->reachable, &m->residual};
    unsigned seen = 0;
    memset(m, 0, sizeof(*m));
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        char line[160], key[32], value[128], tail;
        if (length >= sizeof(line))
            return 0;
        memcpy(line, text, length);
        line[length] = 0;
        if (length && line[length - 1] == '\r')
            line[length - 1] = 0;
        text += length + (end != NULL);
        if (!line[0])
            continue;
        if (sscanf(line, "%31s %127s %c", key, value, &tail) != 2)
            return 0;
        unsigned k = 0;
        while (k < KEYS && strcmp(key, keys[k]))
            ++k;
        if (k == KEYS || seen & (1u << k))
            return 0;
        seen |= 1u << k;
        if (k < 2) {
            if (strncmp(value, "sd:/", 4) || strlen(value) >= sizeof(m->tag_file))
                return 0;
            strcpy(k ? m->bsp_file : m->tag_file, value);
        } else {
            char *stop;
            errno = 0;
            unsigned long parsed = strtoul(value, &stop, 0);
            if (errno || *stop || parsed > UINT32_MAX || value[0] == '-')
                return 0;
            *numbers[k] = (uint32_t)parsed;
        }
    }
    return seen == (1u << KEYS) - 1 && m->tag_bytes && m->tag_bytes <= CACHE_GRAPH_CACHE_BYTES && m->bsp_bytes;
}

/* ---------- plans: what fits of the upstream reservations */

static void fit(const char *name, const struct cache_arena_request *requests, size_t count, uintptr_t lo,
                uintptr_t hi, size_t reserve)
{
    struct cache_arena_plan plan;
    struct cache_arena_result result;
    size_t total = reserve;
    for (size_t index = 0; index < count; ++index)
        total += requests[index].size;
    int ok = cache_arena_plan_build(requests, count, (void *)lo, hi - lo, reserve, &plan, &result);
    out("FIT %s requested=%lu available=%lu fits=%d required=%lu %s=%ld failing_slot=%ld\n", name,
        (unsigned long)total, (unsigned long)(hi - lo), ok, (unsigned long)result.required,
        ok ? "remaining" : "shortfall",
        ok ? (long)(hi - lo - plan.required) : (long)(total - (hi - lo)),
        ok ? -1L : (long)result.slot_index);
}

/* ---------- one load/use/unload cycle */

struct cycle_result {
    int graph_ok, native_same, state_ok;
};

static unsigned char *slot_view(const struct cache_arena_owner *owner, size_t index, size_t bytes)
{
    struct cache_arena_handle handle;
    struct cache_arena_result result;
    unsigned char *view = NULL;
    if (!cache_arena_owner_handle(owner, index, &handle, &result) ||
        !cache_arena_owner_resolve(owner, &handle, 0, bytes, &view, &result))
        return NULL;
    return view;
}

static void describe_graph(const char *prefix, const struct cache_graph_report *report, int full)
{
    static char text[49152];
    cache_graph_describe(report, text, sizeof(text));
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        if (full || !strncmp(line, "GRAPH", 5))
            out("%s %s\n", prefix, line);
        if (!end)
            break;
        line = end + 1;
    }
}

static int graph_matches(const struct cache_graph_report *r, const struct manifest *m)
{
    return r->digest == m->graph_digest && r->edge_digest == m->edge_digest && r->pointer_fields == m->pointer_fields &&
           r->null_fields == m->null_fields && r->reachable == m->reachable && r->residual_address_words == m->residual &&
           r->corrections == 0;
}

static int load_tags(const struct manifest *m, struct cache_arena_owner *owner, struct cache_arena_handle *tag,
                     struct cache_bsp_reference *reference, const char **failure)
{
    struct cache_arena_handle io0, io1;
    struct cache_arena_result arena;
    struct cache_stream_result stream = {0};
    struct cache_bsp_result bsp;
    if (!cache_arena_owner_handle(owner, TAG_SLOT, tag, &arena) || !cache_arena_owner_handle(owner, IO0_SLOT, &io0, &arena) ||
        !cache_arena_owner_handle(owner, IO1_SLOT, &io1, &arena)) {
        *failure = "handles";
        return 0;
    }
    FILE *input = fopen(m->tag_file, "rb");
    if (!input) {
        *failure = "tag_open";
        return 0;
    }
    int read = cache_stream_read(input, owner, tag, &io0, &io1, m->tag_bytes, m->tag_crc32, &stream);
    if (fclose(input) != 0 || !read) {
        *failure = read ? "tag_close" : cache_stream_error_name(stream.error);
        return 0;
    }
    if (!cache_bsp_select(owner, tag, m->tag_bytes, m->map_bytes, m->bsp_ordinal, reference, &bsp) ||
        (uint32_t)reference->file_size != m->bsp_bytes) {
        *failure = "bsp_select";
        return 0;
    }
    input = fopen(m->bsp_file, "rb");
    if (!input) {
        *failure = "bsp_open";
        return 0;
    }
    stream = (struct cache_stream_result){0};
    read = cache_stream_read_at(input, owner, tag, &io0, &io1, reference->slot_offset, m->bsp_bytes, m->bsp_crc32,
                                &stream);
    if (fclose(input) != 0 || !read) {
        *failure = read ? "bsp_close" : cache_stream_error_name(stream.error);
        return 0;
    }
    return 1;
}

static int state_cycle(unsigned cycle, unsigned char *state, int restore)
{
    static struct gs_region region;
    struct gs_result result;
    struct gs_report before, after;
    char text[512];
    u64 started = gettime();
    if (!gs_build(&region, state, HALO_PORT_GAME_STATE_SIZE, HALO_PORT_GAME_STATE_CPU_SIZE,
                  HALO_PORT_GAME_STATE_GPU_SIZE, &result)) {
        out("FAIL state_build cycle=%u %s %lu\n", cycle, gs_error_name(result.error), (unsigned long)result.offset);
        return 0;
    }
    out("STATE_BUILD cycle=%u base=%p allocations=%u cpu_used=%lu gpu_used=%lu cpu_free=%lu build_us=%" PRIu64 "\n",
        cycle, (void *)state, region.count, (unsigned long)region.cpu_used, (unsigned long)region.gpu_used,
        (unsigned long)(region.cpu_size - region.cpu_used), ticks_to_microsecs(gettime() - started));
    if (restore) {
        FILE *in = fopen(STATE_PATH, "rb");
        u64 begun = gettime();
        int ok = in && gs_restore(&region, in, &result);
        if (in)
            fclose(in);
        if (!ok || !gs_save(&region, NULL, &before, &result)) {
            out("FAIL state_restore cycle=%u %s %lu\n", cycle, gs_error_name(result.error), (unsigned long)result.offset);
            return 0;
        }
        out("STATE_RESTORE cycle=%u step=%u image_crc=%08" PRIx32 " restore_us=%" PRIu64 "\n", cycle, before.step,
            before.image_crc, ticks_to_microsecs(gettime() - begun));
    }
    if (!gs_update(&region, &result)) {
        out("FAIL state_update cycle=%u %s %lu\n", cycle, gs_error_name(result.error), (unsigned long)result.offset);
        return 0;
    }
    FILE *file = fopen(STATE_PATH, "wb");
    u64 begun = gettime();
    int ok = file && gs_save(&region, file, &after, &result);
    if (file && fclose(file) != 0)
        ok = 0;
    if (!ok) {
        out("FAIL state_save cycle=%u\n", cycle);
        return 0;
    }
    gs_describe(&after, text, sizeof(text));
    out("STATE cycle=%u save_us=%" PRIu64 " %s", cycle, ticks_to_microsecs(gettime() - begun), text);
    return after.stale_rejected == after.stale_checked;
}

static int has_state_file(void)
{
    struct stat info;
    return stat(STATE_PATH, &info) == 0 && info.st_size > 0;
}

static int run_cycle(unsigned cycle, const struct manifest *m, int have_manifest, uintptr_t lo, uintptr_t hi,
                     void *workspace, size_t workspace_bytes, int restore_state)
{
    const struct cache_arena_request requests[SLOT_COUNT] = {
        {CACHE_GRAPH_CACHE_BYTES, 64}, {HALO_PORT_GAME_STATE_SIZE, 64}, {SOUND_CACHE_BYTES, 64},
        {CACHE_STREAM_IO_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64}};
    struct cache_arena_plan plan;
    struct cache_arena_owner owner;
    struct cache_arena_result arena;
    struct cache_arena_handle tag, stale;
    struct cache_bsp_reference reference;
    const char *failure = "none";
    int ok = 1;
    memset(&owner, 0, sizeof(owner));
    uintptr_t begin = lo + (cycle & 1 ? 4096u : 0u);
    if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)begin, hi - begin, RESERVE_BYTES, &plan, &arena) ||
        !cache_arena_owner_bind(&owner, &plan, (void *)begin, hi - begin, &arena)) {
        out("FAIL plan cycle=%u %s\n", cycle, cache_arena_error_name(arena.error));
        return 0;
    }
    SYS_SetArena2Lo((void *)(begin + plan.required));
    out("CYCLE_PLAN cycle=%u placement=%u span=%p required=%lu remaining=%lu tag=%p state=%p sound=%p\n", cycle,
        cycle & 1, (void *)begin, (unsigned long)plan.required, (unsigned long)(hi - begin - plan.required),
        (void *)(begin + plan.slots[TAG_SLOT].offset), (void *)(begin + plan.slots[STATE_SLOT].offset),
        (void *)(begin + plan.slots[SOUND_SLOT].offset));
    unsigned char *state = slot_view(&owner, STATE_SLOT, HALO_PORT_GAME_STATE_SIZE);
    ok = state && state_cycle(cycle, state, restore_state);
    check(ok, "state_cycle");

    if (have_manifest) {
        u64 started = gettime();
        if (!load_tags(m, &owner, &tag, &reference, &failure)) {
            out("FAIL load cycle=%u reason=%s\n", cycle, failure);
            ok = 0;
        } else {
            unsigned char *tags = slot_view(&owner, TAG_SLOT, CACHE_GRAPH_CACHE_BYTES);
            struct cache_graph_input input = {tags, m->tag_bytes, CACHE_GRAPH_CACHE_BYTES, CACHE_GRAPH_TAG_BASE,
                reference.slot_offset, m->bsp_bytes, (int32_t)reference.datum, 0};
            static struct cache_graph_report encoded, relocation, native;
            struct cache_graph_result result;
            u64 loaded = gettime();
            int walked = cache_graph_walk(&input, workspace, workspace_bytes, &encoded, &result);
            u64 walked_at = gettime();
            if (!walked)
                out("FAIL walk cycle=%u %s offset=%lu tag=%ld\n", cycle, cache_graph_error_name(result.error),
                    (unsigned long)result.offset, result.tag_ordinal);
            int relocated = walked && cache_graph_relocate(&input, tags, workspace, workspace_bytes, &relocation, &result);
            u64 relocated_at = gettime();
            input.native = 1;
            int native_ok = relocated && cache_graph_walk(&input, workspace, workspace_bytes, &native, &result);
            u64 native_at = gettime();
            int same = native_ok && !memcmp(&native, &encoded, sizeof(native));
            int matches = walked && graph_matches(&encoded, m);
            describe_graph("ENCODED", &encoded, cycle == 0);
            out("GRAPH_CYCLE cycle=%u load_us=%" PRIu64 " walk_us=%" PRIu64 " relocate_us=%" PRIu64
                " native_walk_us=%" PRIu64 " relocated=%u nulled=%u native_digest=%08" PRIx32 " native_same=%d host_match=%d\n",
                cycle, ticks_to_microsecs(loaded - started), ticks_to_microsecs(walked_at - loaded),
                ticks_to_microsecs(relocated_at - walked_at), ticks_to_microsecs(native_at - relocated_at),
                relocation.pointer_fields, relocation.null_fields, native.digest, same, matches);
            /* after relocation the header's table and the scenario's root are native MEM2 pointers,
             * dereferenced directly as the engine would */
            uint32_t instances = 0, scenario_root = 0;
            uint32_t scenario = (uint32_t)(tags[4] | tags[5] << 8);
            uintptr_t first = (uintptr_t)tags, last = first + CACHE_GRAPH_CACHE_BYTES;
            if (relocated) {
                memcpy(&instances, tags, 4);
                if (instances >= first && instances < last)
                    memcpy(&scenario_root, (const unsigned char *)(uintptr_t)instances + scenario * 32u + 20u, 4);
            }
            int native_pointers = instances >= first && instances < last && scenario_root >= first && scenario_root < last;
            out("NATIVE_POINTERS cycle=%u instances=%08" PRIx32 " scenario_root=%08" PRIx32 " in_tag_slot=%d\n", cycle,
                instances, scenario_root, native_pointers);
            check(native_pointers, "native_pointers");
            check(walked && relocated && same && matches, "graph_cycle");
            ok = ok && walked && relocated && same && matches;
            /* faults on this load: a structure_bsps block address past the tags, then a broken tag table */
            if (cycle == CYCLES - 1) {
                /* reload encoded bytes first: the relocation rewrote them */
                if (!load_tags(m, &owner, &tag, &reference, &failure)) {
                    check(0, "fault_reload");
                } else {
                    input.native = 0;
                    uint32_t table = tags[0] | tags[1] << 8 | tags[2] << 16 | (uint32_t)tags[3] << 24;
                    size_t scenario = (table - CACHE_GRAPH_TAG_BASE) + 20;
                    uint32_t root = tags[scenario] | tags[scenario + 1] << 8 | tags[scenario + 2] << 16 |
                                    (uint32_t)tags[scenario + 3] << 24;
                    size_t field = (root - CACHE_GRAPH_TAG_BASE) + 0x5a4 + 4;
                    unsigned char saved[4];
                    memcpy(saved, tags + field, 4);
                    tags[field + 3] ^= 0x40; /* address 0x40000000 further */
                    int rejected = !cache_graph_walk(&input, workspace, workspace_bytes, &encoded, &result);
                    out("FAULT kind=block_address_outside rejected=%d reason=%s offset=%lu expected_offset=%lu\n",
                        rejected, cache_graph_error_name(result.error), (unsigned long)result.offset,
                        (unsigned long)(field - 4));
                    check(rejected && result.error == CACHE_GRAPH_EXTENT && result.offset == field - 4, "fault_block");
                    memcpy(tags + field, saved, 4);
                    size_t ordinal_field = (table - CACHE_GRAPH_TAG_BASE) + 32 + 12; /* tag 1's datum */
                    tags[ordinal_field] ^= 0x02;
                    rejected = !cache_graph_walk(&input, workspace, workspace_bytes, &encoded, &result);
                    out("FAULT kind=tag_table_ordinal rejected=%d reason=%s offset=%lu expected_offset=%lu\n", rejected,
                        cache_graph_error_name(result.error), (unsigned long)result.offset,
                        (unsigned long)ordinal_field);
                    check(rejected && result.error == CACHE_GRAPH_TABLE && result.offset == ordinal_field,
                          "fault_table");
                    tags[ordinal_field] ^= 0x02;
                    int restored = cache_graph_walk(&input, workspace, workspace_bytes, &encoded, &result) &&
                                   graph_matches(&encoded, m);
                    out("FAULT kind=restored_bytes walk_ok=%d\n", restored);
                    check(restored, "fault_restored");
                }
            }
        }
    }
    /* unload: a handle from this cycle must not resolve after release */
    if (!cache_arena_owner_handle(&owner, STATE_SLOT, &stale, &arena))
        check(0, "stale_handle_take");
    struct cache_arena_result released;
    unsigned char *view = NULL;
    int release_ok = cache_arena_owner_release(&owner, &released);
    int stale_rejected = !cache_arena_owner_resolve(&owner, &stale, 0, 1, &view, &arena) && arena.error == CACHE_ARENA_STATE;
    int arena_ok = (uintptr_t)SYS_GetArena2Lo() == begin + plan.required;
    SYS_SetArena2Lo((void *)lo);
    out("UNLOAD cycle=%u released=%d stale_handle_rejected=%d arena_restored=%d heap_in_use=%d\n", cycle, release_ok,
        stale_rejected, arena_ok && (uintptr_t)SYS_GetArena2Lo() == lo, mallinfo().uordblks);
    check(release_ok && stale_rejected && arena_ok, "unload");
    return ok && release_ok && stale_rejected && arena_ok;
}

/* ---------- damaged state images */

static int copy_with_fault(long truncate, long flip_at)
{
    FILE *in = fopen(STATE_PATH, "rb"), *to = fopen(FAULT_PATH, "wb");
    long offset = 0;
    int c, ok = in && to;
    while (ok && (c = fgetc(in)) != EOF) {
        if (truncate >= 0 && offset >= truncate)
            break;
        if (offset == flip_at)
            c ^= 0x01;
        if (fputc(c, to) == EOF)
            ok = 0;
        ++offset;
    }
    if (in)
        fclose(in);
    if (to && fclose(to) != 0)
        ok = 0;
    return ok;
}

static void state_faults(uintptr_t lo, uintptr_t hi)
{
    static const struct { const char *name; long truncate, flip; } faults[] = {
        {"truncated", 100, -1}, {"crc_flip", -1, 4000}, {"magic", -1, 0}, {"empty", 0, -1}};
    if (hi - lo < HALO_PORT_GAME_STATE_SIZE + 64)
        return;
    unsigned char *base = (unsigned char *)((lo + 63) & ~(uintptr_t)63);
    SYS_SetArena2Lo(base + HALO_PORT_GAME_STATE_SIZE);
    for (unsigned index = 0; index < sizeof(faults) / sizeof(faults[0]); ++index) {
        static struct gs_region region;
        struct gs_result result;
        int copied = copy_with_fault(faults[index].truncate, faults[index].flip);
        int built = gs_build(&region, base, HALO_PORT_GAME_STATE_SIZE, HALO_PORT_GAME_STATE_CPU_SIZE,
                             HALO_PORT_GAME_STATE_GPU_SIZE, &result);
        FILE *in = fopen(FAULT_PATH, "rb");
        int taken = copied && built && in && gs_restore(&region, in, &result);
        if (in)
            fclose(in);
        out("STATE_FAULT kind=%s rejected=%d reason=%s live=%d\n", faults[index].name, !taken,
            gs_error_name(result.error), region.live);
        check(copied && built && !taken && !region.live, "state_fault");
    }
    SYS_SetArena2Lo((void *)lo);
}

/* ---------- heap capacity probe (last: it moves the arenas' low bounds) */

static void heap_probe(void)
{
    /* greedy 1 MiB then 64 KiB requests until malloc fails: what the libogc heap reaches in each bank */
    struct mallinfo before = mallinfo();
    static void *chunks[2048];
    static size_t sizes[2048];
    size_t chunk_count = 0, total = 0, mem1_bytes = 0, mem2_bytes = 0;
    for (size_t size = 1u << 20; size >= 65536; size >>= 4)
        while (chunk_count < 2048) {
            void *chunk = malloc(size);
            if (!chunk)
                break;
            sizes[chunk_count] = size;
            chunks[chunk_count++] = chunk;
            total += size;
        }
    for (size_t index = 0; index < chunk_count; ++index) {
        if ((uintptr_t)chunks[index] >= 0x90000000u)
            mem2_bytes += sizes[index];
        else
            mem1_bytes += sizes[index];
    }
    struct mallinfo full = mallinfo();
    for (size_t index = 0; index < chunk_count; ++index)
        free(chunks[index]);
    struct mallinfo after = mallinfo();
    out("HEAP_PROBE greedy_total=%lu chunks=%lu in_mem1=%lu in_mem2=%lu arena_before=%d arena_full=%d "
        "arena_after=%d in_use_after=%d free_after=%d arena1=%p..%p arena2=%p..%p\n",
        (unsigned long)total, (unsigned long)chunk_count, (unsigned long)mem1_bytes, (unsigned long)mem2_bytes,
        before.arena, full.arena, after.arena, after.uordblks, after.fordblks, SYS_GetArena1Lo(), SYS_GetArena1Hi(),
        SYS_GetArena2Lo(), SYS_GetArena2Hi());
    /* then single requests: where malloc places them (logged, then freed) */
    static const size_t singles[] = {16u << 20, 32u << 20, 48u << 20, 60u << 20};
    for (unsigned index = 0; index < sizeof(singles) / sizeof(singles[0]); ++index) {
        void *block = malloc(singles[index]);
        uintptr_t at = (uintptr_t)block;
        out("HEAP_SINGLE bytes=%lu at=%p end=%p arena1_lo=%p arena2_lo=%p arena2_hi=%p within_mem2_arena=%d\n",
            (unsigned long)singles[index], block, (void *)(at ? at + singles[index] : 0), SYS_GetArena1Lo(),
            SYS_GetArena2Lo(), SYS_GetArena2Hi(),
            at >= 0x90000000u && at + singles[index] <= (uintptr_t)SYS_GetArena2Hi());
        free(block);
    }
}

int main(void)
{
    uint32_t fpscr_entry = fpscr();
    clear_ni();
    uint32_t fpscr_engine = fpscr();
    uintptr_t entry_sp = (uintptr_t)__builtin_frame_address(0);
    paint_stack(entry_sp);
    uintptr_t a1lo = (uintptr_t)SYS_GetArena1Lo(), a1hi = (uintptr_t)SYS_GetArena1Hi();
    uintptr_t a2lo = (uintptr_t)SYS_GetArena2Lo(), a2hi = (uintptr_t)SYS_GetArena2Hi();
    struct mallinfo heap_entry = mallinfo();
    u64 boot = gettime();

    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (!mode)
        return 1;
    unsigned previous_runs = 0;
    int storage = init_storage(&previous_runs);
    out("BEGIN target=memory_strategy build=%s previous_runs=%u storage=%d ios=%d.%d\n", WII_BUILD_ID, previous_runs,
        storage, IOS_GetVersion(), IOS_GetRevision());
    out("FPSCR entry=%08" PRIx32 " engine=%08" PRIx32 " ni_entry=%d ni_engine=%d\n", fpscr_entry, fpscr_engine,
        (fpscr_entry & 4u) != 0, (fpscr_engine & 4u) != 0);
    check((fpscr_engine & 4u) == 0, "fpscr_ni");
    out("MAP link app_start=%p bss=%p..%p sbss=%p arena1_link=%p..%p arena2_link=%p..%p\n", (void *)__app_start,
        (void *)__bss_start, (void *)__bss_end, (void *)__sbss_start, (void *)__Arena1Lo, (void *)__Arena1Hi,
        (void *)__Arena2Lo, (void *)__Arena2Hi);
    out("MAP entry arena1=%p..%p bytes=%lu arena2=%p..%p bytes=%lu libogc_arena1_taken=%lu ios_reserved_above_mem2=%lu "
        "heap_arena=%d heap_in_use=%d main_sp=%p main_stack=%u\n",
        (void *)a1lo, (void *)a1hi, (unsigned long)(a1hi - a1lo), (void *)a2lo, (void *)a2hi,
        (unsigned long)(a2hi - a2lo), (unsigned long)(a1lo - (uintptr_t)__Arena1Lo),
        (unsigned long)(0x94000000u - a2hi), heap_entry.arena, heap_entry.uordblks, (void *)entry_sp, MAIN_STACK_BYTES);

    /* GX as the diagnostics configure it: 256 KiB FIFO, two XFBs */
    void *fifo = memalign(32, FIFO_BYTES);
    u32 xfb_bytes = VIDEO_GetFrameBufferSize(mode);
    void *xfb0 = SYS_AllocateFramebuffer(mode), *xfb1 = SYS_AllocateFramebuffer(mode);
    if (!fifo || !xfb0 || !xfb1) {
        out("ALLOC_FAIL fifo=%p xfb0=%p xfb1=%p\n", fifo, xfb0, xfb1);
        return 1;
    }
    memset(fifo, 0, FIFO_BYTES);
    DCFlushRange(fifo, FIFO_BYTES);
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(MEM_K0_TO_K1(xfb0));
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    GX_Init(fifo, FIFO_BYTES);
    struct mallinfo heap_gx = mallinfo();
    out("MAP gx fifo=%p bytes=%u xfb0=%p xfb1=%p xfb_bytes_each=%u video=%ux%u heap_arena=%d heap_in_use=%d arena1=%p\n",
        fifo, FIFO_BYTES, xfb0, xfb1, xfb_bytes, mode->fbWidth, mode->xfbHeight, heap_gx.arena, heap_gx.uordblks,
        SYS_GetArena1Lo());

    uintptr_t lo = (uintptr_t)SYS_GetArena2Lo(), hi = (uintptr_t)SYS_GetArena2Hi();
    /* the upstream reservations against the actual MEM2 arena */
    {
        const struct cache_arena_request all[] = {{CACHE_GRAPH_CACHE_BYTES, 64}, {HALO_PORT_GAME_STATE_SIZE, 64},
            {SOUND_CACHE_BYTES, 64}, {XBOX_TEXTURE_CACHE_BYTES, 64}};
        const struct cache_arena_request desktop[] = {{CACHE_GRAPH_CACHE_BYTES, 64}, {HALO_PORT_GAME_STATE_SIZE, 64},
            {SOUND_CACHE_BYTES, 64}, {HALO_PORT_TEXTURE_CACHE_SIZE, 64}};
        const struct cache_arena_request no_textures[] = {{CACHE_GRAPH_CACHE_BYTES, 64},
            {HALO_PORT_GAME_STATE_SIZE, 64}, {SOUND_CACHE_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64},
            {CACHE_STREAM_IO_BYTES, 64}};
        const struct cache_arena_request compact_state[] = {{CACHE_GRAPH_CACHE_BYTES, 64},
            {(gs_census_cpu_requested + gs_census_gpu_requested + 31) & ~31ul, 64}, {SOUND_CACHE_BYTES, 64},
            {CACHE_STREAM_IO_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64}};
        fit("mem2_tags_state_sound_xbox_textures", all, 4, lo, hi, 0);
        fit("mem2_tags_state_sound_desktop_textures", desktop, 4, lo, hi, 0);
        fit("mem2_tags_state_sound_io_reserve", no_textures, 5, lo, hi, RESERVE_BYTES);
        fit("mem2_tags_census_state_sound_io_reserve", compact_state, 5, lo, hi, RESERVE_BYTES);
        const struct cache_arena_request mem1_textures[] = {{XBOX_TEXTURE_CACHE_BYTES, 32}};
        fit("mem1_xbox_textures_after_gx", mem1_textures, 1, (uintptr_t)SYS_GetArena1Lo(),
            (uintptr_t)SYS_GetArena1Hi(), 0);
        out("CENSUS cpu_requested=%lu gpu_requested=%lu cpu_reservation=%lu gpu_reservation=%lu rows=%u\n",
            gs_census_cpu_requested, gs_census_gpu_requested, (unsigned long)HALO_PORT_GAME_STATE_CPU_SIZE,
            (unsigned long)HALO_PORT_GAME_STATE_GPU_SIZE, gs_census_row_count);
    }
    {
        uint16_t seed, native;
        unsigned differ = 0, arrays = 0;
        gs_identifier_seeds("object", &seed, &native);
        out("SEEDS object=%04x native_copy=%04x", seed, native);
        gs_identifier_seeds("players", &seed, &native);
        out(" players=%04x native_copy=%04x", seed, native);
        for (unsigned row = 0; row < gs_census_row_count; ++row)
            if (gs_census_rows[row].kind == GS_DATA) {
                gs_identifier_seeds(gs_census_rows[row].name, &seed, &native);
                ++arrays;
                differ += seed != native;
            }
        out(" arrays=%u native_copy_differs=%u\n", arrays, differ);
    }

    struct manifest manifest;
    static char text[2048];
    int have_manifest = 0;
    if (storage == 1) {
        FILE *probe = fopen(MANIFEST_PATH, "rb");
        if (probe) {
            fclose(probe);
            have_manifest = read_text(MANIFEST_PATH, text, sizeof(text)) && parse_manifest(text, &manifest);
            check(have_manifest, "manifest");
        }
    }
    out("MANIFEST present=%d tag_bytes=%" PRIu32 " bsp_bytes=%" PRIu32 " expected_digest=%08" PRIx32 "\n",
        have_manifest, have_manifest ? manifest.tag_bytes : 0, have_manifest ? manifest.bsp_bytes : 0,
        have_manifest ? manifest.graph_digest : 0);

    size_t workspace_bytes = cache_graph_workspace_bytes(CACHE_GRAPH_CACHE_BYTES);
    void *workspace = have_manifest ? memalign(32, workspace_bytes) : NULL;
    if (have_manifest && !workspace)
        check(0, "workspace");
    struct mallinfo heap_ready = mallinfo();
    out("MAP ready heap_arena=%d heap_in_use=%d graph_workspace=%lu at=%p arena1=%p\n", heap_ready.arena,
        heap_ready.uordblks, (unsigned long)(workspace ? workspace_bytes : 0), workspace, SYS_GetArena1Lo());

    int restore = has_state_file();
    out("STATE_PRIOR present=%d\n", restore);
    unsigned passed = 0;
    int heap_cycle0 = 0, heap_flat = 1;
    for (unsigned cycle = 0; storage == 1 && cycle < CYCLES; ++cycle) {
        if (run_cycle(cycle, &manifest, have_manifest && workspace, lo, hi, workspace, workspace_bytes, restore))
            ++passed;
        restore = 1;
        int in_use = mallinfo().uordblks;
        if (cycle == 0)
            heap_cycle0 = in_use;
        else if (in_use != heap_cycle0)
            heap_flat = 0;
        VIDEO_WaitVSync();
    }
    check(heap_flat, "heap_flat");
    if (storage == 1)
        state_faults(lo, hi);
    free(workspace);
    uintptr_t low_water = stack_low_water();
    out("STACK entry_sp=%p painted=%u peak_below_entry=%lu window_exhausted=%d\n", (void *)entry_sp, PAINT_BYTES,
        (unsigned long)(paint_top - low_water + 512), low_water <= paint_bottom);
    heap_probe();
    out("END target=memory_strategy build=%s cycles=%u passed=%u manifest=%d failures=%d elapsed_ms=%" PRIu64
        " result=%s\n", WII_BUILD_ID, CYCLES, passed, have_manifest, failures,
        ticks_to_millisecs(gettime() - boot), !failures && passed == CYCLES && !log_failed ? "pass" : "fail");
    if (record)
        fclose(record);
    return 0;
}
