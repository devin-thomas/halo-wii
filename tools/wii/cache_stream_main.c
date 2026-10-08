/* Provisional MEM2 reservations and raw-tag streaming; no engine pool load. */
#include "cache_arena_plan.h"
#include "cache_arena_fixture.h"
#include "cache_address_probe.h"
#include "cache_address_fixture.h"
#include "cache_address_owned.h"
#include "cache_stream_io.h"
#include "cache_stream_fixture.h"
#include "cache_probe_payload.h"
#include "../../port/linux/include/halo_port_capacity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GEKKO
#include <gccore.h>
#include <fat.h>
#include <errno.h>
#include <sys/stat.h>
#include <malloc.h>
#endif

enum { TAG_SLOT, STATE_SLOT, SOUND_SLOT, INDEX_SLOT, IO0_SLOT, IO1_SLOT, SLOT_COUNT };
typedef char verify_stream_index_words[sizeof(struct cache_address_instance) == 32 ? 1 : -1];
#define RESERVE_BYTES ((size_t)0x200000)
#define HOST_SPAN_BYTES ((size_t)54368224)

static unsigned char *slot_view(const struct cache_arena_owner *owner, size_t index, size_t size)
{
    struct cache_arena_handle handle;
    struct cache_arena_result status;
    unsigned char *view = NULL;
    if (!cache_arena_owner_handle(owner, index, &handle, &status) ||
        !cache_arena_owner_resolve(owner, &handle, 0, size, &view, &status)) return NULL;
    return view;
}

static int decode_index(const struct cache_arena_owner *owner)
{
    unsigned char *tags = slot_view(owner, TAG_SLOT, CACHE_PRIVATE_SIZE);
    unsigned char *index = slot_view(owner, INDEX_SLOT, CACHE_PRIVATE_COUNT * 32u);
    if (!tags || !index) return 1;
    struct cache_address_region region = {tags, CACHE_PRIVATE_SIZE, UINT32_C(0x803a6000)};
    struct cache_address_header header;
    struct cache_address_result status;
    struct cache_address_span table;
    if (!cache_address_decode_header(&region, &header, &status) || header.tag_count != CACHE_PRIVATE_COUNT ||
        !cache_address_resolve(&region, header.instances_address, header.tag_count, 32, &table, &status) ||
        cache_probe_crc32(tags + table.offset, table.length) != CACHE_PRIVATE_TABLE_CRC) return 1;
    unsigned scenario_found = 0;
    for (size_t i = 0; i < CACHE_PRIVATE_COUNT; ++i) {
        struct cache_address_instance value;
        struct cache_address_span name, root;
        if (!cache_address_decode_instance(tags, region.size, table.offset + i * 32, &value, &status) ||
            (value.datum & 65535u) != i ||
            !cache_address_resolve(&region, value.name_address, 1, 1, &name, &status) ||
            !memchr(tags + name.offset, 0, region.size - name.offset)) return 1;
        if (value.root_address) {
            if (!cache_address_resolve(&region, value.root_address, 1, 1, &root, &status)) return 1;
        } else if (value.group != UINT32_C(0x73627370)) return 1;
        if (i == (header.scenario_datum & 65535u)) {
            if (value.datum != header.scenario_datum || value.group != UINT32_C(0x73636e72)) return 1;
            scenario_found = 1;
        }
        memcpy(index + i * 32, &value, sizeof(value));
        uint32_t words[8] = {value.group, value.parent[0], value.parent[1], value.datum,
                             value.name_address, value.root_address, value.unused[0], value.unused[1]};
        unsigned char serialized[32];
        for (unsigned word = 0; word < 8; ++word)
            for (unsigned byte = 0; byte < 4; ++byte)
                serialized[word * 4 + byte] = (unsigned char)(words[word] >> (byte * 8));
        if (memcmp(serialized, tags + table.offset + i * 32, 32)) return 1;
    }
    return !scenario_found;
}

static int controlled_fragmentation(FILE *report, uintptr_t begin, uintptr_t end,
                                    const struct cache_arena_request *requests)
{
    const size_t guard = 65536;
    size_t capacity = end - begin;
    size_t first = (capacity / 3) & ~(size_t)31;
    size_t second = (capacity * 2 / 3) & ~(size_t)31;
    if (first + guard > second || second + guard > capacity) return 1;
    uintptr_t starts[3] = {begin, begin + first + guard, begin + second + guard};
    size_t lengths[3] = {first, second - first - guard, capacity - second - guard};
    struct cache_arena_plan full;
    struct cache_arena_result status;
    if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)begin, capacity, RESERVE_BYTES, &full, &status)) return 1;
#ifdef GEKKO
    SYS_SetArena2Lo((void *)end);
#endif
    memset((void *)(begin + first), 0x6d, guard);
    memset((void *)(begin + second), 0x9b, guard);
    unsigned rejected = 0;
    size_t largest = 0;
    for (unsigned i = 0; i < 3; ++i) {
        struct cache_arena_plan candidate;
        if (lengths[i] > largest) largest = lengths[i];
        if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)starts[i], lengths[i],
                                    RESERVE_BYTES, &candidate, &status) && status.error == CACHE_ARENA_CAPACITY) ++rejected;
    }
    unsigned failures = rejected != 3 || capacity - 2 * guard < full.required;
    for (size_t i = 0; i < guard; ++i)
        if (((unsigned char *)begin)[first + i] != 0x6d || ((unsigned char *)begin)[second + i] != 0x9b) { ++failures; break; }
    fprintf(report, "STREAM FRAGMENT controlled_intervals=3 guard_bytes=%lu aggregate_free=%lu largest_free=%lu requested=%lu rejected=%u failures=%u scope=owned_intervals_not_system_heap\n",
            (unsigned long)(2 * guard), (unsigned long)(capacity - 2 * guard), (unsigned long)largest,
            (unsigned long)full.required, rejected, failures);
#ifdef GEKKO
    if ((uintptr_t)SYS_GetArena2Lo() != end || (uintptr_t)SYS_GetArena2Hi() != end) {
        fprintf(report, "STREAM FAIL fragmentation arena mutation; refusing restore\n"); return 1;
    }
    SYS_SetArena2Lo((void *)begin);
#endif
    return failures != 0;
}

static int stream_cycles(FILE *report, const char *path)
{
    const struct cache_arena_request requests[SLOT_COUNT] = {
        {0x1600000, 64}, {HALO_PORT_GAME_STATE_SIZE, 32}, {0x400000, 32},
        {65535u * 32u, 32}, {CACHE_STREAM_IO_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64}
    };
    struct cache_arena_owner owner = {0};
    struct cache_arena_handle previous = {0};
    unsigned failures = 0, expected_errors = 0, completed = 0, stale_rebind = 0, stale_release = 0;
    size_t maximum_charge = 0, maximum_io_chunks = 0;
    fprintf(report, "STREAM REQUEST tags=%lu state=%lu sound=%lu max_index=%lu io_each=%lu reserve=%lu state_sound=canary_placeholders_not_engine_pools\n",
            (unsigned long)requests[TAG_SLOT].size, (unsigned long)requests[STATE_SLOT].size,
            (unsigned long)requests[SOUND_SLOT].size, (unsigned long)requests[INDEX_SLOT].size,
            (unsigned long)CACHE_STREAM_IO_BYTES, (unsigned long)RESERVE_BYTES);
#ifdef GEKKO
    uintptr_t initial_lo = (uintptr_t)SYS_GetArena2Lo();
    uintptr_t initial_hi = (uintptr_t)SYS_GetArena2Hi();
    fprintf(report, "STREAM ARENAS mem1_lo=%p mem1_hi=%p mem1_bytes=%lu mem2_lo=%p mem2_hi=%p mem2_bytes=%lu\n",
            SYS_GetArena1Lo(), SYS_GetArena1Hi(), (unsigned long)SYS_GetArena1Size(),
            (void *)initial_lo, (void *)initial_hi, (unsigned long)SYS_GetArena2Size());
    struct mallinfo before = mallinfo();
    fprintf(report, "STREAM HEAP before in_use=%d arena=%d free=%d\n", before.uordblks, before.arena, before.fordblks);
#else
    unsigned char *storage = malloc(HOST_SPAN_BYTES + 4096);
    if (!storage) { fprintf(report, "STREAM FAIL host capacity allocation\n"); return 1; }
    uintptr_t initial_lo = (uintptr_t)storage;
    uintptr_t initial_hi = initial_lo + HOST_SPAN_BYTES;
#endif
    failures += (unsigned)controlled_fragmentation(report, initial_lo, initial_hi, requests);
    for (unsigned cycle = 0; cycle < 16 && !failures; ++cycle) {
        uintptr_t begin = initial_lo + (cycle & 1u ? 4096u : 0u);
        struct cache_arena_plan plan;
        struct cache_arena_result arena_status;
        if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)begin, initial_hi - begin,
                                    RESERVE_BYTES, &plan, &arena_status)) {
            fprintf(report, "STREAM FAIL plan cycle=%u error=%s required=%lu\n", cycle,
                    cache_arena_error_name(arena_status.error), (unsigned long)arena_status.required);
            ++failures; break;
        }
#ifdef GEKKO
        SYS_SetArena2Lo((void *)(begin + plan.required));
#endif
        if (!cache_arena_owner_bind(&owner, &plan, (void *)begin, initial_hi - begin, &arena_status)) {
            fprintf(report, "STREAM FAIL owner bind\n"); ++failures;
#ifdef GEKKO
            SYS_SetArena2Lo((void *)initial_lo);
#endif
            break;
        }
        unsigned char *bad_view = NULL;
        if (cycle) {
            if (cache_arena_owner_resolve(&owner, &previous, 0, 1, &bad_view, &arena_status) ||
                arena_status.error != CACHE_ARENA_HANDLE) ++failures;
            else ++stale_rebind;
        }
        struct cache_arena_handle tag = {0}, io0 = {0}, io1 = {0};
        if (!cache_arena_owner_handle(&owner, TAG_SLOT, &tag, &arena_status) ||
            !cache_arena_owner_handle(&owner, IO0_SLOT, &io0, &arena_status) ||
            !cache_arena_owner_handle(&owner, IO1_SLOT, &io1, &arena_status)) {
            fprintf(report, "STREAM FAIL live handles\n"); ++failures;
        }
        /* Entire requested backing is owned, but engine pools remain canary placeholders. */
        memset((void *)begin, 0xa5, plan.required);
        FILE *input = fopen(path, "rb");
        struct cache_stream_result stream_status = {0};
        if (!input) { fprintf(report, "STREAM FAIL open owned raw input\n"); ++failures; }
        else {
            if (!cache_stream_read(input, &owner, &tag, &io0, &io1, CACHE_PRIVATE_SIZE,
                                    CACHE_PRIVATE_CRC, &stream_status)) {
                fprintf(report, "STREAM FAIL read cycle=%u error=%s bytes=%lu\n", cycle,
                        cache_stream_error_name(stream_status.error), (unsigned long)stream_status.bytes);
                ++failures;
            } else {
                if (decode_index(&owner)) {
                    fprintf(report, "STREAM FAIL index identity/span or numeric table validation cycle=%u\n", cycle);
                    ++failures;
                }
                if (stream_status.chunks > maximum_io_chunks) maximum_io_chunks = stream_status.chunks;
                if (cycle == 0) fprintf(report, "STREAM IO bytes=%lu chunks=%lu fread_calls=%lu crc32=%08lx\n",
                                        (unsigned long)stream_status.bytes, (unsigned long)stream_status.chunks,
                                        (unsigned long)stream_status.read_calls, (unsigned long)stream_status.actual_crc32);
            }
            if (cycle == 0) {
                const size_t lengths[] = {CACHE_PRIVATE_SIZE - 1u, CACHE_PRIVATE_SIZE + 1u, CACHE_PRIVATE_SIZE};
                const enum cache_stream_error errors[] = {CACHE_STREAM_TRAILING, CACHE_STREAM_SHORT, CACHE_STREAM_CRC};
                for (unsigned test = 0; test < 3; ++test) {
                    rewind(input); stream_status = (struct cache_stream_result){0};
                    int ok = cache_stream_read(input, &owner, &tag, &io0, &io1, lengths[test],
                                                test == 2 ? CACHE_PRIVATE_CRC ^ 1u : CACHE_PRIVATE_CRC, &stream_status);
                    if (ok || stream_status.error != errors[test]) ++failures;
                    else ++expected_errors;
                }
            }
            if (fclose(input)) { fprintf(report, "STREAM FAIL input close\n"); ++failures; }
        }
        for (size_t index = STATE_SLOT; index <= SOUND_SLOT; ++index) {
            unsigned char *view = slot_view(&owner, index, requests[index].size);
            if (!view || view[0] != 0xa5 || view[requests[index].size - 1] != 0xa5) ++failures;
        }
        for (size_t i = plan.reserve_offset; i < plan.required; ++i)
            if (((unsigned char *)begin)[i] != 0xa5) { ++failures; break; }
        size_t charge = begin - initial_lo + plan.required;
        if (charge > maximum_charge) maximum_charge = charge;
        if (cycle < 2) fprintf(report, "STREAM PLAN placement=%u base=%p data_end=%lu reserve=%lu required=%lu arena_charge=%lu remaining=%lu\n",
                              cycle, (void *)begin, (unsigned long)plan.data_end, (unsigned long)plan.reserve_size,
                              (unsigned long)plan.required, (unsigned long)charge, (unsigned long)(initial_hi - initial_lo - charge));
        previous = tag;
        if (!cache_arena_owner_release(&owner, &arena_status)) {
            fprintf(report, "STREAM FAIL release; backing retained\n");
            return 1;
        }
        if (cache_arena_owner_resolve(&owner, &previous, 0, 1, &bad_view, &arena_status) ||
            arena_status.error != CACHE_ARENA_STATE) ++failures;
        else ++stale_release;
#ifdef GEKKO
        if ((uintptr_t)SYS_GetArena2Lo() != begin + plan.required || (uintptr_t)SYS_GetArena2Hi() != initial_hi) {
            fprintf(report, "STREAM FAIL arena changed; refusing restore\n"); return 1;
        }
        SYS_SetArena2Lo((void *)initial_lo);
#endif
        ++completed;
    }
#ifdef GEKKO
    struct mallinfo after = mallinfo();
    fprintf(report, "STREAM HEAP released in_use=%d arena=%d free=%d\n", after.uordblks, after.arena, after.fordblks);
    fprintf(report, "STREAM ARENAS released mem2_lo=%p mem2_hi=%p mem2_bytes=%lu\n",
            SYS_GetArena2Lo(), SYS_GetArena2Hi(), (unsigned long)SYS_GetArena2Size());
#else
    free(storage);
#endif
    fprintf(report, "STREAM SUMMARY cycles=%u tags=%lu max_charge=%lu chunks=%lu expected_errors=%u stale_rebind=%u stale_release=%u generation=%llu failures=%u\n",
            completed, (unsigned long)CACHE_PRIVATE_COUNT, (unsigned long)maximum_charge, (unsigned long)maximum_io_chunks,
            expected_errors, stale_rebind, stale_release, (unsigned long long)owner.generation, failures);
    return failures != 0;
}

int main(int argc, char **argv)
{
    FILE *report = stdout;
    const char *path;
#ifdef GEKKO
    (void)argc; (void)argv;
    VIDEO_Init(); GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (!mode) { fprintf(stderr, "STREAM video mode unavailable\n"); return 2; }
    void *framebuffer = SYS_AllocateFramebuffer(mode);
    if (!framebuffer) { fprintf(stderr, "STREAM framebuffer unavailable\n"); return 2; }
    framebuffer = MEM_K0_TO_K1(framebuffer);
    console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode); VIDEO_SetNextFramebuffer(framebuffer); VIDEO_SetBlack(false); VIDEO_Flush();
    VIDEO_WaitVSync(); if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    if (!fatInitDefault()) { fprintf(stderr, "STREAM SD unavailable\n"); return 2; }
    if (mkdir("sd:/halo-wii-memory", 0777) && errno != EEXIST) { fprintf(stderr, "STREAM log directory failed\n"); return 2; }
    report = fopen("sd:/halo-wii-memory/cache-stream.log", "a");
    if (!report) { fprintf(stderr, "STREAM log open failed\n"); return 2; }
    path = CACHE_STREAM_FILENAME;
#else
    if (argc != 2) { fprintf(stderr, "STREAM requires private raw tag path\n"); return 2; }
    path = argv[1];
#endif
    if (fprintf(report, "BEGIN STREAM build=%s\n", CACHE_PROBE_BUILD_ID) < 0 || fflush(report)) return 2;
    int result = wii_cache_address_fixture(report, 1);
    result |= wii_cache_arena_fixture(report, 1);
    result |= wii_cache_stream_fixture(report, 1);
    result |= stream_cycles(report, path);
    if (fprintf(report, "END STREAM build=%s result=%d\n", CACHE_PROBE_BUILD_ID, result) < 0 || fflush(report) || ferror(report)) return 2;
#ifdef GEKKO
    if (fclose(report)) return 2;
    for (unsigned i = 0; i < 60; ++i) VIDEO_WaitVSync();
#endif
    return result;
}
