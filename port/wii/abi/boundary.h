#ifndef WII_ABI_BOUNDARY_H
#define WII_ABI_BOUNDARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Serialized addresses stay integers; these helpers never manufacture pointers. */
bool wii_xbox_span(uint32_t base, uint32_t length, uint32_t address,
                   uint32_t bytes, uint32_t *offset);
bool wii_array_span(uint32_t length, uint32_t offset, uint32_t count,
                    uint32_t element_bytes);
bool wii_read_le32(const uint8_t *data, size_t length, size_t offset, uint32_t *value);
float wii_float_from_bits(uint32_t bits);
uint32_t wii_float_bits(float value);
/* Caller supplies upper bits explicitly; no historical HS payload policy implied. */
uint32_t wii_cell_replace_low8(uint32_t payload, uint8_t value);
uint32_t wii_cell_replace_low16(uint32_t payload, uint16_t value);

#endif
