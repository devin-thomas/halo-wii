#include "bitfield_boundary.h"

enum wii_bitfield_status wii_decode_animation_event_header(const uint8_t *stream, size_t available,
                                                           struct wii_animation_event_header *out)
{
    if (stream == NULL || out == NULL) return WII_BITFIELD_NULL;
    if (available < 1) return WII_BITFIELD_TRUNCATED;
    struct wii_animation_event_header header = {0};
    header.time_delta_kind = (uint8_t)(stream[0] & 3u);
    header.event_type = (uint8_t)(stream[0] >> 2);
    switch (header.time_delta_kind) {
    case 0:
    case 1:
        header.time_delta = header.time_delta_kind;
        header.header_size = 1;
        break;
    case 2:
        if (available < 2) return WII_BITFIELD_TRUNCATED;
        header.time_delta = stream[1];
        if (header.time_delta <= 1) return WII_BITFIELD_RANGE;
        header.header_size = 2;
        break;
    default:
        if (available < 3) return WII_BITFIELD_TRUNCATED;
        header.time_delta = (uint16_t)(stream[1] | (stream[2] << 8));
        if (header.time_delta <= 0xff) return WII_BITFIELD_RANGE;
        header.header_size = 3;
        break;
    }
    *out = header;
    return WII_BITFIELD_OK;
}

enum wii_bitfield_status wii_decode_message_header(const uint8_t *wire, size_t available,
                                                   struct wii_message_header *out)
{
    if (wire == NULL || out == NULL) return WII_BITFIELD_NULL;
    if (available < 2) return WII_BITFIELD_TRUNCATED;
    unsigned value = (unsigned)wire[0] | ((unsigned)wire[1] << 8);
    out->flags = (uint8_t)(value & 3u);
    out->type = (uint8_t)((value >> 2) & 3u);
    out->size = (uint16_t)(value >> 4);
    return WII_BITFIELD_OK;
}

enum wii_bitfield_status wii_encode_message_header(const struct wii_message_header *header,
                                                   uint8_t *wire, size_t capacity)
{
    if (header == NULL || wire == NULL) return WII_BITFIELD_NULL;
    if (capacity < 2) return WII_BITFIELD_TRUNCATED;
    if (header->flags > 3 || header->type > 3 || header->size > 0xfff) return WII_BITFIELD_RANGE;
    unsigned value = (unsigned)header->flags | ((unsigned)header->type << 2) | ((unsigned)header->size << 4);
    wire[0] = (uint8_t)(value & 0xffu);
    wire[1] = (uint8_t)(value >> 8);
    return WII_BITFIELD_OK;
}

enum wii_bitfield_status wii_pack_bsp_switch_state(int triggered_switch, unsigned ticks, uint8_t *state)
{
    if (state == NULL) return WII_BITFIELD_NULL;
    if (triggered_switch < -8 || triggered_switch > 7 || ticks > 15) return WII_BITFIELD_RANGE;
    *state = (uint8_t)(((unsigned)triggered_switch & 15u) | (ticks << 4));
    return WII_BITFIELD_OK;
}

enum wii_bitfield_status wii_unpack_bsp_switch_state(const uint8_t *state, int *triggered_switch,
                                                     unsigned *ticks)
{
    if (state == NULL || triggered_switch == NULL || ticks == NULL) return WII_BITFIELD_NULL;
    unsigned low = *state & 15u;
    *triggered_switch = low >= 8 ? (int)low - 16 : (int)low;
    *ticks = *state >> 4;
    return WII_BITFIELD_OK;
}
