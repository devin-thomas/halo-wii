#ifndef WII_CACHE_SHA256_FIXTURE_H
#define WII_CACHE_SHA256_FIXTURE_H

#include <stddef.h>
#include <stdio.h>

/* Implemented by the runner's unchanged SHA section from p2p_crypto.c. */
void p2p_sha256(const void *data, int size, unsigned char *digest);

/* Hash at most 22MiB and write 64 lowercase hex digits plus NUL. Return one
 * on success, zero on invalid length, NULL argument or overlapping storage.
 * NULL input is allowed only for zero bytes. Rejection preserves output.
 * Accessible input/output ranges are truthful, immutable, disjoint and owned
 * by one caller. The SHA is a byte identity, not an Xbox cache checksum. */
int cache_sha256_hex(const void *data, size_t bytes, char output[65]);

/* Authored vectors only. Any temporary input is freed before return. */
int cache_sha256_fixture(FILE *report);

#endif
