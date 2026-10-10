#ifndef WII_ABI_BITFIELD_BOUNDARY_H
#define WII_ABI_BITFIELD_BOUNDARY_H
/* Explicit decoders for engine bit-field records whose bytes are authored or
   exchanged in the Xbox (little-endian, LSB-first bit-field) representation.
   Candidates for HWI-006: they read bytes arithmetically and never overlay a
   C bit-field on input, so the result does not depend on the compiler's
   bit-field allocation or byte order. Outputs are written only on success. */

#include <stddef.h>
#include <stdint.h>

enum wii_bitfield_status
{
    WII_BITFIELD_OK = 0,
    WII_BITFIELD_NULL,
    WII_BITFIELD_TRUNCATED,
    WII_BITFIELD_RANGE
};

/* source/cutscene/recorded_animation_playback.c struct animation_event_header:
   byte 0 bits 0-1 time-delta kind, bits 2-7 event type; kind 2 adds one delta
   byte (2..255), kind 3 adds an LE16 delta (256..65535). The ranges are the
   source's own assertions. */
struct wii_animation_event_header
{
    uint8_t time_delta_kind;
    uint8_t event_type;
    uint16_t time_delta;
    uint8_t header_size;
};
enum wii_bitfield_status wii_decode_animation_event_header(const uint8_t *stream, size_t available,
                                                           struct wii_animation_event_header *out);

/* source/bungie_net/common/message_encryption.c union message_header_value and
   message_header.h GET_MESSAGE_*: LE16 word, flags bits 0-1, type bits 2-3,
   size bits 4-15. */
struct wii_message_header
{
    uint8_t flags;
    uint8_t type;
    uint16_t size;
};
enum wii_bitfield_status wii_decode_message_header(const uint8_t *wire, size_t available,
                                                   struct wii_message_header *out);
enum wii_bitfield_status wii_encode_message_header(const struct wii_message_header *header,
                                                   uint8_t *wire, size_t capacity);

/* source/game/players.h bsp_switch_state byte (Xbox layout): low nibble the
   signed 4-bit local_player_triggered_switch (-8..7, NONE = -1), high nibble
   the unsigned bsp_check_recursive_switch_ticks (0..15). */
enum wii_bitfield_status wii_pack_bsp_switch_state(int triggered_switch, unsigned ticks, uint8_t *state);
enum wii_bitfield_status wii_unpack_bsp_switch_state(const uint8_t *state, int *triggered_switch,
                                                     unsigned *ticks);

#endif
