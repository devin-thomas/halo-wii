/* HUS1 unicode text loader (HWI-008E). See hus_strings.h. */
#include "hus_strings.h"

#include <string.h>

_Static_assert(sizeof(struct hus_string) == 8, "hus_string matches HUS_STRING_FORMAT");

static const uint32_t record_bytes[2] = {8, 2};

enum content_error hus_load(const unsigned char *data, uint32_t bytes, struct hus_strings *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    memset(out, 0, sizeof(*out));
    struct content_layout string, unit;
    if (content_layout_init(&string, HUS_STRING_FORMAT) != sizeof(struct hus_string) ||
        content_layout_init(&unit, "H") != 2)
        return CONTENT_ARGUMENT;
    struct content_section s[2];
    enum content_error error = content_sections_parse(data, bytes, "HUS1", record_bytes, 2, s);
    if (error != CONTENT_OK)
        return error;
    if (s[0].count > HUS_MAX_STRINGS)
        return CONTENT_COUNT;
    struct hus_strings t;
    memset(&t, 0, sizeof(t));
    t.string_count = s[0].count;
    t.unit_count = s[1].count;
    const uint64_t sizes[2] = {s[0].bytes, s[1].bytes};
    error = content_arena_reserve(&t.arena, content_arena_size(sizes, 2));
    if (error != CONTENT_OK)
        return error;
    t.strings = content_arena_take(&t.arena, s[0].bytes);
    t.units = content_arena_take(&t.arena, s[1].bytes);
    content_records_decode(&string, s[0].data, t.string_count, t.strings, sizeof(struct hus_string));
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < t.string_count && error == CONTENT_OK; ++i) {
        if (t.strings[i].unit_count > HUS_MAX_STRING_UNITS)
            error = CONTENT_COUNT;
        else if (t.strings[i].first_unit != cursor)
            error = CONTENT_RANGE;
        cursor += t.strings[i].unit_count;
    }
    if (error == CONTENT_OK && cursor != t.unit_count)
        error = CONTENT_RANGE;
    if (error != CONTENT_OK) {
        content_arena_release(&t.arena);
        return error;
    }
    content_records_decode(&unit, s[1].data, t.unit_count, t.units, 2);
    *out = t;
    return CONTENT_OK;
}

void hus_release(struct hus_strings *strings)
{
    if (strings == NULL)
        return;
    content_arena_release(&strings->arena);
    memset(strings, 0, sizeof(*strings));
}

const uint16_t *hus_string_at(const struct hus_strings *strings, uint32_t index, uint32_t *unit_count)
{
    if (index >= strings->string_count)
        return NULL;
    *unit_count = strings->strings[index].unit_count;
    return strings->units + strings->strings[index].first_unit;
}

void hus_digest(const struct hus_strings *t, unsigned char digest[32])
{
    struct content_layout unit;
    content_layout_init(&unit, "H");
    struct content_sha256 sha;
    content_sha256_init(&sha);
    for (uint32_t i = 0; i < t->string_count; ++i) {
        unsigned char length[4];
        content_put_le32(length, 2u * t->strings[i].unit_count);
        content_sha256_update(&sha, length, 4);
        content_records_digest(&sha, &unit, t->units + t->strings[i].first_unit, t->strings[i].unit_count, 2);
    }
    content_sha256_final(&sha, digest);
}
