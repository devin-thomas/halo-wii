#include "cache_stream_io.h"
#include "cache_address_owned.h"
#include <string.h>

const char *cache_stream_error_name(enum cache_stream_error error)
{
    static const char *const names[] = {
        "ok", "argument", "handle", "capacity", "alias", "short",
        "trailing", "io", "crc", "overflow"
    };
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

static int reject(struct cache_stream_result *result, enum cache_stream_error error)
{
    result->error = error;
    return 0;
}

static int resolve(const struct cache_arena_owner *owner,
                   const struct cache_arena_handle *handle, size_t offset, size_t length,
                   unsigned char **output, struct cache_stream_result *result)
{
    struct cache_arena_result arena = {0};
    if (cache_arena_owner_resolve(owner, handle, offset, length, output, &arena))
        return 1;
    result->arena_error = arena.error;
    return reject(result, arena.error == CACHE_ARENA_SPAN ? CACHE_STREAM_CAPACITY :
                          CACHE_STREAM_HANDLE);
}

static int overlap(const void *left, size_t left_bytes, const void *right, size_t right_bytes)
{
    uintptr_t a = (uintptr_t)left;
    uintptr_t b = (uintptr_t)right;
    return left_bytes && right_bytes && (a <= b ? b - a < left_bytes : a - b < right_bytes);
}

static int control_alias(const void *bytes, size_t size, FILE *input,
                          const struct cache_arena_owner *owner,
                          const struct cache_arena_handle *tag,
                          const struct cache_arena_handle *io0,
                          const struct cache_arena_handle *io1,
                          struct cache_stream_result *result)
{
    return overlap(bytes, size, input, sizeof(*input)) ||
        overlap(bytes, size, owner, sizeof(*owner)) ||
        overlap(bytes, size, tag, sizeof(*tag)) ||
        overlap(bytes, size, io0, sizeof(*io0)) ||
        overlap(bytes, size, io1, sizeof(*io1)) ||
        overlap(bytes, size, result, sizeof(*result));
}

int cache_stream_read_at(FILE *input, const struct cache_arena_owner *owner,
                         const struct cache_arena_handle *tag,
                         const struct cache_arena_handle *io0,
                         const struct cache_arena_handle *io1,
                         size_t destination_offset, size_t expected_bytes,
                         uint32_t expected_crc32, struct cache_stream_result *result)
{
    if (!result || result->error != CACHE_STREAM_OK)
        return 0;
    memset(result, 0, sizeof(*result));
    if (!input || !owner || !tag || !io0 || !io1)
        return reject(result, CACHE_STREAM_ARGUMENT);
    if (expected_bytes > SIZE_MAX - destination_offset) {
        result->arena_error = CACHE_ARENA_OVERFLOW;
        return reject(result, CACHE_STREAM_OVERFLOW);
    }

    unsigned char *destination = NULL;
    unsigned char *buffers[2] = {NULL, NULL};
    if (!resolve(owner, tag, destination_offset, expected_bytes, &destination, result) ||
        !resolve(owner, io0, 0, CACHE_STREAM_IO_BYTES, &buffers[0], result) ||
        !resolve(owner, io1, 0, CACHE_STREAM_IO_BYTES, &buffers[1], result))
        return 0;
    if (owner->plan.slots[io0->slot_index].size != CACHE_STREAM_IO_BYTES ||
        owner->plan.slots[io1->slot_index].size != CACHE_STREAM_IO_BYTES)
        return reject(result, CACHE_STREAM_CAPACITY);
    if (tag->slot_index == io0->slot_index || tag->slot_index == io1->slot_index ||
        io0->slot_index == io1->slot_index)
        return reject(result, CACHE_STREAM_ALIAS);
    if (overlap(destination, expected_bytes, buffers[0], CACHE_STREAM_IO_BYTES) ||
        overlap(destination, expected_bytes, buffers[1], CACHE_STREAM_IO_BYTES) ||
        overlap(buffers[0], CACHE_STREAM_IO_BYTES, buffers[1], CACHE_STREAM_IO_BYTES) ||
        control_alias(destination, expected_bytes, input, owner, tag, io0, io1, result) ||
        control_alias(buffers[0], CACHE_STREAM_IO_BYTES, input, owner, tag, io0, io1, result) ||
        control_alias(buffers[1], CACHE_STREAM_IO_BYTES, input, owner, tag, io0, io1, result))
        return reject(result, CACHE_STREAM_ALIAS);
    if (ferror(input))
        return reject(result, CACHE_STREAM_IO);

    while (result->bytes < expected_bytes)
    {
        size_t remaining = expected_bytes - result->bytes;
        size_t request = remaining < CACHE_STREAM_IO_BYTES ? remaining : CACHE_STREAM_IO_BYTES;
        unsigned char *buffer = buffers[result->chunks & 1u];
        size_t received = fread(buffer, 1, request, input);
        ++result->read_calls;
        if (received)
        {
            memcpy(destination + result->bytes, buffer, received);
            result->bytes += received;
            ++result->chunks;
        }
        if (ferror(input))
            return reject(result, CACHE_STREAM_IO);
        if (received != request)
            return reject(result, CACHE_STREAM_SHORT);
    }

    result->actual_crc32 = cache_probe_crc32(destination, expected_bytes);
    size_t trailing = fread(buffers[result->chunks & 1u], 1, 1, input);
    ++result->read_calls;
    if (ferror(input))
        return reject(result, CACHE_STREAM_IO);
    if (trailing)
        return reject(result, CACHE_STREAM_TRAILING);
    if (!feof(input))
        return reject(result, CACHE_STREAM_IO);
    if (result->actual_crc32 != expected_crc32)
        return reject(result, CACHE_STREAM_CRC);
    return 1;
}

int cache_stream_read(FILE *input, const struct cache_arena_owner *owner,
                      const struct cache_arena_handle *tag,
                      const struct cache_arena_handle *io0,
                      const struct cache_arena_handle *io1,
                      size_t expected_bytes, uint32_t expected_crc32,
                      struct cache_stream_result *result)
{
    return cache_stream_read_at(input, owner, tag, io0, io1, 0, expected_bytes, expected_crc32, result);
}
