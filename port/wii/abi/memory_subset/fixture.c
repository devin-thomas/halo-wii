#include "shim.h"

int main(void)
{
    unsigned int checks = 0;
#define REQUIRE(condition, name) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "MEMORY FAIL %s offset=%u check=%u\n", name, start, checks); \
    return 1; } } while (0)
    const byte expected[] = {0x5a, 0x12, 0x34, 0x12, 0x34, 0x56, 0x78,
                             0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const byte one = 0x5a;
    const short two = 0x1234;
    const long four = 0x12345678;
    const __int64 eight = 0x1122334455667788LL;
    for (unsigned int start = 0; start < 8; ++start) {
        union { uint64_t alignment; byte bytes[40]; } storage;
        memset(storage.bytes, 0xcc, sizeof(storage.bytes));
        struct data_encoding_state state;
        data_encode_new(&state, storage.bytes + start, 32);
        REQUIRE(data_encode_memory(&state, &one, 1, 1) &&
                data_encode_memory(&state, &two, 1, -2) &&
                data_encode_memory(&state, &four, 1, -4) &&
                data_encode_memory(&state, &eight, 1, -8), "encode_scalars");
        REQUIRE(state.offset == 15 && !state.overflow, "encode_state");
        /* Exact Xbox/x86 wire bytes, not just a same-target round trip. */
        REQUIRE(memcmp(storage.bytes + start, expected, sizeof(expected)) == 0, "wire_big_endian_golden");
        REQUIRE(storage.bytes[start + 15] == 0xcc &&
                (start == 0 || storage.bytes[start - 1] == 0xcc), "encode_canaries");
        data_decode_new(&state, storage.bytes + start, 15);
        REQUIRE(data_decode_byte(&state) == one && data_decode_short(&state) == two &&
                data_decode_long(&state) == four && data_decode_int64(&state) == eight,
                "decode_scalars");
        REQUIRE(state.offset == 15 && !state.overflow, "decode_state");
        byte native[15];
        memcpy(native, &one, 1);
        memcpy(native + 1, &two, 2);
        memcpy(native + 3, &four, 4);
        memcpy(native + 7, &eight, 8);
        REQUIRE(memcmp(storage.bytes + start, native, sizeof(native)) == 0, "decode_mutates_to_native");
        REQUIRE(storage.bytes[start + 15] == 0xcc &&
                (start == 0 || storage.bytes[start - 1] == 0xcc), "decode_canaries");
        memset(storage.bytes, 0xcc, sizeof(storage.bytes));
        byte snapshot[sizeof(storage.bytes)];
        memcpy(snapshot, storage.bytes, sizeof(snapshot));
        data_decode_new(&state, storage.bytes + start, 3);
        REQUIRE(data_decode_long(&state) == 0 && state.overflow && state.offset == 0 &&
                memcmp(storage.bytes, snapshot, sizeof(snapshot)) == 0,
                "decode_truncated");
        REQUIRE(data_decode_byte(&state) == 0 && state.overflow && state.offset == 0 &&
                memcmp(storage.bytes, snapshot, sizeof(snapshot)) == 0, "decode_overflow_sticky");
        data_encode_new(&state, storage.bytes + start, 3);
        REQUIRE(!data_encode_memory(&state, &four, 1, -4) && state.overflow && state.offset == 0 &&
                memcmp(storage.bytes, snapshot, sizeof(snapshot)) == 0,
                "encode_truncated");
        REQUIRE(!data_encode_memory(&state, &one, 1, 1) && state.overflow &&
                state.offset == 0 && memcmp(storage.bytes, snapshot, sizeof(snapshot)) == 0,
                "encode_overflow_sticky");
        data_encode_new(&state, storage.bytes + start, 32);
        REQUIRE(data_encode_integer(&state, one, 255) &&
                data_encode_integer(&state, two, 65535) &&
                data_encode_integer(&state, four, 0x7fffffff), "integer_encode_dispatch");
        REQUIRE(state.offset == 7 && memcmp(storage.bytes + start, expected, 7) == 0,
                "integer_dispatch_wire");
        data_decode_new(&state, storage.bytes + start, 7);
        REQUIRE(data_decode_integer(&state, 255) == one &&
                data_decode_integer(&state, 65535) == two &&
                data_decode_integer(&state, 0x7fffffff) == four, "integer_decode_dispatch");
        REQUIRE(state.offset == 7 && !state.overflow, "integer_decode_state");
    }
    if (printf("MEMORY SUBSET checks=%u offsets=8 wire=5a1234123456781122334455667788 mutation=native\n", checks) < 0 ||
        fflush(stdout) != 0) return 1;
    return 0;
#undef REQUIRE
}
