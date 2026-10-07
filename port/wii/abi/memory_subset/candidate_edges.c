#include "candidate.h"
#include <limits.h>

int wii_memory_candidate_edges(FILE *report, int collect_failures)
{
    unsigned int checks = 0, failures = 0;
#define REQUIRE(condition, name) do { ++checks; if (!(condition)) { \
    ++failures; \
    if (fprintf(report, "CANDIDATE FAIL %s offset=%u check=%u\n", name, start, checks) < 0 || \
        fflush(report) != 0 || !collect_failures) return 1; } } while (0)
    const byte golden[] = {0x5a, 0x12, 0x34, 0x12, 0x34, 0x56, 0x78,
                           0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const byte one = 0x5a; const short two = 0x1234; const long four = 0x12345678;
    const __int64 eight = 0x1122334455667788LL;
    const uint16_t shorts[] = {0xfffe, 0x8001};
    const uint32_t longs[] = {0x89abcdef, 0x01234567};
    const uint64_t int64s[] = {UINT64_C(0xfedcba9876543210), UINT64_C(0x0123456789abcdef)};
    const byte wires[][16] = {
        {0xff, 0xfe, 0x80, 0x01},
        {0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67},
        {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
         0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}
    };
    for (unsigned int start = 0; start < 8; ++start) {
        union { uint64_t alignment; byte bytes[40]; } storage, source;
        byte snapshot[40], expected[40];
        struct data_encoding_state state;
        memset(storage.bytes, 0xcc, 40);
        memcpy(storage.bytes + start, golden, 15);
        memcpy(expected, storage.bytes, 40);
        memcpy(expected + start, &one, 1); memcpy(expected + start + 1, &two, 2);
        memcpy(expected + start + 3, &four, 4); memcpy(expected + start + 7, &eight, 8);
        data_decode_new(&state, storage.bytes + start, 15);
        REQUIRE(candidate_decode_byte(&state) == one && candidate_decode_short(&state) == two &&
                candidate_decode_long(&state) == four && candidate_decode_int64(&state) == eight &&
                state.offset == 15 && !state.overflow, "independent_golden_decode");
        REQUIRE(memcmp(storage.bytes, expected, 40) == 0, "independent_decode_mutation_canaries");

        const long widths[] = {2, 4, 8};
        const void *arrays[] = {shorts, longs, int64s};
        for (unsigned int a = 0; a < 3; ++a) {
            long width = widths[a];
            memset(storage.bytes, 0xcc, 40);
            memcpy(storage.bytes + start, wires[a], (size_t)width);
            memcpy(expected, storage.bytes, 40);
            memcpy(expected + start, arrays[a], (size_t)width);
            data_decode_new(&state, storage.bytes + start, width);
            if (a == 0) {
                short value; memcpy(&value, shorts, 2);
                REQUIRE(candidate_decode_short(&state) == value && !state.overflow &&
                        memcmp(storage.bytes, expected, 40) == 0, "signed_short_bits");
            } else if (a == 1) {
                long value; memcpy(&value, longs, 4);
                REQUIRE(candidate_decode_long(&state) == value && !state.overflow &&
                        memcmp(storage.bytes, expected, 40) == 0, "signed_long_bits");
            } else {
                __int64 value; memcpy(&value, int64s, 8);
                REQUIRE(candidate_decode_int64(&state) == value && !state.overflow &&
                        memcmp(storage.bytes, expected, 40) == 0, "signed_int64_bits");
            }
            memset(storage.bytes, 0xcc, 40);
            memset(source.bytes, 0xdd, 40);
            memcpy(source.bytes + (7 - start), arrays[a], (size_t)(2 * width));
            memcpy(snapshot, source.bytes, 40);
            memcpy(expected, storage.bytes, 40);
            memcpy(expected + start, wires[a], (size_t)(2 * width));
            data_encode_new(&state, storage.bytes + start, 32);
            REQUIRE(candidate_encode_memory(&state, source.bytes + (7 - start), 2, -width) &&
                    state.offset == 2 * width && !state.overflow &&
                    memcmp(storage.bytes, expected, 40) == 0 &&
                    memcmp(source.bytes, snapshot, 40) == 0, "unaligned_array_wire_source_canaries");
            memcpy(expected + start, arrays[a], (size_t)(2 * width));
            data_decode_new(&state, storage.bytes + start, 2 * width);
            REQUIRE(candidate_decode_memory(&state, 2, -width) == storage.bytes + start &&
                    state.offset == 2 * width && !state.overflow &&
                    memcmp(storage.bytes, expected, 40) == 0, "unaligned_array_native_canaries");
        }
        memset(storage.bytes, 0xcc, 40); memcpy(expected, storage.bytes, 40);
        memset(expected + start, 0, 16);
        data_encode_new(&state, storage.bytes + start, 32);
        REQUIRE(candidate_encode_memory(&state, NULL, 2, -8) && state.offset == 16 &&
                memcmp(storage.bytes, expected, 40) == 0, "null_source_zero_fill");
        memset(storage.bytes, 0xcc, 40); memcpy(expected, storage.bytes, 40);
        memcpy(expected + start, golden + 1, 3);
        data_encode_new(&state, storage.bytes + start, 3);
        REQUIRE(candidate_encode_memory(&state, golden + 1, 3, 1) && state.offset == 3 &&
                memcmp(storage.bytes, expected, 40) == 0, "raw_bytes_no_swap");
        REQUIRE(candidate_decode_memory(&state, 0, -8) == storage.bytes + start + 3 &&
                state.offset == 3 && !state.overflow && memcmp(storage.bytes, expected, 40) == 0,
                "zero_decode_at_end");
        data_encode_new(&state, storage.bytes + start, 3);
        REQUIRE(candidate_encode_memory(&state, NULL, 0, -8) && state.offset == 0 &&
                !state.overflow && memcmp(storage.bytes, expected, 40) == 0, "zero_encode_no_change");
        memset(storage.bytes, 0xcc, 40); memcpy(expected, storage.bytes, 40);
        expected[start] = 0xff; expected[start + 1] = 0xff;
        data_encode_new(&state, storage.bytes + start, 3);
        REQUIRE(candidate_encode_integer(&state, 65535, 65535) && state.offset == 2 &&
                memcmp(storage.bytes, expected, 40) == 0, "integer_signed_width_wire");
        short minus_one = -1; memcpy(expected + start, &minus_one, 2);
        data_decode_new(&state, storage.bytes + start, 2);
        REQUIRE(candidate_decode_integer(&state, 65535) == -1 && state.offset == 2 &&
                !state.overflow && memcmp(storage.bytes, expected, 40) == 0, "integer_signed_width_result");

        memcpy(snapshot, storage.bytes, 40);
        const short counts[] = {-1, 1, 32767};
        const long sizes[] = {-2, 2, -8};
        for (unsigned int a = 0; a < 3; ++a) {
            data_encode_new(&state, storage.bytes + start, 3);
            REQUIRE(!candidate_encode_memory(&state, &eight, counts[a], sizes[a]) &&
                    state.overflow && state.offset == 0 &&
                    memcmp(storage.bytes, snapshot, 40) == 0, "malformed_encode_unchanged");
            data_decode_new(&state, storage.bytes + start, 3);
            REQUIRE(candidate_decode_memory(&state, counts[a], sizes[a]) == NULL &&
                    state.overflow && state.offset == 0 &&
                    memcmp(storage.bytes, snapshot, 40) == 0, "malformed_decode_unchanged");
        }
        data_encode_new(&state, storage.bytes, LONG_MAX); state.offset = LONG_MAX - 1;
        REQUIRE(!candidate_encode_memory(&state, &eight, 2, -8) && state.overflow &&
                state.offset == LONG_MAX - 1 && memcmp(storage.bytes, snapshot, 40) == 0,
                "encode_cursor_overflow_rejected");
        data_decode_new(&state, storage.bytes, LONG_MAX); state.offset = LONG_MAX - 1;
        REQUIRE(candidate_decode_memory(&state, 2, -8) == NULL && state.overflow &&
                state.offset == LONG_MAX - 1 && memcmp(storage.bytes, snapshot, 40) == 0,
                "decode_cursor_overflow_rejected");
        data_encode_new(&state, storage.bytes + start, 3);
        REQUIRE(!candidate_encode_memory(&state, NULL, 1, LONG_MIN) && state.overflow &&
                state.offset == 0 && memcmp(storage.bytes, snapshot, 40) == 0,
                "minimum_size_rejected");
    }
    if (fprintf(report, "CANDIDATE EDGES checks=%u offsets=8 failures=%u\n", checks, failures) < 0 ||
        fflush(report) != 0) return 1;
    return failures != 0;
#undef REQUIRE
}
