/* HWF1 font loader (HWI-008E).
 *
 * Container (tools/wii/content_text.py): sections 1 FONT (24), 2 TABLES (8:
 * first index, index count), 3 INDICES (s16 character index, -1 none), 4
 * CHARACTERS (20), 5 PIXELS (8-bit glyph coverage, verbatim). One font tag per
 * file.
 *
 * Decoded form, in one owned 32-byte aligned arena: native records, the
 * character tables as (first, count) into one s16 index array, and the glyph
 * coverage bytes verbatim (row-major per glyph, max(width, 0) x max(height, 0)
 * bytes from the glyph's pixel offset). Rendering decision: glyphs stay 8-bit
 * linear coverage in memory; the text renderer copies the glyphs it draws into
 * a GX I8 (or I4) cache texture, as the Xbox engine fills its font cache, so
 * the font itself is never tiled. */
#ifndef HALO_WII_HWF_FONT_H
#define HALO_WII_HWF_FONT_H

#include "content_sections.h"

#define HWF_FONT_FORMAT "IhhhhIII"
#define HWF_TABLE_FORMAT "II"
#define HWF_CHARACTER_FORMAT "Hhhhhhhhi"
#define HWF_MAX_TABLES 256u
#define HWF_MAX_TABLE_ENTRIES 256u
#define HWF_MAX_CHARACTERS 32767u

struct hwf_font_record {
    uint32_t flags;
    int16_t ascending_height, descending_height, leading_height, leading_width;
    uint32_t table_count, character_count, pixel_bytes;
};

struct hwf_table {
    uint32_t first_index, index_count;
};

struct hwf_character {
    uint16_t character;
    int16_t character_width, bitmap_width, bitmap_height, bitmap_origin_x, bitmap_origin_y;
    int16_t hardware_character_index, pad;
    int32_t pixel_offset;
};

struct hwf_font {
    struct hwf_font_record font;
    struct hwf_table *tables;
    int16_t *indices;
    uint32_t index_count;
    struct hwf_character *characters;
    unsigned char *pixels;
    struct content_arena arena;
};

/* Validates the container, the font record against the sections, tables
 * that tile the index array (at most 256 of at most 256 entries), every index
 * (-1 or a character), and every glyph's pixels inside the pixel data, and
 * decodes into owned memory. */
enum content_error hwf_load(const unsigned char *data, uint32_t bytes, struct hwf_font *out);
void hwf_release(struct hwf_font *font);

/* The character for a UTF-16 code unit by the engine's rule
 * (font_get_character_by_ascii_code): table code >> 8 of exactly 256 entries,
 * entry code & 0xFF; NULL when there is none. */
const struct hwf_character *hwf_character_for(const struct hwf_font *font, uint32_t code_unit);

/* SHA-256 of the decoded font, every field little-endian in order: font
 * record, tables, indices, characters, pixels. */
void hwf_digest(const struct hwf_font *font, unsigned char digest[32]);

#endif
