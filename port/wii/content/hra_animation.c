/* HRA1 recorded-animation loader (HWI-008C). See hra_animation.h.
 *
 * Field codes follow recorded_animation_initialize.c and the two playback
 * codecs (tools/wii/recorded_animation.py). */
#include "hra_animation.h"

#include <string.h>

enum code { U8, S8, PAD8, S16, U16, S32, F32 };

static const unsigned char code_bytes[] = {1, 1, 1, 2, 2, 4, 4};

/* Unit-control field tables: version 0 and 1 use the first table, each
 * later version appends one more. */
static const unsigned char unit_control_base[] = {U8, U8, S16, S16, S16, F32, F32, F32, F32, F32, F32, F32, F32,
                                                  F32, F32, F32};
static const unsigned char unit_control_extra[] = {S32, S16, S16};

static int unit_control_field(uint32_t version, uint32_t index, unsigned char *code)
{
    uint32_t tables = version < 1 ? 1 : version;
    if (tables > 4)
        return -1;
    uint32_t count = 16 + (tables - 1);
    if (index >= count)
        return 0;
    *code = index < 16 ? unit_control_base[index] : unit_control_extra[index - 16];
    return 1;
}

static uint32_t unit_control_count(uint32_t version)
{
    uint32_t tables = version < 1 ? 1 : version;
    return tables > 4 ? 0 : 16 + (tables - 1);
}

/* Payload codes after an event header; returns the count or -1 for an
 * unknown type. */
static int payload_codes(int current, uint32_t type, unsigned char codes[3])
{
    if (current) {
        if (type <= 1) return 0;
        if (type <= 3) { codes[0] = U8; return 1; }
        if (type <= 5) { codes[0] = S16; return 1; }
        if (type == 6) { codes[0] = codes[1] = F32; return 2; }
        if (type <= 14) { codes[0] = codes[1] = S8; return 2; }
        if (type <= 22) { codes[0] = codes[1] = S16; return 2; }
        return -1;
    }
    if (type <= 1) return 0;
    if (type <= 3) { codes[0] = U8; codes[1] = PAD8; return 2; }
    if (type == 4) { codes[0] = U16; return 1; }
    if (type == 5) { codes[0] = S16; return 1; }
    if (type == 6) { codes[0] = codes[1] = F32; return 2; }
    if (type <= 8) return 0;
    if (type <= 15) { codes[0] = codes[1] = codes[2] = F32; return 3; }
    if (type <= 22) { codes[0] = codes[1] = F32; return 2; }
    return -1;
}

/* Whether a canonical 32-bit word is a valid value of the code's width. */
static int word_fits(unsigned char code, uint32_t word)
{
    switch (code) {
    case U8: case PAD8: return word <= 0xFFu;
    case S8: return word <= 0x7Fu || word >= 0xFFFFFF80u;
    case S16: return word <= 0x7FFFu || word >= 0xFFFF8000u;
    case U16: return word <= 0xFFFFu;
    default: return 1;
    }
}

enum content_error hra_parse(const unsigned char *data, uint32_t bytes, struct hra_record *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    if (bytes < HRA_HEADER_BYTES)
        return CONTENT_TRUNCATED;
    if (memcmp(data, "HRA1", 4) != 0)
        return CONTENT_MAGIC;
    struct hra_record r;
    memset(&r, 0, sizeof(r));
    if (content_be16(data + 4) != 1)
        return CONTENT_VERSION;
    r.animation_version = data[6];
    r.unit_control_version = data[7];
    r.length_in_ticks = content_be16(data + 8);
    r.field_count = content_be16(data + 10);
    r.event_count = content_be32(data + 12);
    r.source_bytes = content_be32(data + 16);
    r.total_ticks = content_be32(data + 20);
    r.tail_bytes = content_be32(data + 24);
    r.source_tag = content_be32(data + 28);
    if (r.animation_version < 1 || r.animation_version > 4 || unit_control_count(r.unit_control_version) == 0)
        return CONTENT_VERSION;
    if (r.field_count != unit_control_count(r.unit_control_version))
        return CONTENT_COUNT;
    if (r.event_count == 0 || r.event_count > HRA_MAX_EVENTS || r.source_bytes == 0 ||
        r.source_bytes > HRA_MAX_STREAM_BYTES || r.tail_bytes > HRA_MAX_STREAM_BYTES)
        return CONTENT_COUNT;
    r.current_codec = r.animation_version == 4;
    r.state_count = r.current_codec ? 6u : 0u;
    uint64_t expected = (uint64_t)HRA_HEADER_BYTES + 4u * ((uint64_t)r.field_count + r.state_count) +
                        16u * (uint64_t)r.event_count + r.tail_bytes;
    if (expected != bytes)
        return bytes < expected ? CONTENT_TRUNCATED : CONTENT_LENGTH;
    const unsigned char *p = data + HRA_HEADER_BYTES;
    r.unit_control = p;
    for (uint32_t i = 0; i < r.field_count; ++i) {
        unsigned char code = 0;
        unit_control_field(r.unit_control_version, i, &code);
        if (!word_fits(code, content_be32(p + 4 * i)))
            return CONTENT_VALUE;
    }
    p += 4u * r.field_count;
    r.state = p;
    for (uint32_t i = 0; i < r.state_count; ++i)
        if (!word_fits(S16, content_be32(p + 4 * i)))
            return CONTENT_VALUE;
    p += 4u * r.state_count;
    r.events = p;
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < r.event_count; ++i, p += 16) {
        uint32_t type = p[0], kind = p[1], delta = content_be16(p + 2);
        unsigned char codes[3];
        int count = payload_codes(r.current_codec, type, codes);
        if (count < 0)
            return CONTENT_EVENT;
        if ((type == 1) != (i + 1 == r.event_count))
            return CONTENT_EVENT;   /* exactly one end event, and it is last */
        if (r.current_codec) {
            if ((kind == 0 && delta != 0) || (kind == 1 && delta != 1) || (kind == 2 && (delta < 2 || delta > 255)) ||
                (kind == 3 && delta <= 255) || kind > 3)
                return CONTENT_DELTA;
        } else if (kind != 0xFF) {
            return CONTENT_DELTA;
        }
        for (int w = 0; w < 3; ++w) {
            uint32_t word = content_be32(p + 4 + 4 * w);
            if (w >= count ? word != 0 : !word_fits(codes[w], word))
                return w >= count ? CONTENT_RESERVED : CONTENT_VALUE;
        }
        ticks += delta;
    }
    if (ticks != r.total_ticks)
        return CONTENT_COUNT;
    r.tail = p;
    *out = r;
    return CONTENT_OK;
}

static int put(unsigned char *out, uint32_t capacity, uint32_t *at, unsigned char code, uint32_t value)
{
    uint32_t size = code_bytes[code];
    if (*at > capacity || size > capacity - *at)
        return 0;
    for (uint32_t b = 0; b < size; ++b)
        out[*at + b] = (unsigned char)(value >> (8 * b));
    *at += size;
    return 1;
}

enum content_error hra_encode_xbox(const struct hra_record *r, unsigned char *out, uint32_t capacity,
                                   uint32_t *written)
{
    if (r == NULL || out == NULL || written == NULL)
        return CONTENT_ARGUMENT;
    uint32_t at = 0;
    for (uint32_t i = 0; i < r->field_count; ++i) {
        unsigned char code = 0;
        unit_control_field(r->unit_control_version, i, &code);
        if (!put(out, capacity, &at, code, content_be32(r->unit_control + 4 * i)))
            return CONTENT_CAPACITY;
    }
    for (uint32_t i = 0; i < r->state_count; ++i)
        if (!put(out, capacity, &at, S16, content_be32(r->state + 4 * i)))
            return CONTENT_CAPACITY;
    const unsigned char *p = r->events;
    for (uint32_t i = 0; i < r->event_count; ++i, p += 16) {
        uint32_t type = p[0], kind = p[1], delta = content_be16(p + 2);
        unsigned char codes[3];
        int count = payload_codes(r->current_codec, type, codes);
        if (count < 0)
            return CONTENT_EVENT;
        if (r->current_codec) {
            if (!put(out, capacity, &at, U8, ((type << 2) | kind) & 0xFFu))
                return CONTENT_CAPACITY;
            if ((kind == 2 && !put(out, capacity, &at, U8, delta)) || (kind == 3 && !put(out, capacity, &at, U16, delta)))
                return CONTENT_CAPACITY;
        } else if (!put(out, capacity, &at, S16, type) || !put(out, capacity, &at, U16, delta)) {
            return CONTENT_CAPACITY;
        }
        for (int w = 0; w < count; ++w)
            if (!put(out, capacity, &at, codes[w], content_be32(p + 4 + 4 * w)))
                return CONTENT_CAPACITY;
    }
    if (at > capacity || r->tail_bytes > capacity - at)
        return CONTENT_CAPACITY;
    memcpy(out + at, r->tail, r->tail_bytes);
    at += r->tail_bytes;
    *written = at;
    return at == r->source_bytes ? CONTENT_OK : CONTENT_IDENTITY;
}

enum content_error hra_verify_source(const struct hra_record *record, unsigned char *scratch, uint32_t capacity,
                                     unsigned char digest[32])
{
    uint32_t written = 0;
    enum content_error error = hra_encode_xbox(record, scratch, capacity, &written);
    if (error != CONTENT_OK)
        return error;
    content_sha256(scratch, written, digest);
    return content_be32(digest) == record->source_tag ? CONTENT_OK : CONTENT_IDENTITY;
}
