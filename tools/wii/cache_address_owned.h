#ifndef WII_CACHE_ADDRESS_OWNED_H
#define WII_CACHE_ADDRESS_OWNED_H
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
uint32_t cache_probe_crc32(const void *, size_t);
int cache_address_owned(FILE *, const unsigned char *, size_t, uint32_t, uint32_t, uint32_t);
#endif
