/* Checks over actual engine declarations and bodies (abi_engine_types.h and
   abi_engine.c are generated from the source tree). This unit is compiled
   twice: WII_ABI_PROBE_VARIANT=baseline with the engine flags as every port
   uses them, and WII_ABI_PROBE_VARIANT=signed_char adding -fsigned-char
   (WII_ABI_PROBE_CANDIDATE). References are the semantics the source was
   written against: MSVC for x86, little-endian, LSB-first bit-fields, signed
   plain char. */
#include "abi_engine_types.h"
#include "engine_probe.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define WII_ABI_CAT2(a, b) a##b
#define WII_ABI_CAT(a, b) WII_ABI_CAT2(a, b)
#define WII_ABI_STR2(a) #a
#define WII_ABI_STR(a) WII_ABI_STR2(a)
#define PROBE_ENTRY WII_ABI_CAT(wii_abi_engine_probe_, WII_ABI_PROBE_VARIANT)
#define ID(name) WII_ABI_STR(WII_ABI_PROBE_VARIANT) "." name

#ifdef WII_ABI_PROBE_CANDIDATE
#define CHAR_CLASS WII_ABI_CANDIDATE
#else
#define CHAR_CLASS WII_ABI_ASSUMPTION
#endif

/* Values pass through volatile objects so that the compiler cannot fold the
   implementation-defined conversions away or reject constant ones. */
static volatile int volatile_none = _local_player_triggered_switch_none;
static volatile int volatile_two = 2;
static volatile int volatile_twelve = 12;

static void char_checks(struct wii_abi_report *report)
{
    wii_abi_check_i64(report, ID("char.plain_min"), CHAR_CLASS, CHAR_MIN, -128);

    /* recorded_animation_playback.c: delta bytes read from the playback stream */
    const uint8_t deltas[2] = {0xf6, 0x05};
    struct vector_char_difference_data difference;
    memcpy(&difference, deltas, sizeof(difference));
    wii_abi_check_i64(report, ID("char.vector_char_difference.delta_yaw"), CHAR_CLASS, difference.delta_yaw, -10);
    wii_abi_check_i64(report, ID("char.vector_char_difference.delta_pitch"), CHAR_CLASS, difference.delta_pitch, 5);

    /* players.h: char local_player_triggered_switch : 4 holds NONE; players.c
       tests it against _local_player_triggered_switch_none every tick */
    struct wii_abi_players_bsp_switch_excerpt state;
    memset(&state, 0, sizeof(state));
    state.local_player_triggered_switch = volatile_none;
    int readback = state.local_player_triggered_switch;
    wii_abi_check_i64(report, ID("char.bitfield.triggered_switch_none_value"), CHAR_CLASS, readback, -1);
    wii_abi_check_i64(report, ID("char.bitfield.triggered_switch_pending_after_none"), CHAR_CLASS,
                      readback != volatile_none, 0);
}

static void bitfield_checks(struct wii_abi_report *report)
{
    /* Bit allocation is not changed by -fsigned-char: engine_assumption in both variants. */
    struct wii_abi_players_bsp_switch_excerpt state;
    memset(&state, 0, sizeof(state));
    wii_abi_check_u64(report, ID("bitfield.players_switch.size"), WII_ABI_ASSUMPTION, sizeof(state), 1);
    state.local_player_triggered_switch = volatile_two;
    state.bsp_check_recursive_switch_ticks = (byte)volatile_twelve;
    wii_abi_check_u64(report, ID("bitfield.players_switch.state_byte"), WII_ABI_ASSUMPTION, state.bsp_switch_state, 0xc2);
    state.bsp_switch_state = 0xcf;
    wii_abi_check_i64(report, ID("bitfield.players_switch.ticks_from_byte"), WII_ABI_ASSUMPTION,
                      state.bsp_check_recursive_switch_ticks, 12);

    struct animation_event_header header;
    wii_abi_check_u64(report, ID("bitfield.animation_header.size"), WII_ABI_ASSUMPTION, sizeof(header), 1);
    const uint8_t header_0d = 0x0d, header_fe = 0xfe;
    memcpy(&header, &header_0d, 1);
    wii_abi_check_u64(report, ID("bitfield.animation_header.0d.time_delta"), WII_ABI_ASSUMPTION, header.time_delta, 1);
    wii_abi_check_u64(report, ID("bitfield.animation_header.0d.event_type"), WII_ABI_ASSUMPTION, header.event_type, 3);
    memcpy(&header, &header_fe, 1);
    wii_abi_check_u64(report, ID("bitfield.animation_header.fe.time_delta"), WII_ABI_ASSUMPTION, header.time_delta, 2);
    wii_abi_check_u64(report, ID("bitfield.animation_header.fe.event_type"), WII_ABI_ASSUMPTION, header.event_type, 63);
    memset(&header, 0, sizeof(header));
    header.time_delta = 2;
    header.event_type = 5;
    uint8_t encoded;
    memcpy(&encoded, &header, 1);
    wii_abi_check_u64(report, ID("bitfield.animation_header.encode_2_5"), WII_ABI_ASSUMPTION, encoded, 0x16);

    /* message_encryption.c reads a native word, then its bit-fields */
    union message_header_value value;
    const uint8_t wire[2] = {0x34, 0x12};
    memcpy(&value.value, wire, sizeof(value.value));
    wii_abi_check_u64(report, ID("bitfield.message_header.wire_le.flags"), WII_ABI_ASSUMPTION, value.fields.flags, 0);
    wii_abi_check_u64(report, ID("bitfield.message_header.wire_le.type"), WII_ABI_ASSUMPTION, value.fields.type, 1);
    wii_abi_check_u64(report, ID("bitfield.message_header.wire_le.size"), WII_ABI_ASSUMPTION,
                      value.fields.message_size, 0x123);
    /* within one target: message_header.c builds with shifts/masks, message_encryption.c reads bit-fields */
    word built = 0;
    build_message_header(&built, 0x20, 3, 2);
    value.value = built;
    wii_abi_check_u64(report, ID("bitfield.message_header.native_build.size"), WII_ABI_ASSUMPTION,
                      value.fields.message_size, 0x20);
    wii_abi_check_u64(report, ID("bitfield.message_header.native_build.type"), WII_ABI_ASSUMPTION, value.fields.type, 3);
    wii_abi_check_u64(report, ID("bitfield.message_header.native_build.flags"), WII_ABI_ASSUMPTION, value.fields.flags, 2);
    wii_abi_check_u64(report, ID("bitfield.message_header.native_build.macros_agree"), WII_ABI_CONTRACT,
                      (uint64_t)GET_MESSAGE_SIZE(built) << 8 | (uint64_t)GET_MESSAGE_TYPE(built) << 4 |
                      GET_MESSAGE_FLAGS(built), 0x2032);

    /* hud_nav_points.c: runtime-only datum (not persisted) */
    struct hud_nav_point_datum nav;
    wii_abi_check_u64(report, ID("bitfield.hud_nav_point.size"), WII_ABI_ASSUMPTION, sizeof(nav), 12);
    wii_abi_check_u64(report, ID("bitfield.hud_nav_point.z_offset_offset"), WII_ABI_ASSUMPTION,
                      offsetof(struct hud_nav_point_datum, z_offset), 4);
    memset(&nav, 0, sizeof(nav));
    nav.type = (short)(volatile_none - 2);
    nav.screen_type = (short)(volatile_two + 3);
    wii_abi_check_i64(report, ID("bitfield.hud_nav_point.type_signed_readback"), WII_ABI_CONTRACT, nav.type, -3);
    wii_abi_check_i64(report, ID("bitfield.hud_nav_point.screen_type_readback"), WII_ABI_CONTRACT, nav.screen_type, 5);
    uint8_t nav_bytes[2];
    memcpy(nav_bytes, (const uint8_t *)&nav + offsetof(struct hud_nav_point_datum, nav_index) + sizeof(short), 2);
    const uint8_t nav_reference[2] = {0x5d, 0x00};
    wii_abi_check_bytes(report, ID("bitfield.hud_nav_point.unit_bytes"), WII_ABI_ASSUMPTION, nav_bytes, nav_reference, 2);
}

#ifndef WII_ABI_PROBE_CANDIDATE
static volatile long volatile_long_max = 2147483647L;
static volatile long volatile_negative = -17;

static void message_crypt_checks(struct wii_abi_report *report)
{
    /* Canary-bounded: a misread size field can address up to 0xfff bytes. */
    union { unsigned long align; word words[2112]; } storage;
    uint8_t *bytes = (uint8_t *)storage.words;
    for (size_t i = 0; i < sizeof(storage.words); ++i) bytes[i] = (uint8_t)(0xa0 ^ (i * 37u));
    uint8_t original[sizeof(storage.words)];
    build_message_header(&storage.words[0], 20, 3, 0);
    memcpy(original, bytes, sizeof(original));
    const unsigned long key[2] = {0x01234567ul, 0x89abcdeful};
    message_encrypt(&storage.words[0], key);
    size_t beyond = 0;
    for (size_t i = 20; i < sizeof(original); ++i) beyond += bytes[i] != original[i];
    wii_abi_check_u64(report, ID("message_crypt.encrypt.bytes_changed_beyond_message"), WII_ABI_ASSUMPTION, beyond, 0);
    wii_abi_check_u64(report, ID("message_crypt.encrypt.flags_after"), WII_ABI_ASSUMPTION,
                      GET_MESSAGE_FLAGS(storage.words[0]), 1);
    wii_abi_observe_bytes(report, ID("message_crypt.encrypt.first_20_bytes_native"), bytes, 20);
    message_decrypt(&storage.words[0], key);
    wii_abi_check_u64(report, ID("message_crypt.roundtrip_identical"), WII_ABI_ASSUMPTION,
                      memcmp(original, bytes, sizeof(original)) == 0, 1);

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
    wii_abi_check_u64(report, ID("integer.wchar_bytes"), WII_ABI_ASSUMPTION, sizeof(wchar_t), 2);
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
        snprintf(id, sizeof(id), "%s.ftol.%08lx", WII_ABI_STR(WII_ABI_PROBE_VARIANT), (unsigned long)cases[i].bits);
        wii_abi_check_i64(report, id, WII_ABI_CONTRACT, fast_ftol(wii_abi_f32_from_bits(cases[i].bits)),
                          cases[i].reference);
    }
}
#endif

void PROBE_ENTRY(struct wii_abi_report *report)
{
    unsigned long assertions = wii_abi_engine_assertions;
    char_checks(report);
    bitfield_checks(report);
#ifndef WII_ABI_PROBE_CANDIDATE
    message_crypt_checks(report);
    interlocked_checks(report);
    integer_checks(report);
    ftol_checks(report);
#endif
    wii_abi_observe_u64(report, ID("engine_assertions_failed"), wii_abi_engine_assertions - assertions);
}
