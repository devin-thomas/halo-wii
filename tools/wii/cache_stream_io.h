#ifndef WII_CACHE_STREAM_IO_H
#define WII_CACHE_STREAM_IO_H
#include "cache_arena_plan.h"
#include <stdio.h>

#define CACHE_STREAM_IO_BYTES 65536u

enum cache_stream_error {
    CACHE_STREAM_OK,
    CACHE_STREAM_ARGUMENT,
    CACHE_STREAM_HANDLE,
    CACHE_STREAM_CAPACITY,
    CACHE_STREAM_ALIAS,
    CACHE_STREAM_SHORT,
    CACHE_STREAM_TRAILING,
    CACHE_STREAM_IO,
    CACHE_STREAM_CRC
};

struct cache_stream_result {
    enum cache_stream_error error;
    enum cache_arena_error arena_error;
    size_t read_calls;
    size_t chunks;
    size_t bytes;
    uint32_t actual_crc32;
};

const char *cache_stream_error_name(enum cache_stream_error);

/* Read the remaining FILE contents into a live tag slot, with two distinct
 * exactly-64-KiB IO slots alternating for data reads. The tag slot must contain
 * expected_bytes. Truthful immutable owner/plan/handle controls and separate
 * result storage are required; the caller retains file and storage ownership.
 * Reading starts at the current file position; no rewind, close or release is
 * performed. The final CRC is of raw loaded bytes, not an Xbox map checksum.
 * Initialize result to zero. Errors are sticky until the caller resets it;
 * an OK invocation resets counters. read_calls counts fread invocations,
 * including the final one-byte
 * EOF check; chunks counts nonempty data reads, bytes counts tag bytes copied.
 * Short/IO rejection may leave partial tag/IO writes. Trailing/CRC rejection
 * may leave a full tag write. No failure releases or changes the owner.
 * actual_crc32 is populated after all expected bytes have been copied.
 * This is synchronous IO; alternating buffers do not imply asynchronous IO. */
int cache_stream_read(FILE *input, const struct cache_arena_owner *owner,
                      const struct cache_arena_handle *tag,
                      const struct cache_arena_handle *io0,
                      const struct cache_arena_handle *io1,
                      size_t expected_bytes, uint32_t expected_crc32,
                      struct cache_stream_result *result);
#endif
