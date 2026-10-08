#include "cache_stream_fixture.h"
#include "cache_stream_io.h"
#include "cache_address_owned.h"
#include <stdlib.h>
#include <string.h>

#ifdef GEKKO
int wii_cache_stream_fixture(FILE *report, int collect_failures)
{
    (void)collect_failures;
    return !report ||
        fprintf(report, "STREAM SYNTHETIC not_executed target=PPC reason=host_tmpfile_only\n") < 0;
}
#else
enum fault {
    NO_FAULT, BAD_CRC, STALE_TAG, STALE_IO0, STALE_IO1, INACTIVE_OWNER,
    TAG_IO_ALIAS, IO_ALIAS, NULL_FILE, NULL_OWNER, NULL_TAG, NULL_IO0, NULL_IO1
};

struct stream_case {
    const char *name;
    size_t expected;
    size_t stored;
    size_t tag_size;
    size_t io0_size;
    size_t io1_size;
    enum fault fault;
    enum cache_stream_error error;
    int golden;
};

struct fixture_context {
    FILE *report;
    size_t cases;
    size_t checks;
    size_t failures;
    int collect;
    int aborted;
};

static unsigned char pattern(size_t index, int golden)
{
    static const unsigned char digits[] = "123456789";
    return golden ? digits[index] : (unsigned char)((index * 37u + 19u) & 255u);
}

static int check(struct fixture_context *context, const char *name, int condition,
                 const char *label)
{
    ++context->checks;
    if (condition)
        return 1;
    ++context->failures;
    if (fprintf(context->report, "STREAM FAIL case=%s check=%s\n", name, label) < 0 ||
        !context->collect)
        context->aborted = 1;
    return 0;
}

#define STREAM_CHECK(condition, label) \
    do { if (!check(context, test->name, (condition), (label)) && context->aborted) goto cleanup; } while (0)

static void run_case(struct fixture_context *context, const struct stream_case *test,
                     unsigned round)
{
    FILE *input = NULL;
    unsigned char *storage = NULL;
    unsigned char *source = NULL;
    unsigned char *snapshot = NULL;
    struct cache_arena_owner owner = {0};
    struct cache_arena_result arena = {0};
    struct cache_arena_plan plan = {0};
    struct cache_arena_handle handles[3] = {{0}};
    size_t capacity = test->tag_size + test->io0_size + test->io1_size + 353;
    size_t total = capacity + 64;
    ++context->cases;
    if (fprintf(context->report, "STREAM CASE name=%s round=%u expected=%lu stored=%lu\n",
                test->name, round, (unsigned long)test->expected,
                (unsigned long)test->stored) < 0)
    {
        context->aborted = 1;
        ++context->failures;
        return;
    }
    storage = malloc(total);
    snapshot = malloc(total);
    size_t source_size = test->stored > test->expected ? test->stored : test->expected;
    source = malloc(source_size ? source_size : 1);
    if (!storage || !snapshot || !source)
    {
        check(context, test->name, 0, "fixture_allocation");
        context->aborted = 1;
        goto cleanup;
    }
    memset(storage, 0xa5, total);
    for (size_t i = 0; i < (test->stored > test->expected ? test->stored : test->expected); ++i)
        source[i] = pattern(i, test->golden);
    input = tmpfile();
    if (!input)
    {
        check(context, test->name, 0, "fixture_tmpfile");
        context->aborted = 1;
        goto cleanup;
    }
    if (!check(context, test->name, fwrite(source, 1, test->stored, input) == test->stored,
               "fixture_file_write"))
    {
        context->aborted = 1;
        goto cleanup;
    }
    if (!check(context, test->name, fflush(input) == 0 && fseek(input, 0, SEEK_SET) == 0,
               "fixture_file_rewind"))
    {
        context->aborted = 1;
        goto cleanup;
    }
    struct cache_arena_request requests[3] = {
        {test->tag_size, 32}, {test->io0_size, 32}, {test->io1_size, 32}
    };
    if (!cache_arena_plan_build(requests, 3, storage + 32, capacity, 257, &plan, &arena) ||
        !cache_arena_owner_bind(&owner, &plan, storage + 32, capacity, &arena))
    {
        check(context, test->name, 0, "fixture_arena_setup");
        context->aborted = 1;
        goto cleanup;
    }
    for (size_t i = 0; i < 3; ++i)
    {
        if (!cache_arena_owner_handle(&owner, i, &handles[i], &arena))
        {
            check(context, test->name, 0, "fixture_handle_setup");
            context->aborted = 1;
            goto cleanup;
        }
    }
    switch (test->fault)
    {
        case STALE_TAG: --handles[0].generation; break;
        case STALE_IO0: --handles[1].generation; break;
        case STALE_IO1: --handles[2].generation; break;
        case INACTIVE_OWNER:
            STREAM_CHECK(cache_arena_owner_release(&owner, &arena), "fixture_inactive_release");
            break;
        case TAG_IO_ALIAS: handles[0] = handles[1]; break;
        case IO_ALIAS: handles[2] = handles[1]; break;
        default: break;
    }
    struct cache_arena_owner before = owner;
    memcpy(snapshot, storage, total);
    uint32_t expected_crc = cache_probe_crc32(source, test->expected);
    if (test->golden)
        STREAM_CHECK(expected_crc == UINT32_C(0xcbf43926), "independent_standard_crc_golden");
    if (test->fault == BAD_CRC)
        expected_crc ^= 1u;
    struct cache_stream_result result = {0};
    int accepted = cache_stream_read(test->fault == NULL_FILE ? NULL : input,
                                    test->fault == NULL_OWNER ? NULL : &owner,
                                    test->fault == NULL_TAG ? NULL : &handles[0],
                                    test->fault == NULL_IO0 ? NULL : &handles[1],
                                    test->fault == NULL_IO1 ? NULL : &handles[2],
                                    test->expected, expected_crc, &result);
    STREAM_CHECK(accepted == (test->error == CACHE_STREAM_OK), "acceptance");
    STREAM_CHECK(result.error == test->error, "explicit_error");
    STREAM_CHECK(memcmp(&before, &owner, sizeof(owner)) == 0, "owner_unchanged");
    STREAM_CHECK(result.bytes <= test->expected, "copied_byte_bound");
    size_t copied = test->stored < test->expected ? test->stored : test->expected;
    if (test->error <= CACHE_STREAM_ALIAS && test->error != CACHE_STREAM_OK)
    {
        STREAM_CHECK(result.read_calls == 0 && result.chunks == 0 && result.bytes == 0,
                     "preflight_no_io");
        STREAM_CHECK(memcmp(storage, snapshot, total) == 0, "preflight_full_storage_preserved");
        STREAM_CHECK(ftell(input) == 0, "preflight_file_position");
    }
    else
    {
        size_t chunks = copied / CACHE_STREAM_IO_BYTES + (copied % CACHE_STREAM_IO_BYTES != 0);
        size_t calls = test->error == CACHE_STREAM_SHORT ?
            copied / CACHE_STREAM_IO_BYTES + 1 : chunks + 1;
        STREAM_CHECK(result.bytes == copied && result.chunks == chunks && result.read_calls == calls,
                     "exact_io_statistics");
        STREAM_CHECK(memcmp(storage + 32 + plan.slots[0].offset, source, copied) == 0,
                     "tag_copied_prefix");
        if (test->error != CACHE_STREAM_SHORT)
            STREAM_CHECK(result.actual_crc32 == cache_probe_crc32(source, test->expected),
                         "actual_loaded_crc");
        if (test->error == CACHE_STREAM_OK)
        {
            unsigned char *first = storage + 32 + plan.slots[1].offset;
            unsigned char *second = storage + 32 + plan.slots[2].offset;
            size_t first_bytes = copied < CACHE_STREAM_IO_BYTES ? copied : CACHE_STREAM_IO_BYTES;
            STREAM_CHECK(memcmp(first, source, first_bytes) == 0, "first_io_buffer");
            if (copied > CACHE_STREAM_IO_BYTES)
                STREAM_CHECK(memcmp(second, source + CACHE_STREAM_IO_BYTES,
                                    copied - CACHE_STREAM_IO_BYTES) == 0, "alternating_second_buffer");
            else
                STREAM_CHECK(memcmp(second, snapshot + 32 + plan.slots[2].offset,
                                    test->io1_size) == 0, "unused_second_buffer_preserved");
        }
    }
    int guards_preserved = 1;
    for (size_t i = 0; i < total; ++i)
    {
        int writable = 0;
        for (size_t slot = 0; slot < 3; ++slot)
        {
            size_t begin = 32 + plan.slots[slot].offset;
            size_t length = slot == 0 ? result.bytes : plan.slots[slot].size;
            if (i >= begin && i - begin < length)
                writable = 1;
        }
        if (!writable && storage[i] != snapshot[i])
            guards_preserved = 0;
    }
    STREAM_CHECK(guards_preserved, "prefix_suffix_padding_reserve_and_unwritten_tag_guards");
    if (result.error != CACHE_STREAM_OK)
    {
        struct cache_stream_result saved = result;
        long position = ftell(input);
        memcpy(snapshot, storage, total);
        STREAM_CHECK(!cache_stream_read(input, &owner, &handles[0], &handles[1], &handles[2],
                                       test->expected, expected_crc, &result), "sticky_rejected");
        STREAM_CHECK(memcmp(&saved, &result, sizeof(result)) == 0, "sticky_stats_error_preserved");
        STREAM_CHECK(ftell(input) == position && memcmp(storage, snapshot, total) == 0,
                     "sticky_no_read_or_write");
        STREAM_CHECK(fseek(input, 0, SEEK_SET) == 0, "explicit_reset_file_rewind");
        memset(&result, 0, sizeof(result));
        STREAM_CHECK(!cache_stream_read(test->fault == NULL_FILE ? NULL : input,
                                       test->fault == NULL_OWNER ? NULL : &owner,
                                       test->fault == NULL_TAG ? NULL : &handles[0],
                                       test->fault == NULL_IO0 ? NULL : &handles[1],
                                       test->fault == NULL_IO1 ? NULL : &handles[2],
                                       test->expected, expected_crc, &result), "reset_rechecks_error");
        STREAM_CHECK(result.error == saved.error && result.arena_error == saved.arena_error &&
                     result.bytes == saved.bytes && result.chunks == saved.chunks &&
                     result.read_calls == saved.read_calls && result.actual_crc32 == saved.actual_crc32,
                     "reset_reproduces_exact_statistics");
        STREAM_CHECK(memcmp(&before, &owner, sizeof(owner)) == 0 &&
                     memcmp(storage, snapshot, total) == 0, "reset_owner_storage_preserved");
    }
cleanup:
    if (owner.live && !cache_arena_owner_release(&owner, &arena))
    {
        check(context, test->name, 0, "fixture_owner_release");
        context->aborted = 1;
    }
    if (input && fclose(input) != 0)
    {
        check(context, test->name, 0, "fixture_file_close");
        context->aborted = 1;
    }
    free(source);
    free(snapshot);
    free(storage);
}

#define IO_SIZE CACHE_STREAM_IO_BYTES
#define CASE(name, expected, stored, fault, error) \
    {name, expected, stored, (expected) + 17, IO_SIZE, IO_SIZE, fault, error, 0}

int wii_cache_stream_fixture(FILE *report, int collect_failures)
{
    static const struct stream_case tests[] = {
        CASE("exact_zero", 0, 0, NO_FAULT, CACHE_STREAM_OK),
        CASE("exact_one", 1, 1, NO_FAULT, CACHE_STREAM_OK),
        {"crc_standard_golden", 9, 9, 26, IO_SIZE, IO_SIZE, NO_FAULT, CACHE_STREAM_OK, 1},
        CASE("exact_65535", 65535, 65535, NO_FAULT, CACHE_STREAM_OK),
        CASE("exact_65536", 65536, 65536, NO_FAULT, CACHE_STREAM_OK),
        CASE("exact_65537", 65537, 65537, NO_FAULT, CACHE_STREAM_OK),
        CASE("short_empty", 1, 0, NO_FAULT, CACHE_STREAM_SHORT),
        CASE("short_partial_buffer", 65536, 65535, NO_FAULT, CACHE_STREAM_SHORT),
        CASE("short_after_full_buffer", 65537, 65536, NO_FAULT, CACHE_STREAM_SHORT),
        CASE("trailing_empty_expected", 0, 1, NO_FAULT, CACHE_STREAM_TRAILING),
        CASE("trailing_one", 1, 2, NO_FAULT, CACHE_STREAM_TRAILING),
        CASE("trailing_two_buffers", 65537, 65538, NO_FAULT, CACHE_STREAM_TRAILING),
        CASE("crc_one", 1, 1, BAD_CRC, CACHE_STREAM_CRC),
        CASE("crc_two_buffers", 65537, 65537, BAD_CRC, CACHE_STREAM_CRC),
        {"tag_capacity", 65537, 65537, 65536, IO_SIZE, IO_SIZE, NO_FAULT, CACHE_STREAM_CAPACITY, 0},
        {"io0_capacity", 1, 1, 18, IO_SIZE - 1, IO_SIZE, NO_FAULT, CACHE_STREAM_CAPACITY, 0},
        {"io1_capacity", 1, 1, 18, IO_SIZE, IO_SIZE - 1, NO_FAULT, CACHE_STREAM_CAPACITY, 0},
        {"io0_oversized", 1, 1, 18, IO_SIZE + 1, IO_SIZE, NO_FAULT, CACHE_STREAM_CAPACITY, 0},
        {"io1_oversized", 1, 1, 18, IO_SIZE, IO_SIZE + 1, NO_FAULT, CACHE_STREAM_CAPACITY, 0},
        CASE("tag_io_alias", 1, 1, TAG_IO_ALIAS, CACHE_STREAM_ALIAS),
        CASE("io_alias", 1, 1, IO_ALIAS, CACHE_STREAM_ALIAS),
        CASE("stale_tag", 1, 1, STALE_TAG, CACHE_STREAM_HANDLE),
        CASE("stale_io0", 1, 1, STALE_IO0, CACHE_STREAM_HANDLE),
        CASE("stale_io1", 1, 1, STALE_IO1, CACHE_STREAM_HANDLE),
        CASE("inactive_owner", 1, 1, INACTIVE_OWNER, CACHE_STREAM_HANDLE),
        CASE("null_file", 1, 1, NULL_FILE, CACHE_STREAM_ARGUMENT),
        CASE("null_owner", 1, 1, NULL_OWNER, CACHE_STREAM_ARGUMENT),
        CASE("null_tag", 1, 1, NULL_TAG, CACHE_STREAM_ARGUMENT),
        CASE("null_io0", 1, 1, NULL_IO0, CACHE_STREAM_ARGUMENT),
        CASE("null_io1", 1, 1, NULL_IO1, CACHE_STREAM_ARGUMENT)
    };
    struct fixture_context context = {report, 0, 0, 0, collect_failures != 0, 0};
    if (!report || fprintf(report, "STREAM BEGIN scope=host_synthetic_tmpfile_only io_bytes=65536\n") < 0)
        return 1;
    for (unsigned round = 0; round < 2 && !context.aborted; ++round)
        for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]) && !context.aborted; ++i)
            run_case(&context, &tests[i], round);
    if (fprintf(report, "STREAM SUMMARY cases=%lu checks=%lu failures=%lu aborted=%d runtime_io_error_injection=not_performed\n",
                (unsigned long)context.cases, (unsigned long)context.checks,
                (unsigned long)context.failures, context.aborted) < 0 ||
        fprintf(report, "STREAM END result=%d\n", context.failures != 0 || context.aborted) < 0 ||
        ferror(report))
        return 1;
    return context.failures != 0 || context.aborted;
}
#endif
