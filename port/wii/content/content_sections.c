/* Sectioned big-endian containers and explicit record codecs (HWI-008E). See
 * content_sections.h. */
#include "content_sections.h"

#include <string.h>

static uint32_t padding(uint32_t n) { return (0u - n) & 31u; }

static int zero(const unsigned char *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] != 0)
            return 0;
    return 1;
}

enum content_error content_sections_parse(const unsigned char *data, uint32_t bytes, const char magic[4],
                                          const uint32_t *record_bytes, uint32_t section_count,
                                          struct content_section *out)
{
    if (data == NULL || magic == NULL || record_bytes == NULL || out == NULL || section_count == 0 ||
        section_count > CONTENT_SECTIONS_MAX)
        return CONTENT_ARGUMENT;
    if (bytes < CONTENT_SECTIONS_HEADER_BYTES)
        return CONTENT_TRUNCATED;
    if (memcmp(data, magic, 4) != 0)
        return CONTENT_MAGIC;
    if (content_be16(data + 4) != 1 || content_be16(data + 6) != CONTENT_SECTIONS_HEADER_BYTES)
        return CONTENT_VERSION;
    if (content_be16(data + 10) != 0 || !zero(data + 16, 16))
        return CONTENT_RESERVED;
    if (content_be32(data + 12) != bytes)
        return CONTENT_LENGTH;
    if (content_be16(data + 8) != section_count)
        return CONTENT_SECTION;
    uint32_t head = CONTENT_SECTIONS_HEADER_BYTES + CONTENT_SECTIONS_ENTRY_BYTES * section_count;
    if (head + padding(head) > bytes)
        return CONTENT_TRUNCATED;
    if (!zero(data + head, padding(head)))
        return CONTENT_RESERVED;
    uint32_t cursor = head + padding(head);
    for (uint32_t i = 0; i < section_count; ++i) {
        const unsigned char *entry = data + CONTENT_SECTIONS_HEADER_BYTES + CONTENT_SECTIONS_ENTRY_BYTES * i;
        struct content_section s;
        s.id = content_be16(entry);
        s.record_bytes = content_be16(entry + 2);
        s.count = content_be32(entry + 4);
        s.offset = content_be32(entry + 8);
        s.bytes = content_be32(entry + 12);
        if (s.id != i + 1 || s.record_bytes != record_bytes[i])
            return CONTENT_SECTION;
        if ((uint64_t)s.count * s.record_bytes != s.bytes || s.offset != cursor)
            return CONTENT_SECTION;
        if (s.bytes > bytes - cursor)
            return CONTENT_LENGTH;
        uint32_t end = cursor + s.bytes;
        if (padding(end) > bytes - end)
            return CONTENT_LENGTH;
        if (!zero(data + end, padding(end)))
            return CONTENT_RESERVED;
        s.data = data + s.offset;
        out[i] = s;
        cursor = end + padding(end);
    }
    return cursor == bytes ? CONTENT_OK : CONTENT_LENGTH;
}

static uint32_t code_size(char code)
{
    switch (code) {
    case 'b': case 'B': return 1;
    case 'h': case 'H': return 2;
    case 'i': case 'I': case 'f': return 4;
    default: return 0;
    }
}

uint32_t content_layout_init(struct content_layout *layout, const char *format)
{
    memset(layout, 0, sizeof(*layout));
    uint32_t at = 0;
    for (const char *p = format; *p != '\0'; ++p) {
        uint32_t size = code_size(*p);
        if (size == 0 || at % size != 0 || layout->fields >= CONTENT_LAYOUT_MAX_FIELDS || at > 255)
            return layout->bytes = 0;
        layout->code[layout->fields] = (unsigned char)*p;
        layout->offset[layout->fields] = (unsigned char)at;
        layout->fields++;
        at += size;
    }
    return layout->bytes = at;
}

void content_records_decode(const struct content_layout *layout, const unsigned char *source, uint32_t count,
                            void *destination, uint32_t stride)
{
    unsigned char *out = destination;
    for (uint32_t r = 0; r < count; ++r, source += layout->bytes, out += stride) {
        for (uint32_t f = 0; f < layout->fields; ++f) {
            const unsigned char *in = source + layout->offset[f];
            unsigned char *to = out + layout->offset[f];
            switch (code_size((char)layout->code[f])) {
            case 1:
                *to = *in;
                break;
            case 2: {
                uint16_t v = (uint16_t)content_be16(in);
                memcpy(to, &v, 2);
                break;
            }
            default: {
                uint32_t v = content_be32(in);
                memcpy(to, &v, 4);
                break;
            }
            }
        }
    }
}

uint32_t content_field_word(const struct content_layout *layout, const void *record, uint32_t field)
{
    const unsigned char *at = (const unsigned char *)record + layout->offset[field];
    switch (layout->code[field]) {
    case 'b': return (uint32_t)(int32_t)(int8_t)*at;
    case 'B': return *at;
    case 'h': { int16_t v; memcpy(&v, at, 2); return (uint32_t)(int32_t)v; }
    case 'H': { uint16_t v; memcpy(&v, at, 2); return v; }
    default: { uint32_t v; memcpy(&v, at, 4); return v; }
    }
}

void content_records_digest(struct content_sha256 *sha, const struct content_layout *layout, const void *source,
                            uint32_t count, uint32_t stride)
{
    const unsigned char *in = source;
    unsigned char buffer[1024];
    uint32_t used = 0;
    for (uint32_t r = 0; r < count; ++r, in += stride) {
        if (used + layout->bytes > sizeof(buffer)) {
            content_sha256_update(sha, buffer, used);
            used = 0;
        }
        for (uint32_t f = 0; f < layout->fields; ++f) {
            uint32_t word = content_field_word(layout, in, f);
            unsigned char *to = buffer + used + layout->offset[f];
            switch (code_size((char)layout->code[f])) {
            case 1: *to = (unsigned char)word; break;
            case 2: content_put_le16(to, word); break;
            default: content_put_le32(to, word); break;
            }
        }
        used += layout->bytes;
    }
    if (used > 0)
        content_sha256_update(sha, buffer, used);
}

void content_unpack_vector(uint32_t packed, float out[3])
{
    /* Sign-extend each field explicitly; (2c + 1) is exact in float, so one
     * rounding remains, the same as the engine's expression. */
    int32_t i = (int32_t)(packed & 0x7FFu) - (int32_t)((packed & 0x400u) << 1);
    int32_t j = (int32_t)((packed >> 11) & 0x7FFu) - (int32_t)(((packed >> 11) & 0x400u) << 1);
    int32_t k = (int32_t)((packed >> 22) & 0x3FFu) - (int32_t)(((packed >> 22) & 0x200u) << 1);
    out[0] = (float)(2 * i + 1) * (1.0f / 2047.0f);
    out[1] = (float)(2 * j + 1) * (1.0f / 2047.0f);
    out[2] = (float)(2 * k + 1) * (1.0f / 1023.0f);
}

enum content_error content_arena_reserve(struct content_arena *arena, uint64_t total)
{
    arena->base = NULL;
    arena->bytes = arena->used = 0;
    if (total > CONTENT_DECODED_MAX_BYTES)
        return CONTENT_COUNT;
    if (total == 0)
        return CONTENT_OK;
    arena->base = content_alloc_aligned((uint32_t)total);
    if (arena->base == NULL)
        return CONTENT_MEMORY;
    arena->bytes = (uint32_t)total;
    return CONTENT_OK;
}

void *content_arena_take(struct content_arena *arena, uint32_t bytes)
{
    if (bytes == 0)
        return NULL;
    void *at = arena->base + arena->used;
    arena->used += content_align32(bytes);
    return at;
}

void content_arena_release(struct content_arena *arena)
{
    if (arena == NULL)
        return;
    content_free_aligned(arena->base);
    arena->base = NULL;
    arena->bytes = arena->used = 0;
}
