/* Sectioned big-endian containers and explicit record codecs (HWI-008E).
 *
 * HWM1, HWL1, HWC1, HMA1, HWF1 and HUS1 share one layout
 * (tools/wii/be_records.py):
 *
 *   header, 32 bytes  magic (4 ASCII), version u16 = 1, header bytes u16 = 32,
 *                     section count u16, flags u16 = 0, total file bytes u32,
 *                     16 reserved bytes = 0
 *   section table     one 16-byte entry per section in ascending id order:
 *                     id u16, record bytes u16, record count u32, offset u32,
 *                     bytes u32 (= count * record bytes)
 *   sections          each at the next 32-byte boundary after the table or the
 *                     previous section; padding is zero and the file ends on a
 *                     32-byte boundary
 *
 * Records are described by a format string, one code per field: b s8, B u8,
 * h s16, H u16, i s32, I u32, f f32 (IEEE-754 bits). Every field is naturally
 * aligned inside its record, so the record has the same layout as a C struct
 * of those types; each loader checks its structs against its formats with
 * static assertions. Decoding reads every field big-endian with shifts and
 * stores it as a native value; the canonical serialization writes every field
 * little-endian (the Xbox order). Nothing depends on host byte order, struct
 * packing or compiler bit-fields (ADR-018). */
#ifndef HALO_WII_CONTENT_SECTIONS_H
#define HALO_WII_CONTENT_SECTIONS_H

#include "content_common.h"

#define CONTENT_SECTIONS_HEADER_BYTES 32u
#define CONTENT_SECTIONS_ENTRY_BYTES 16u
#define CONTENT_SECTIONS_MAX 9u
#define CONTENT_LAYOUT_MAX_FIELDS 64u
/* Upper bound on one decoded asset; larger inputs reject with CONTENT_COUNT. */
#define CONTENT_DECODED_MAX_BYTES (48u * 1024u * 1024u)

struct content_section {
    uint32_t id, record_bytes, count, offset, bytes;
    const unsigned char *data;
};

/* Validates the header and section table of a container whose sections are
 * ids 1..section_count with the given record sizes, the padding and the total
 * length. out[i] describes section id i + 1. Errors: TRUNCATED, MAGIC,
 * VERSION, RESERVED (flags, reserved bytes, padding), LENGTH (total, a section
 * past the end, bytes after the last section), SECTION (count, id, record
 * size, size, offset). */
enum content_error content_sections_parse(const unsigned char *data, uint32_t bytes, const char magic[4],
                                          const uint32_t *record_bytes, uint32_t section_count,
                                          struct content_section *out);

/* A record format compiled to field offsets. */
struct content_layout {
    uint32_t fields, bytes;
    unsigned char code[CONTENT_LAYOUT_MAX_FIELDS];
    unsigned char offset[CONTENT_LAYOUT_MAX_FIELDS];
};

/* 0 for a format with an unknown code, a misaligned field or too many
 * fields; the record size otherwise. */
uint32_t content_layout_init(struct content_layout *layout, const char *format);

/* count big-endian records into native records stride bytes apart. */
void content_records_decode(const struct content_layout *layout, const unsigned char *source, uint32_t count,
                            void *destination, uint32_t stride);

/* Appends the little-endian serialization of count native records stride
 * bytes apart to a SHA-256. */
void content_records_digest(struct content_sha256 *sha, const struct content_layout *layout, const void *source,
                            uint32_t count, uint32_t stride);

/* Native field access by format code: the value as a 32-bit word, signed
 * codes sign-extended. */
uint32_t content_field_word(const struct content_layout *layout, const void *record, uint32_t field);

/* The engine's uncompress_int32_to_real_vector3d
 * (source/rasterizer/rasterizer_geometry.c): 11, 11 and 10-bit signed
 * components c, each (2c + 1) * (1 / (2^bits - 1)) in single precision. The
 * result has the same bits as the engine's. */
void content_unpack_vector(uint32_t packed, float out[3]);

/* One owned 32-byte aligned allocation carved into 32-byte aligned arrays. */
struct content_arena {
    unsigned char *base;
    uint32_t bytes, used;
};

static inline uint32_t content_align32(uint32_t bytes) { return (bytes + 31u) & ~31u; }

/* Sum of the 32-byte aligned sizes of count arrays (bytes each), or a value
 * above CONTENT_DECODED_MAX_BYTES when any or the sum exceeds it. */
static inline uint64_t content_arena_size(const uint64_t *bytes, uint32_t count)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes[i] > CONTENT_DECODED_MAX_BYTES)
            return (uint64_t)CONTENT_DECODED_MAX_BYTES + 1u;
        total += (bytes[i] + 31u) & ~(uint64_t)31u;
    }
    return total;
}

/* A 16-bit big-endian field as a signed value. */
static inline int16_t content_s16(const unsigned char *p)
{
    uint32_t v = content_be16(p);
    return (int16_t)((int32_t)v - (int32_t)((v & 0x8000u) << 1));
}

/* Allocates total bytes (0 allowed: no allocation). */
enum content_error content_arena_reserve(struct content_arena *arena, uint64_t total);
/* The next bytes of the arena, 32-byte aligned; NULL for 0 bytes. The caller
 * reserved enough. */
void *content_arena_take(struct content_arena *arena, uint32_t bytes);
void content_arena_release(struct content_arena *arena);

#endif
