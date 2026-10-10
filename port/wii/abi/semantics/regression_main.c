/* Host-only x86 regression for the ADR-018 engine changes
   (run_abi_semantics.py --regression-base <commit>). The original_* bodies
   are the engine functions as they were at that commit (abi_original.c); the
   others are the current ones (abi_engine.c). On a little-endian x86 port the
   two must behave identically: same bytes written, same results, same
   applied events, same assertions.

   Excluded by design, and reported: message headers whose size field is 0 or
   1. The original wraps its block count and enciphers up to 0xFFFF blocks
   past the message; the current code leaves such a message untouched. */
#include "abi_engine_types.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void original_message_encrypt(word *msgptr, unsigned long const key[2]);
void original_message_decrypt(word *msgptr, unsigned long const key[2]);
boolean original_recorded_animation_apply_event_stream(struct animation_playback_controller *animation_state,
                                                       struct recorded_unit_control *control, long *ticks,
                                                       byte const **playback_stream, byte const *playback_stream_end);

unsigned long wii_abi_engine_assertions;
unsigned long wii_abi_playback_damaged;
static const byte *trace_base;
static uint64_t trace_digest;

void wii_abi_playback_trace(unsigned event_type, byte const *position)
{
    uint64_t value = (uint64_t)event_type << 32 | (uint64_t)(position - trace_base);
    for (int i = 0; i < 8; ++i) {
        trace_digest ^= (value >> (8 * i)) & 0xffu;
        trace_digest *= UINT64_C(0x100000001b3);
    }
}

static uint32_t rng_state = 0x6d2b79f5u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

typedef boolean (*apply_stream)(struct animation_playback_controller *, struct recorded_unit_control *, long *,
                                byte const **, byte const *);

struct outcome
{
    boolean result;
    long ticks;
    size_t position;
    unsigned long damaged;
    unsigned long assertions;
    uint64_t trace;
};

static struct outcome play(apply_stream apply, const byte *stream, size_t length, long ticks)
{
    struct animation_playback_controller animation_state;
    struct recorded_unit_control control;
    memset(&animation_state, 0, sizeof(animation_state));
    memset(&control, 0, sizeof(control));
    const byte *cursor = stream;
    struct outcome outcome;
    unsigned long damaged = wii_abi_playback_damaged, assertions = wii_abi_engine_assertions;
    trace_base = stream;
    trace_digest = UINT64_C(0xcbf29ce484222325);
    outcome.result = apply(&animation_state, &control, &ticks, &cursor, stream + length);
    outcome.ticks = ticks;
    outcome.position = (size_t)(cursor - stream);
    outcome.damaged = wii_abi_playback_damaged - damaged;
    outcome.assertions = wii_abi_engine_assertions - assertions;
    outcome.trace = trace_digest;
    return outcome;
}

static int same(const struct outcome *a, const struct outcome *b)
{
    return a->result == b->result && a->ticks == b->ticks && a->position == b->position &&
           a->damaged == b->damaged && a->assertions == b->assertions && a->trace == b->trace;
}

int main(void)
{
    int failed = 0;

    /* message_encrypt/message_decrypt: every size 2..0xFFF, both header
       states, random type, payload, key and trailing canary */
    static union { unsigned long align; word words[2200]; } a, b;
    byte *pa = (byte *)a.words, *pb = (byte *)b.words;
    unsigned long cases = 0, mismatches = 0;
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned size = 2; size <= MAXIMUM_MESSAGE_SIZE; ++size) {
            const unsigned long key[2] = {rng(), rng()};
            for (size_t i = 0; i < sizeof(a.words); ++i) pa[i] = (byte)rng();
            build_message_header(&a.words[0], (word)size, (byte)(1 + rng() % 3), (byte)(rng() % 4));
            memcpy(pb, pa, sizeof(a.words));
            if (round == 0) {
                message_encrypt(&a.words[0], key);
                original_message_encrypt(&b.words[0], key);
            } else {
                message_decrypt(&a.words[0], key);
                original_message_decrypt(&b.words[0], key);
            }
            ++cases;
            mismatches += memcmp(pa, pb, sizeof(a.words)) != 0;
        }
    }
    printf("REGRESSION message_crypt cases=%lu mismatches=%lu excluded_sizes=0,1\n", cases, mismatches);
    failed |= mismatches != 0;

    /* recorded_animation_apply_event_stream: every stream of up to 3 bytes at
       three tick budgets, then generated streams up to 48 bytes */
    static const long budgets[3] = {0, 2, 300};
    byte stream[64];
    cases = 0;
    mismatches = 0;
    unsigned long assertions = 0, damaged = 0;
    for (size_t length = 0; length <= 3; ++length) {
        const uint32_t limit = length == 0 ? 1u : 1u << (8 * length);
        for (uint32_t value = 0; value < limit; ++value) {
            for (size_t i = 0; i < length; ++i) stream[i] = (byte)(value >> (8 * i));
            for (int budget = 0; budget < 3; ++budget) {
                struct outcome x = play(recorded_animation_apply_event_stream, stream, length, budgets[budget]);
                struct outcome y = play(original_recorded_animation_apply_event_stream, stream, length,
                                        budgets[budget]);
                ++cases;
                mismatches += !same(&x, &y);
                assertions += x.assertions;
                damaged += x.damaged;
            }
        }
    }
    printf("REGRESSION playback_exhaustive_3_bytes cases=%lu mismatches=%lu damaged=%lu assertions=%lu\n", cases,
           mismatches, damaged, assertions);
    failed |= mismatches != 0;
    cases = 0;
    mismatches = 0;
    assertions = 0;
    damaged = 0;
    unsigned long applied = 0;
    for (unsigned i = 0; i < 1u << 18; ++i) {
        size_t n = 0;
        unsigned events = rng() % 6;
        for (unsigned e = 0; e < events; ++e) {
            unsigned kind = rng() % 4, type = rng() % 25;
            stream[n++] = (byte)(kind | type << 2);
            if (kind == 2) stream[n++] = (byte)(rng() % 4 == 0 ? rng() % 2 : 2 + rng() % 254);
            if (kind == 3) {
                unsigned delta = rng() % 4 == 0 ? rng() % 256 : 256 + rng() % 65280;
                stream[n++] = (byte)(delta & 0xff);
                stream[n++] = (byte)(delta >> 8);
            }
            for (unsigned d = rng() % 9; d > 0; --d) stream[n++] = (byte)rng();
        }
        if (rng() % 2) stream[n++] = (byte)(rng() % 4 | 1 << 2);
        size_t length = rng() % 4 == 0 ? rng() % (n + 1) : n;
        long ticks = rng() % 8 == 0 ? -(long)(rng() % 100) : (long)(rng() % 0x3000);
        struct outcome x = play(recorded_animation_apply_event_stream, stream, length, ticks);
        struct outcome y = play(original_recorded_animation_apply_event_stream, stream, length, ticks);
        ++cases;
        mismatches += !same(&x, &y);
        assertions += x.assertions;
        damaged += x.damaged;
        applied += x.position > 0;
    }
    printf("REGRESSION playback_generated cases=%lu mismatches=%lu damaged=%lu assertions=%lu advanced=%lu\n", cases,
           mismatches, damaged, assertions, applied);
    failed |= mismatches != 0;
    printf("REGRESSION result=%d\n", failed);
    return failed;
}
