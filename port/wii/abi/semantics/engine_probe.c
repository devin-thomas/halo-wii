/* Checks over actual engine declarations and bodies (abi_engine_types.h and
   abi_engine.c are generated from the source tree). This unit and the
   generated one compile with the adopted Wii engine semantics
   (tools/wii/build.py ENGINE_SEMANTIC_FLAGS: -fsigned-char, -fshort-wchar,
   ADR-018). References are the semantics the source was written against:
   MSVC for x86, little-endian, LSB-first bit-fields, signed plain char,
   16-bit wchar_t.

   Bit-fields: data crossing an external boundary is decoded explicitly by the
   engine (recorded_animation_decode_event_header, message_header.h's
   GET_MESSAGE_* in message_encrypt/message_decrypt) and checked here as
   contracts. In-memory bit-fields keep the compiler's allocation, which
   ADR-018 allows: their field round trips are contracts, and their raw bytes
   are reported as layout observations (they differ by target, by design). */
#include "abi_engine_types.h"
#include "engine_probe.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define ID(name) "engine." name

/* Values pass through volatile objects so that the compiler cannot fold the
   implementation-defined conversions away or reject constant ones. */
static volatile int volatile_none = _local_player_triggered_switch_none;
static volatile int volatile_two = 2;
static volatile int volatile_twelve = 12;
static volatile int volatile_fifteen = 15;
static volatile long volatile_long_max = 2147483647L;
static volatile long volatile_negative = -17;

/* FNV-1a 64 over explicit little-endian values, so digests compare across
   targets. */
static uint64_t fnv(uint64_t digest, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        digest ^= (value >> (8 * i)) & 0xffu;
        digest *= UINT64_C(0x100000001b3);
    }
    return digest;
}
#define FNV_START UINT64_C(0xcbf29ce484222325)

static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void char_checks(struct wii_abi_report *report)
{
    wii_abi_check_i64(report, ID("char.plain_min"), WII_ABI_CONTRACT, CHAR_MIN, -128);

    /* recorded_animation_playback.c: delta bytes read from the playback stream */
    const uint8_t deltas[2] = {0xf6, 0x05};
    struct vector_char_difference_data difference;
    memcpy(&difference, deltas, sizeof(difference));
    wii_abi_check_i64(report, ID("char.vector_char_difference.delta_yaw"), WII_ABI_CONTRACT,
                      difference.delta_yaw, -10);
    wii_abi_check_i64(report, ID("char.vector_char_difference.delta_pitch"), WII_ABI_CONTRACT,
                      difference.delta_pitch, 5);

    /* players.h: char local_player_triggered_switch : 4 holds NONE; players.c
       tests it against _local_player_triggered_switch_none every tick */
    struct wii_abi_players_bsp_switch_excerpt state;
    memset(&state, 0, sizeof(state));
    state.local_player_triggered_switch = volatile_none;
    int readback = state.local_player_triggered_switch;
    wii_abi_check_i64(report, ID("char.bitfield.triggered_switch_none_value"), WII_ABI_CONTRACT, readback, -1);
    wii_abi_check_i64(report, ID("char.bitfield.triggered_switch_pending_after_none"), WII_ABI_CONTRACT,
                      readback != volatile_none, 0);
}

static void wchar_checks(struct wii_abi_report *report)
{
    /* engine text is UTF-16 code units (tag strings, profiles, saves) */
    static const wchar_t text[] = L"Haloé";
    static const uint16_t units[] = {0x48, 0x61, 0x6c, 0x6f, 0xe9, 0};
    wii_abi_check_u64(report, ID("wchar.bytes"), WII_ABI_CONTRACT, sizeof(wchar_t), 2);
    wii_abi_check_u64(report, ID("wchar.literal_bytes"), WII_ABI_CONTRACT, sizeof(text), sizeof(units));
    unsigned mismatches = 0;
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); ++i) mismatches += (uint16_t)text[i] != units[i];
    wii_abi_check_u64(report, ID("wchar.literal_code_units"), WII_ABI_CONTRACT, mismatches, 0);
    wii_abi_check_u64(report, ID("wchar.unsigned"), WII_ABI_CONTRACT, (wchar_t)-1 > 0, 1);
}

/* In-memory bit-fields: field round trips are contracts, raw bytes are
   target layout observations (never read from or written to external data). */
static void layout_checks(struct wii_abi_report *report)
{
    struct wii_abi_players_bsp_switch_excerpt state;
    memset(&state, 0, sizeof(state));
    wii_abi_check_u64(report, ID("bitfield.players_switch.size"), WII_ABI_CONTRACT, sizeof(state), 1);
    state.local_player_triggered_switch = volatile_two;
    state.bsp_check_recursive_switch_ticks = (byte)volatile_twelve;
    wii_abi_check_i64(report, ID("bitfield.players_switch.fields_2_12.switch"), WII_ABI_CONTRACT,
                      state.local_player_triggered_switch, 2);
    wii_abi_check_i64(report, ID("bitfield.players_switch.fields_2_12.ticks"), WII_ABI_CONTRACT,
                      state.bsp_check_recursive_switch_ticks, 12);
    wii_abi_observe_u64(report, ID("layout.players_switch.state_byte_2_12"), state.bsp_switch_state);
    state.local_player_triggered_switch = volatile_none;
    state.bsp_check_recursive_switch_ticks = (byte)volatile_fifteen;
    wii_abi_check_i64(report, ID("bitfield.players_switch.fields_none_15.switch"), WII_ABI_CONTRACT,
                      state.local_player_triggered_switch, -1);
    wii_abi_check_i64(report, ID("bitfield.players_switch.fields_none_15.ticks"), WII_ABI_CONTRACT,
                      state.bsp_check_recursive_switch_ticks, 15);

    struct animation_event_header header;
    wii_abi_check_u64(report, ID("bitfield.animation_header.size"), WII_ABI_CONTRACT, sizeof(header), 1);
    memset(&header, 0, sizeof(header));
    header.time_delta = 2;
    header.event_type = 5;
    wii_abi_check_u64(report, ID("bitfield.animation_header.fields_2_5"), WII_ABI_CONTRACT,
                      (uint64_t)header.time_delta << 8 | header.event_type, 0x205);
    uint8_t encoded;
    memcpy(&encoded, &header, 1);
    wii_abi_observe_u64(report, ID("layout.animation_header.byte_2_5"), encoded);

    /* hud_nav_points.c: runtime-only datum (not persisted) */
    struct hud_nav_point_datum nav;
    wii_abi_check_u64(report, ID("bitfield.hud_nav_point.size"), WII_ABI_CONTRACT, sizeof(nav), 12);
    wii_abi_check_u64(report, ID("bitfield.hud_nav_point.z_offset_offset"), WII_ABI_CONTRACT,
                      offsetof(struct hud_nav_point_datum, z_offset), 4);
    memset(&nav, 0, sizeof(nav));
    nav.type = (short)(volatile_none - 2);
    nav.screen_type = (short)(volatile_two + 3);
    wii_abi_check_i64(report, ID("bitfield.hud_nav_point.type_signed_readback"), WII_ABI_CONTRACT, nav.type, -3);
    wii_abi_check_i64(report, ID("bitfield.hud_nav_point.screen_type_readback"), WII_ABI_CONTRACT, nav.screen_type, 5);
    uint8_t nav_bytes[2];
    memcpy(nav_bytes, (const uint8_t *)&nav + offsetof(struct hud_nav_point_datum, nav_index) + sizeof(short), 2);
    wii_abi_observe_bytes(report, ID("layout.hud_nav_point.unit_bytes"), nav_bytes, 2);
}

/* recorded_animation_playback.c recorded_animation_decode_event_header: the
   Xbox-authored header byte (kind bits 0-1, event type bits 2-7) and its
   byte or little-endian word time delta. */
static void animation_header_checks(struct wii_abi_report *report)
{
    static const word sizes[4] = {1, 1, 2, 3};
    static const word deltas[4] = {0, 1, 0x34, 0x1234};
    unsigned mismatches = 0, truncations = 0, truncation_failures = 0;
    uint64_t digest = FNV_START;
    for (unsigned b = 0; b < 256; ++b) {
        const byte stream[3] = {(byte)b, 0x34, 0x12};
        const unsigned kind = b & 3u, type = b >> 2;
        struct animation_event_header header;
        word delta = 0xbeef;
        word size = wii_abi_decode_event_header(stream, stream + 3, &header, &delta);
        mismatches += size != sizes[kind] || header.time_delta != kind || header.event_type != type ||
                      delta != deltas[kind];
        digest = fnv(fnv(fnv(fnv(digest, size), header.time_delta), header.event_type), delta);
        for (unsigned available = 0; available < sizes[kind]; ++available) {
            struct animation_event_header untouched, out;
            memset(&untouched, 0xa5, sizeof(untouched));
            out = untouched;
            word out_delta = 0xbeef;
            ++truncations;
            truncation_failures += wii_abi_decode_event_header(stream, stream + available, &out, &out_delta) != 0 ||
                                   out_delta != 0xbeef || memcmp(&out, &untouched, sizeof(out)) != 0;
        }
    }
    wii_abi_check_u64(report, ID("animation_header.decode.all_256_mismatches"), WII_ABI_CONTRACT, mismatches, 0);
    wii_abi_check_u64(report, ID("animation_header.decode.truncated_cases"), WII_ABI_CONTRACT, truncations, 256 / 4 * 7);
    wii_abi_check_u64(report, ID("animation_header.decode.truncated_rejection_failures"), WII_ABI_CONTRACT,
                      truncation_failures, 0);
    wii_abi_observe_u64(report, ID("animation_header.decode.digest"), digest);

    /* the measured PPC overlay misreadings (2026-10-10-abi-semantics) */
    const byte header_0d[1] = {0x0d}, header_fe[2] = {0xfe, 0x34}, header_word[3] = {0x13, 0x34, 0x12};
    struct animation_event_header header;
    word delta = 0;
    wii_abi_check_u64(report, ID("animation_header.decode.0d.size"), WII_ABI_CONTRACT,
                      wii_abi_decode_event_header(header_0d, header_0d + 1, &header, &delta), 1);
    wii_abi_check_u64(report, ID("animation_header.decode.0d.kind_type"), WII_ABI_CONTRACT,
                      (uint64_t)header.time_delta << 8 | header.event_type, 0x103);
    wii_abi_check_u64(report, ID("animation_header.decode.fe.size"), WII_ABI_CONTRACT,
                      wii_abi_decode_event_header(header_fe, header_fe + 2, &header, &delta), 2);
    wii_abi_check_u64(report, ID("animation_header.decode.fe.kind_type_delta"), WII_ABI_CONTRACT,
                      (uint64_t)header.time_delta << 16 | (uint64_t)header.event_type << 8 | delta, 0x23f34);
    wii_abi_check_u64(report, ID("animation_header.decode.word_delta_le"), WII_ABI_CONTRACT,
                      wii_abi_decode_event_header(header_word, header_word + 3, &header, &delta) == 3 &&
                      header.event_type == 4 ? delta : 0, 0x1234);
}

/* recorded_animation_apply_event_stream, with the traced apply stub
   (engine_playback_glue.h) */
unsigned long wii_abi_playback_damaged;
static const byte *trace_base;
static uint64_t trace_digest;
static unsigned trace_count;
static unsigned trace_types[8];
static unsigned trace_offsets[8];

void wii_abi_playback_trace(unsigned event_type, byte const *position)
{
    unsigned offset = (unsigned)(position - trace_base);
    if (trace_count < 8) {
        trace_types[trace_count] = event_type;
        trace_offsets[trace_count] = offset;
    }
    ++trace_count;
    trace_digest = fnv(fnv(trace_digest, event_type), offset);
}

struct playback_result
{
    unsigned result;
    long ticks;
    unsigned position;
    unsigned long damaged;
    unsigned long assertions;
};

static struct playback_result playback(const byte *stream, size_t length, long ticks)
{
    struct animation_playback_controller animation_state;
    struct recorded_unit_control control;
    memset(&animation_state, 0, sizeof(animation_state));
    memset(&control, 0, sizeof(control));
    const byte *cursor = stream;
    unsigned long damaged = wii_abi_playback_damaged, assertions = wii_abi_engine_assertions;
    trace_base = stream;
    trace_count = 0;
    struct playback_result result;
    result.result = recorded_animation_apply_event_stream(&animation_state, &control, &ticks, &cursor,
                                                          stream + length);
    result.ticks = ticks;
    result.position = (unsigned)(cursor - stream);
    result.damaged = wii_abi_playback_damaged - damaged;
    result.assertions = wii_abi_engine_assertions - assertions;
    return result;
}

static void playback_checks(struct wii_abi_report *report)
{
    /* An Xbox-authored stream: animation state (kind 1, delta 1), aiming speed
       (kind 2, byte delta 5), control flags (kind 3, word delta 0x1234 LE),
       then the end event (kind 0). */
    static const byte stream[] = {0x09, 0xaa, 0x0e, 0x05, 0xbb, 0x13, 0x34, 0x12, 0xcc, 0xdd, 0x04};
    trace_digest = FNV_START;
    struct playback_result r = playback(stream, sizeof(stream), 0x2000);
    wii_abi_check_u64(report, ID("playback.authored.result"), WII_ABI_CONTRACT, r.result, 1);
    wii_abi_check_i64(report, ID("playback.authored.ticks_left"), WII_ABI_CONTRACT, r.ticks, 0x2000 - 1 - 5 - 0x1234);
    wii_abi_check_u64(report, ID("playback.authored.position"), WII_ABI_CONTRACT, r.position, 10);
    wii_abi_check_u64(report, ID("playback.authored.events"), WII_ABI_CONTRACT, trace_count, 3);
    wii_abi_check_u64(report, ID("playback.authored.types"), WII_ABI_CONTRACT,
                      trace_types[0] << 16 | trace_types[1] << 8 | trace_types[2], 0x020304);
    wii_abi_check_u64(report, ID("playback.authored.offsets"), WII_ABI_CONTRACT,
                      trace_offsets[0] << 16 | trace_offsets[1] << 8 | trace_offsets[2], 0x010408);
    wii_abi_check_u64(report, ID("playback.authored.damaged_assertions"), WII_ABI_CONTRACT,
                      r.damaged << 8 | r.assertions, 0);

    /* finished exactly: the end event's delta (0) equals the ticks left */
    static const byte finished[] = {0x09, 0xaa, 0x04};
    r = playback(finished, sizeof(finished), 1);
    wii_abi_check_u64(report, ID("playback.finished.result"), WII_ABI_CONTRACT, r.result, 0);
    wii_abi_check_u64(report, ID("playback.finished.damaged"), WII_ABI_CONTRACT, r.damaged, 0);

    /* every truncation of the authored stream before its end event stops as
       damaged, without reading past the end (the run is bounded by length) */
    unsigned truncation_failures = 0;
    for (size_t length = 0; length < sizeof(stream) - 1; ++length) {
        r = playback(stream, length, 0x2000);
        truncation_failures += r.result != 0 || r.damaged != 1 || r.position > length;
    }
    wii_abi_check_u64(report, ID("playback.truncated.failures"), WII_ABI_CONTRACT, truncation_failures, 0);
    /* an event type outside the table stops as damaged */
    static const byte unknown[] = {0x01 | (30 << 2), 0x04};
    r = playback(unknown, sizeof(unknown), 10);
    wii_abi_check_u64(report, ID("playback.unknown_event.result_damaged"), WII_ABI_CONTRACT,
                      r.result << 8 | r.damaged, 1);

    /* Generated streams: random headers, deltas (some outside their asserted
       ranges), data, truncation and tick budgets. Compared host vs PPC. */
    rng_state = 0x2545f491u;
    uint64_t digest = FNV_START;
    unsigned long damaged = 0, assertions = 0, finished_count = 0;
    trace_digest = FNV_START;
    for (unsigned i = 0; i < 4096; ++i) {
        byte buffer[96];
        size_t n = 0;
        unsigned events = rng() % 6;
        for (unsigned e = 0; e < events; ++e) {
            unsigned kind = rng() % 4, type = rng() % 25;
            buffer[n++] = (byte)(kind | type << 2);
            if (kind == 2) buffer[n++] = (byte)(rng() % 4 == 0 ? rng() % 2 : 2 + rng() % 254);
            if (kind == 3) {
                unsigned value = rng() % 4 == 0 ? rng() % 256 : 256 + rng() % 65280;
                buffer[n++] = (byte)(value & 0xff);
                buffer[n++] = (byte)(value >> 8);
            }
            for (unsigned d = rng() % 9; d > 0; --d) buffer[n++] = (byte)rng();
        }
        if (rng() % 2) buffer[n++] = (byte)(rng() % 4 | 1 << 2);
        size_t length = rng() % 4 == 0 ? rng() % (n + 1) : n;
        long ticks = rng() % 8 == 0 ? -(long)(rng() % 100) : (long)(rng() % 0x3000);
        r = playback(buffer, length, ticks);
        digest = fnv(fnv(fnv(fnv(digest, r.result), (uint32_t)r.ticks), r.position), (uint32_t)length);
        damaged += r.damaged;
        assertions += r.assertions;
        finished_count += r.result == 0 && r.damaged == 0;
    }
    wii_abi_observe_u64(report, ID("playback.generated.result_digest"), digest);
    wii_abi_observe_u64(report, ID("playback.generated.trace_digest"), trace_digest);
    wii_abi_observe_u64(report, ID("playback.generated.damaged"), damaged);
    wii_abi_observe_u64(report, ID("playback.generated.assertions"), assertions);
    wii_abi_observe_u64(report, ID("playback.generated.finished"), finished_count);
}

static void message_crypt_checks(struct wii_abi_report *report)
{
    /* Canary-bounded: a misread size field could address up to 0xfff bytes. */
    union { unsigned long align; word words[2112]; } storage;
    uint8_t *bytes = (uint8_t *)storage.words;
    uint8_t original[sizeof(storage.words)];
    const unsigned long key[2] = {0x01234567ul, 0x89abcdeful};
    for (size_t i = 0; i < sizeof(storage.words); ++i) bytes[i] = (uint8_t)(0xa0 ^ (i * 37u));
    build_message_header(&storage.words[0], 20, 3, 0);
    memcpy(original, bytes, sizeof(original));
    message_encrypt(&storage.words[0], key);
    size_t beyond = 0;
    for (size_t i = 20; i < sizeof(original); ++i) beyond += bytes[i] != original[i];
    wii_abi_check_u64(report, ID("message_crypt.encrypt.bytes_changed_beyond_message"), WII_ABI_CONTRACT, beyond, 0);
    wii_abi_check_u64(report, ID("message_crypt.encrypt.header_after"), WII_ABI_CONTRACT,
                      (uint64_t)GET_MESSAGE_SIZE(storage.words[0]) << 8 | GET_MESSAGE_TYPE(storage.words[0]) << 4 |
                      GET_MESSAGE_FLAGS(storage.words[0]), 0x1431);
    /* the payload is wire data: identical ciphertext on every target */
    wii_abi_observe_bytes(report, ID("message_crypt.encrypt.payload_ciphertext"), bytes + 2, 18);
    message_decrypt(&storage.words[0], key);
    wii_abi_check_u64(report, ID("message_crypt.roundtrip_identical"), WII_ABI_CONTRACT,
                      memcmp(original, bytes, sizeof(original)) == 0, 1);

    /* a size smaller than the header itself: left untouched */
    unsigned rejected_changes = 0;
    for (word size = 0; size < 2; ++size) {
        for (size_t i = 0; i < sizeof(storage.words); ++i) bytes[i] = (uint8_t)(0x5c ^ (i * 11u));
        build_message_header(&storage.words[0], size, 3, 0);
        memcpy(original, bytes, sizeof(original));
        message_encrypt(&storage.words[0], key);
        rejected_changes += memcmp(original, bytes, sizeof(original)) != 0;
        build_message_header(&storage.words[0], size, 3, 1);
        memcpy(original, bytes, sizeof(original));
        message_decrypt(&storage.words[0], key);
        rejected_changes += memcmp(original, bytes, sizeof(original)) != 0;
    }
    wii_abi_check_u64(report, ID("message_crypt.undersized_header_untouched_failures"), WII_ABI_CONTRACT,
                      rejected_changes, 0);

    /* the largest message: nothing beyond its 0xfff bytes changes */
    for (size_t i = 0; i < sizeof(storage.words); ++i) bytes[i] = (uint8_t)(0x33 ^ (i * 7u));
    build_message_header(&storage.words[0], MAXIMUM_MESSAGE_SIZE, 1, 0);
    memcpy(original, bytes, sizeof(original));
    message_encrypt(&storage.words[0], key);
    beyond = 0;
    for (size_t i = MAXIMUM_MESSAGE_SIZE; i < sizeof(original); ++i) beyond += bytes[i] != original[i];
    wii_abi_check_u64(report, ID("message_crypt.maximum.bytes_changed_beyond_message"), WII_ABI_CONTRACT, beyond, 0);
    message_decrypt(&storage.words[0], key);
    wii_abi_check_u64(report, ID("message_crypt.maximum.roundtrip_identical"), WII_ABI_CONTRACT,
                      memcmp(original, bytes, sizeof(original)) == 0, 1);

    /* generated messages and keys: ciphertext digest compared host vs PPC */
    rng_state = 0x9e3779b9u;
    uint64_t digest = FNV_START;
    unsigned roundtrip_failures = 0;
    for (unsigned i = 0; i < 512; ++i) {
        word size = (word)(2 + rng() % 600);
        const unsigned long message_key[2] = {rng(), rng()};
        for (size_t b = 0; b < (size_t)size + 8; ++b) bytes[b] = (uint8_t)rng();
        build_message_header(&storage.words[0], size, (byte)(1 + rng() % 3), 0);
        memcpy(original, bytes, (size_t)size + 8);
        message_encrypt(&storage.words[0], message_key);
        digest = fnv(digest, GET_MESSAGE_SIZE(storage.words[0]) << 4 | GET_MESSAGE_TYPE(storage.words[0]) << 2 |
                             GET_MESSAGE_FLAGS(storage.words[0]));
        for (size_t b = 2; b < (size_t)size + 8; ++b) digest = fnv(digest, bytes[b]);
        message_decrypt(&storage.words[0], message_key);
        roundtrip_failures += memcmp(original, bytes, (size_t)size + 8) != 0;
    }
    wii_abi_check_u64(report, ID("message_crypt.generated.roundtrip_failures"), WII_ABI_CONTRACT, roundtrip_failures, 0);
    wii_abi_observe_u64(report, ID("message_crypt.generated.ciphertext_digest"), digest);

    /* TEA reference vector: key 0, plaintext 0 -> 41ea3a0a 94baa940 */
    const unsigned long zero[2] = {0, 0};
    const long zero_key[4] = {0, 0, 0, 0};
    unsigned long cipher[2], plain[2];
    tea_encipher(zero, cipher, zero_key);
    wii_abi_check_u64(report, ID("tea.vector.word0"), WII_ABI_CONTRACT, cipher[0], 0x41ea3a0aul);
    wii_abi_check_u64(report, ID("tea.vector.word1"), WII_ABI_CONTRACT, cipher[1], 0x94baa940ul);
    tea_decipher(cipher, plain, zero_key);
    wii_abi_check_u64(report, ID("tea.vector.decipher"), WII_ABI_CONTRACT, (uint64_t)plain[0] << 32 | plain[1], 0);
    uint8_t reversible[7] = {1, 2, 3, 4, 5, 6, 7};
    const uint8_t crypt_key[3] = {0x10, 0x20, 0x30};
    reversible_crypt(reversible, 7, crypt_key, 3);
    const uint8_t reversible_reference[7] = {0xee, 0xdd, 0xcc, 0xcb, 0xda, 0xe9, 0xe8};
    wii_abi_check_bytes(report, ID("reversible_crypt.vector"), WII_ABI_CONTRACT, reversible, reversible_reference, 7);
}

static void interlocked_checks(struct wii_abi_report *report)
{
    /* port/linux/src/xbox_kernel.c bodies: Win32 return contracts */
    LONG cells[4] = {41, 0, 7, 100};
    wii_abi_check_i64(report, ID("interlocked.increment.return"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedIncrement(&cells[0]), 42);
    wii_abi_check_i64(report, ID("interlocked.increment.value"), WII_ABI_CONTRACT, cells[0], 42);
    wii_abi_check_i64(report, ID("interlocked.decrement.return"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedDecrement(&cells[1]), -1);
    wii_abi_check_i64(report, ID("interlocked.exchange.return_old"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedExchange(&cells[2], -5), 7);
    wii_abi_check_i64(report, ID("interlocked.exchange.value"), WII_ABI_CONTRACT, cells[2], -5);
    wii_abi_check_i64(report, ID("interlocked.exchange_add.return_old"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedExchangeAdd(&cells[3], -250), 100);
    wii_abi_check_i64(report, ID("interlocked.exchange_add.value"), WII_ABI_CONTRACT, cells[3], -150);
    wii_abi_check_i64(report, ID("interlocked.compare_exchange.hit_return"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedCompareExchange(&cells[3], 9, -150), -150);
    wii_abi_check_i64(report, ID("interlocked.compare_exchange.hit_value"), WII_ABI_CONTRACT, cells[3], 9);
    wii_abi_check_i64(report, ID("interlocked.compare_exchange.miss_return"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedCompareExchange(&cells[3], 77, 10), 9);
    wii_abi_check_i64(report, ID("interlocked.compare_exchange.miss_value"), WII_ABI_CONTRACT, cells[3], 9);
    LONG wrap = (LONG)volatile_long_max;
    wii_abi_check_i64(report, ID("interlocked.increment.wraps"), WII_ABI_CONTRACT,
                      halo_linux_InterlockedIncrement(&wrap), -2147483647L - 1);
    wii_abi_check_u64(report, ID("interlocked.long_bytes"), WII_ABI_CONTRACT, sizeof(LONG), 4);
}

static void integer_checks(struct wii_abi_report *report)
{
    wii_abi_check_i64(report, ID("integer.negative_right_shift"), WII_ABI_ASSUMPTION, volatile_negative >> 2, -5);
    long wrapped = volatile_long_max + 1; /* defined under -fwrapv, as every port builds the game */
    wii_abi_check_i64(report, ID("integer.fwrapv_long_overflow"), WII_ABI_CONTRACT, wrapped, -2147483647L - 1);
    wii_abi_check_u64(report, ID("integer.long_bytes"), WII_ABI_ASSUMPTION, sizeof(long), 4);
    unsigned long seed = 0x12345678ul;
    unsigned short first = seed_random(&seed);
    wii_abi_check_u64(report, ID("random.seed_random.first"), WII_ABI_CONTRACT, first, 0x7543);
    wii_abi_check_u64(report, ID("random.seed_random.seed"), WII_ABI_CONTRACT, seed, 0x75432777ul);
}

static void ftol_checks(struct wii_abi_report *report)
{
    /* cseries.h fast_ftol: x87 FISTP under the default control word rounds
       half to even. Only in-range values are executed (out-of-range casts are
       undefined in C; see the evidence record). */
    static const struct { uint32_t bits; long reference; } cases[] = {
        {0x3f000000u, 0}, {0x3fc00000u, 2}, {0x40200000u, 2}, {0x40600000u, 4},
        {0xbf000000u, 0}, {0xbfc00000u, -2}, {0xc0200000u, -2}, {0x3effffffu, 0},
        {0x4affffffu, 8388608}, {0xcaffffffu, -8388608}, {0x4e6e6b28u, 1000000000},
        {0x4effffffu, 2147483520}, {0xcf000000u, -2147483647L - 1}, {0x80000000u, 0},
        {0x00800000u, 0}, {0xc0300000u, -3}, {0x40b00000u, 6}};
    char id[96];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        snprintf(id, sizeof(id), ID("ftol.%08lx"), (unsigned long)cases[i].bits);
        wii_abi_check_i64(report, id, WII_ABI_CONTRACT, fast_ftol(wii_abi_f32_from_bits(cases[i].bits)),
                          cases[i].reference);
    }
}

void wii_abi_engine_probe(struct wii_abi_report *report)
{
    unsigned long assertions = wii_abi_engine_assertions;
    char_checks(report);
    wchar_checks(report);
    layout_checks(report);
    unsigned long before_playback = wii_abi_engine_assertions;
    animation_header_checks(report);
    playback_checks(report);
    /* the generated streams' out-of-range deltas are expected assertions */
    unsigned long playback_assertions = wii_abi_engine_assertions - before_playback;
    message_crypt_checks(report);
    interlocked_checks(report);
    integer_checks(report);
    ftol_checks(report);
    wii_abi_check_u64(report, ID("engine_assertions_outside_generated_playback"), WII_ABI_CONTRACT,
                      wii_abi_engine_assertions - assertions - playback_assertions, 0);
}
