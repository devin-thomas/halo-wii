/* HUS1 unicode text loader (HWI-008E).
 *
 * Container (tools/wii/content_text.py): sections 1 STRINGS (8: first code
 * unit, code units) and 2 UNITS (UTF-16 code units, big-endian). One unicode
 * string list tag, or one HUD message text tag (a single entry), per file.
 *
 * Decoded form: the string table and the code units as native u16 in one
 * owned 32-byte aligned arena. Each string keeps its exact code units,
 * terminating zero included when the source had one; the engine's 16-bit
 * text type on Wii is a 16-bit code unit (ADR-018), so a string can be used in
 * place. */
#ifndef HALO_WII_HUS_STRINGS_H
#define HALO_WII_HUS_STRINGS_H

#include "content_sections.h"

#define HUS_STRING_FORMAT "II"
#define HUS_MAX_STRINGS 32767u
#define HUS_MAX_STRING_UNITS 65536u

struct hus_string {
    uint32_t first_unit, unit_count;
};

struct hus_strings {
    uint32_t string_count, unit_count;
    struct hus_string *strings;
    uint16_t *units;
    struct content_arena arena;
};

/* Validates the container, the string count, string lengths and string
 * ranges that tile the code units in order, and decodes into owned memory. */
enum content_error hus_load(const unsigned char *data, uint32_t bytes, struct hus_strings *out);
void hus_release(struct hus_strings *strings);

/* String index's code units (NULL for an index out of range). */
const uint16_t *hus_string_at(const struct hus_strings *strings, uint32_t index, uint32_t *unit_count);

/* SHA-256 of the decoded text: per string, its byte length as u32 and its
 * code units, little-endian (for a unicode string list, the converter's source
 * hash). */
void hus_digest(const struct hus_strings *strings, unsigned char digest[32]);

#endif
