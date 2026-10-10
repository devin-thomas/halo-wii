/* HRA1 recorded-animation loader (HWI-008C).
 *
 * Layout (tools/wii/recorded_animation.py pack_canonical): a 32-byte
 * big-endian header; each unit-control field as a 32-bit word (signed values
 * sign-extended, floats as IEEE-754 bits); six 16-bit animation-state values
 * sign-extended (current codec only); one 16-byte record per event (type u8,
 * delta kind u8 with 0xFF for the v1 codec, delta u16, three payload words);
 * then any tail bytes verbatim. hra_encode_xbox() rebuilds the original Xbox
 * little-endian event stream, whose SHA-256 must start with the header's
 * source tag. */
#ifndef HALO_WII_HRA_ANIMATION_H
#define HALO_WII_HRA_ANIMATION_H

#include "content_common.h"

#define HRA_HEADER_BYTES 32u
#define HRA_MAX_EVENTS (1u << 18)
#define HRA_MAX_STREAM_BYTES (1u << 20)

struct hra_record {
    uint32_t animation_version, unit_control_version, length_in_ticks, field_count, event_count;
    uint32_t source_bytes, total_ticks, tail_bytes, source_tag;
    uint32_t state_count;                 /* 6 for the current codec, 0 for v1 */
    int current_codec;
    const unsigned char *unit_control;    /* field_count big-endian words */
    const unsigned char *state;           /* state_count big-endian words */
    const unsigned char *events;          /* event_count 16-byte records */
    const unsigned char *tail;            /* tail_bytes */
};

/* Validates the header, version tables, every event (type, kind, delta,
 * payload widths, unused words zero), the end event, the delta sum and the
 * total length. Pointers in *out refer into data. */
enum content_error hra_parse(const unsigned char *data, uint32_t bytes, struct hra_record *out);

/* Writes the Xbox little-endian stream to out (capacity bytes). Fails with
 * CONTENT_CAPACITY when it does not fit and CONTENT_IDENTITY when the length
 * differs from the header's source_bytes. */
enum content_error hra_encode_xbox(const struct hra_record *record, unsigned char *out, uint32_t capacity,
                                   uint32_t *written);

/* hra_encode_xbox into a scratch buffer, then the source-tag check. The
 * stream's SHA-256 is written to digest. */
enum content_error hra_verify_source(const struct hra_record *record, unsigned char *scratch, uint32_t capacity,
                                     unsigned char digest[32]);

#endif
