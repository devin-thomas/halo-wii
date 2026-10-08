#include "cache_sha256_fixture.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SHA_INPUT_LIMIT ((size_t)22 * 1048576u)

int cache_sha256_hex(const void *data, size_t bytes, char output[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    char encoded[65];
    uintptr_t input = (uintptr_t)data, destination = (uintptr_t)output;
    if (!output || (!data && bytes) || bytes > SHA_INPUT_LIMIT || bytes > INT_MAX ||
            destination > UINTPTR_MAX - 65u || input > UINTPTR_MAX - bytes)
        return 0;
    if (bytes && input < destination + 65u && destination < input + bytes)
        return 0;
    p2p_sha256(data, (int)bytes, digest);
    for (size_t i = 0; i < sizeof(digest); ++i)
    {
        encoded[i * 2] = digits[digest[i] >> 4];
        encoded[i * 2 + 1] = digits[digest[i] & 15u];
    }
    encoded[64] = 0;
    memcpy(output, encoded, sizeof(encoded));
    return 1;
}

struct sha_checks
{
    FILE *report;
    unsigned checks, failures, vectors;
};

static void expect(struct sha_checks *state, int condition, const char *name)
{
    ++state->checks;
    if (!condition)
    {
        ++state->failures;
        fprintf(state->report, "CACHE_SHA256 FAIL check=%s\n", name);
    }
}

static void vector(struct sha_checks *state, const void *data, size_t bytes,
                   const char *name, const char expected_hex[65])
{
    unsigned char actual[32], expected[32];
    char encoded[65];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(expected); ++i)
    {
        size_t high = (size_t)(strchr(digits, expected_hex[i * 2]) - digits);
        size_t low = (size_t)(strchr(digits, expected_hex[i * 2 + 1]) - digits);
        expected[i] = (unsigned char)(high * 16u + low);
    }
    p2p_sha256(data, (int)bytes, actual);
    expect(state, !memcmp(actual, expected, sizeof(expected)), name);
    memset(encoded, 0x6b, sizeof(encoded));
    int ok = cache_sha256_hex(data, bytes, encoded);
    expect(state, ok, name);
    expect(state, !memcmp(encoded, expected_hex, sizeof(encoded)), name);
    ++state->vectors;
}

int cache_sha256_fixture(FILE *report)
{
    static const struct
    {
        size_t bytes;
        const char *hex;
    } padding[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}
    };
    struct sha_checks state = {report, 0, 0, 0};
    if (!report)
        return 1;
    fprintf(report, "CACHE_SHA256 BEGIN scope=authored_vectors_raw_byte_identity\n");
    vector(&state, NULL, 0, "empty",
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    vector(&state, "abc", 3, "abc",
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    unsigned char small[65];
    memset(small, 'a', sizeof(small));
    for (size_t i = 0; i < sizeof(padding) / sizeof(padding[0]); ++i)
    {
        char label[24];
        snprintf(label, sizeof(label), "padding_a_%lu", (unsigned long)padding[i].bytes);
        vector(&state, small, padding[i].bytes, label, padding[i].hex);
    }
    int small_unchanged = 1;
    for (size_t i = 0; i < sizeof(small); ++i)
        if (small[i] != 'a')
            small_unchanged = 0;
    expect(&state, small_unchanged, "padding_input_unchanged");
    unsigned char *authored = malloc(9416);
    int allocated = authored != NULL;
    expect(&state, allocated, "authored_allocation");
    if (authored)
    {
        for (size_t i = 0; i < 9416; ++i)
            authored[i] = (unsigned char)(i & 255u);
        vector(&state, authored, 9416, "authored_i_mod256_9416",
               "6188242db2fb9b021dec50f8ab8c464a71f503685a0da02d17b017c09721e5cc");
        int unchanged = 1;
        for (size_t i = 0; i < 9416; ++i)
            if (authored[i] != (unsigned char)(i & 255u))
                unchanged = 0;
        expect(&state, unchanged, "authored_input_unchanged");
        free(authored);
    }
    char output[65], snapshot[65];
    memset(output, 0x53, sizeof(output));
    memcpy(snapshot, output, sizeof(snapshot));
    expect(&state, !cache_sha256_hex(NULL, 1, output), "NULL_input_rejected");
    expect(&state, !memcmp(output, snapshot, sizeof(output)), "NULL_input_output_unchanged");
    expect(&state, !cache_sha256_hex(small, SHA_INPUT_LIMIT + 1u, output), "limit_excess_rejected");
    expect(&state, !memcmp(output, snapshot, sizeof(output)), "limit_excess_output_unchanged");
    expect(&state, !cache_sha256_hex(small, (size_t)INT_MAX + 1u, output), "int_excess_rejected");
    expect(&state, !memcmp(output, snapshot, sizeof(output)), "int_excess_output_unchanged");
    expect(&state, !cache_sha256_hex(small, SIZE_MAX, output), "size_excess_rejected");
    expect(&state, !memcmp(output, snapshot, sizeof(output)), "size_excess_output_unchanged");
    expect(&state, !cache_sha256_hex(small, sizeof(small), NULL), "NULL_output_rejected");
    unsigned char alias[130], alias_snapshot[130];
    memset(alias, 0x7c, sizeof(alias));
    memcpy(alias_snapshot, alias, sizeof(alias));
    expect(&state, !cache_sha256_hex(alias, 65, (char *)alias), "same_range_rejected");
    expect(&state, !memcmp(alias, alias_snapshot, sizeof(alias)), "same_range_unchanged");
    expect(&state, !cache_sha256_hex(alias, 66, (char *)alias + 65), "partial_overlap_rejected");
    expect(&state, !memcmp(alias, alias_snapshot, sizeof(alias)), "partial_overlap_unchanged");
    expect(&state, cache_sha256_hex(alias, 65, (char *)alias + 65), "adjacent_ranges_accepted");
    expect(&state, !memcmp(alias, alias_snapshot, 65), "adjacent_input_unchanged");
    fprintf(report, "CACHE_SHA256 SUMMARY vectors=%u checks=%u failures=%u aborted=%u limit_bytes=%lu authored_heap_released=1\n",
            state.vectors, state.checks, state.failures, allocated ? 0u : 1u,
            (unsigned long)SHA_INPUT_LIMIT);
    fprintf(report, "CACHE_SHA256 END result=%u\n", state.failures != 0);
    return state.failures != 0 || ferror(report);
}
