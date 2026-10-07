#include "boundary.h"
#include <string.h>

_Static_assert(sizeof(float) == 4, "Boundary requires IEEE binary32 storage");
_Static_assert(sizeof(uint32_t) == 4, "Boundary requires uint32_t");

bool wii_xbox_span(uint32_t base, uint32_t length, uint32_t address,
                   uint32_t bytes, uint32_t *offset)
{
    if (offset == NULL || address < base || length > UINT32_MAX - base) return false;
    uint32_t delta = address - base;
    if (delta > length || bytes > length - delta) return false;
    *offset = delta;
    return true;
}

bool wii_array_span(uint32_t length, uint32_t offset, uint32_t count,
                    uint32_t element_bytes)
{
    if (element_bytes == 0 || offset > length) return false;
    return count <= (length - offset) / element_bytes;
}

bool wii_read_le32(const uint8_t *data, size_t length, size_t offset, uint32_t *value)
{
    if (data == NULL || value == NULL || offset > length || length - offset < 4) return false;
    const uint8_t *p = data + offset;
    *value = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

float wii_float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t wii_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint32_t wii_cell_replace_low8(uint32_t payload, uint8_t value)
{
    return (payload & UINT32_C(0xffffff00)) | (uint32_t)value;
}

uint32_t wii_cell_replace_low16(uint32_t payload, uint16_t value)
{
    return (payload & UINT32_C(0xffff0000)) | (uint32_t)value;
}
