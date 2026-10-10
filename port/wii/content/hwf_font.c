/* HWF1 font loader (HWI-008E). See hwf_font.h. */
#include "hwf_font.h"

#include <string.h>

_Static_assert(sizeof(struct hwf_font_record) == 24, "hwf_font_record matches HWF_FONT_FORMAT");
_Static_assert(sizeof(struct hwf_table) == 8, "hwf_table matches HWF_TABLE_FORMAT");
_Static_assert(sizeof(struct hwf_character) == 20, "hwf_character matches HWF_CHARACTER_FORMAT");

static const uint32_t record_bytes[5] = {24, 8, 2, 20, 1};

static enum content_error check(const struct hwf_font *f, uint32_t tables, uint32_t characters, uint32_t pixels)
{
    if (f->font.table_count != tables || f->font.character_count != characters || f->font.pixel_bytes != pixels)
        return CONTENT_COUNT;
    if (tables > HWF_MAX_TABLES || characters > HWF_MAX_CHARACTERS)
        return CONTENT_COUNT;
    uint64_t cursor = 0;
    for (uint32_t t = 0; t < tables; ++t) {
        if (f->tables[t].first_index != cursor)
            return CONTENT_RANGE;
        if (f->tables[t].index_count > HWF_MAX_TABLE_ENTRIES)
            return CONTENT_COUNT;
        cursor += f->tables[t].index_count;
    }
    if (cursor != f->index_count)
        return CONTENT_RANGE;
    for (uint32_t i = 0; i < f->index_count; ++i)
        if (f->indices[i] < -1 || f->indices[i] >= (int32_t)characters)
            return CONTENT_RANGE;
    for (uint32_t c = 0; c < characters; ++c) {
        const struct hwf_character *g = &f->characters[c];
        int64_t w = g->bitmap_width > 0 ? g->bitmap_width : 0, h = g->bitmap_height > 0 ? g->bitmap_height : 0;
        if (g->pixel_offset < 0 || (int64_t)g->pixel_offset + w * h > (int64_t)pixels)
            return CONTENT_RANGE;
    }
    return CONTENT_OK;
}

enum content_error hwf_load(const unsigned char *data, uint32_t bytes, struct hwf_font *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    struct content_layout font, table, index, character;
    if (content_layout_init(&font, HWF_FONT_FORMAT) != sizeof(struct hwf_font_record) ||
        content_layout_init(&table, HWF_TABLE_FORMAT) != sizeof(struct hwf_table) ||
        content_layout_init(&index, "h") != 2 ||
        content_layout_init(&character, HWF_CHARACTER_FORMAT) != sizeof(struct hwf_character))
        return CONTENT_ARGUMENT;
    struct content_section s[5];
    enum content_error error = content_sections_parse(data, bytes, "HWF1", record_bytes, 5, s);
    if (error != CONTENT_OK)
        return error;
    if (s[0].count != 1)
        return CONTENT_COUNT;
    struct hwf_font f;
    memset(&f, 0, sizeof(f));
    content_records_decode(&font, s[0].data, 1, &f.font, sizeof(f.font));
    f.index_count = s[2].count;
    const uint64_t sizes[4] = {s[1].bytes, s[2].bytes, s[3].bytes, s[4].bytes};
    error = content_arena_reserve(&f.arena, content_arena_size(sizes, 4));
    if (error != CONTENT_OK)
        return error;
    f.tables = content_arena_take(&f.arena, s[1].bytes);
    f.indices = content_arena_take(&f.arena, s[2].bytes);
    f.characters = content_arena_take(&f.arena, s[3].bytes);
    f.pixels = content_arena_take(&f.arena, s[4].bytes);
    content_records_decode(&table, s[1].data, s[1].count, f.tables, sizeof(struct hwf_table));
    content_records_decode(&index, s[2].data, s[2].count, f.indices, 2);
    content_records_decode(&character, s[3].data, s[3].count, f.characters, sizeof(struct hwf_character));
    if (s[4].bytes != 0)
        memcpy(f.pixels, s[4].data, s[4].bytes);
    error = check(&f, s[1].count, s[3].count, s[4].count);
    if (error != CONTENT_OK) {
        content_arena_release(&f.arena);
        return error;
    }
    *out = f;
    return CONTENT_OK;
}

void hwf_release(struct hwf_font *font)
{
    if (font == NULL)
        return;
    content_arena_release(&font->arena);
    memset(font, 0, sizeof(*font));
}

const struct hwf_character *hwf_character_for(const struct hwf_font *font, uint32_t code_unit)
{
    uint32_t t = (code_unit & 0xFFFFu) >> 8;
    if (t >= font->font.table_count || font->tables[t].index_count != 256)
        return NULL;
    int16_t index = font->indices[font->tables[t].first_index + (code_unit & 0xFFu)];
    return index >= 0 && (uint32_t)index < font->font.character_count ? &font->characters[index] : NULL;
}

void hwf_digest(const struct hwf_font *f, unsigned char digest[32])
{
    struct content_layout font, table, index, character;
    content_layout_init(&font, HWF_FONT_FORMAT);
    content_layout_init(&table, HWF_TABLE_FORMAT);
    content_layout_init(&index, "h");
    content_layout_init(&character, HWF_CHARACTER_FORMAT);
    struct content_sha256 sha;
    content_sha256_init(&sha);
    content_records_digest(&sha, &font, &f->font, 1, sizeof(f->font));
    content_records_digest(&sha, &table, f->tables, f->font.table_count, sizeof(struct hwf_table));
    content_records_digest(&sha, &index, f->indices, f->index_count, 2);
    content_records_digest(&sha, &character, f->characters, f->font.character_count, sizeof(struct hwf_character));
    content_sha256_update(&sha, f->pixels, f->font.pixel_bytes);
    content_sha256_final(&sha, digest);
}
