/* Shared pieces of the Wii content loaders: error names, owned file loading
 * and SHA-256 (HWI-008C). */
#include "content_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(GEKKO)
#include <malloc.h>
#define content_aligned_alloc(alignment, bytes) memalign((alignment), (bytes))
#define content_aligned_free(pointer) free(pointer)
#elif defined(_WIN32)
#include <malloc.h>
#define content_aligned_alloc(alignment, bytes) _aligned_malloc((bytes), (alignment))
#define content_aligned_free(pointer) _aligned_free(pointer)
#else
#define content_aligned_alloc(alignment, bytes) aligned_alloc((alignment), (bytes))
#define content_aligned_free(pointer) free(pointer)
#endif

static const char *const error_names[CONTENT_ERROR_COUNT] = {
    "ok", "argument", "io", "size", "memory", "truncated", "magic", "version", "enum", "dimensions",
    "count", "length", "reserved", "event", "delta", "value", "identity", "capacity", "section", "range",
};

const char *content_error_name(enum content_error error)
{
    return (unsigned)error < CONTENT_ERROR_COUNT ? error_names[error] : "unknown";
}

enum content_error content_error_from_name(const char *name)
{
    for (unsigned i = 0; name != NULL && i < CONTENT_ERROR_COUNT; ++i)
        if (strcmp(name, error_names[i]) == 0)
            return (enum content_error)i;
    return CONTENT_ERROR_COUNT;
}

void *content_alloc_aligned(uint32_t bytes)
{
    if (bytes == 0 || bytes > 0x7FFFFFE0u)
        return NULL;
    return content_aligned_alloc(32, (bytes + 31u) & ~31u);
}

void content_free_aligned(void *pointer)
{
    if (pointer != NULL)
        content_aligned_free(pointer);
}

void content_blob_release(struct content_blob *blob)
{
    if (blob == NULL)
        return;
    content_aligned_free(blob->data);
    blob->data = NULL;
    blob->bytes = blob->capacity = 0;
}

enum content_error content_load_file(const char *path, uint32_t max_bytes, struct content_blob *out)
{
    if (path == NULL || out == NULL || max_bytes == 0 || max_bytes > 0x7FFFFFE0u)
        return CONTENT_ARGUMENT;
    out->data = NULL;
    out->bytes = out->capacity = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return CONTENT_IO;
    enum content_error result = CONTENT_OK;
    long size = -1;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
        result = CONTENT_IO;
    else if (size == 0 || (unsigned long)size > max_bytes)
        result = CONTENT_SIZE;
    unsigned char *data = NULL;
    uint32_t capacity = 0;
    if (result == CONTENT_OK) {
        capacity = ((uint32_t)size + 31u) & ~31u;
        data = content_aligned_alloc(32, capacity);
        if (data == NULL)
            result = CONTENT_MEMORY;
    }
    if (result == CONTENT_OK) {
        memset(data + size, 0, capacity - (uint32_t)size);
        size_t got = fread(data, 1, (size_t)size, file);
        unsigned char extra;
        /* A short read, or more bytes than the size taken above, rejects. */
        if (got != (size_t)size || fread(&extra, 1, 1, file) != 0)
            result = CONTENT_IO;
    }
    if (fclose(file) != 0 && result == CONTENT_OK)
        result = CONTENT_IO;
    if (result != CONTENT_OK) {
        if (data != NULL)
            content_aligned_free(data);
        return result;
    }
    out->data = data;
    out->bytes = (uint32_t)size;
    out->capacity = capacity;
    return CONTENT_OK;
}

/* ---- SHA-256 ---------------------------------------------------------------- */

static const uint32_t sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t value, unsigned bits) { return (value >> bits) | (value << (32u - bits)); }

static void sha_block(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = content_be32(block + 4 * i);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void content_sha256_init(struct content_sha256 *context)
{
    static const uint32_t initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(context->state, initial, sizeof(initial));
    context->length = 0;
    context->used = 0;
}

void content_sha256_update(struct content_sha256 *context, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    context->length += (uint64_t)bytes;
    while (bytes > 0) {
        size_t take = 64u - context->used;
        if (take > bytes)
            take = bytes;
        memcpy(context->buffer + context->used, p, take);
        context->used += (uint32_t)take;
        p += take;
        bytes -= take;
        if (context->used == 64) {
            sha_block(context->state, context->buffer);
            context->used = 0;
        }
    }
}

void content_sha256_final(struct content_sha256 *context, unsigned char digest[32])
{
    uint64_t bits = context->length * 8u;
    unsigned char pad = 0x80;
    content_sha256_update(context, &pad, 1);
    pad = 0;
    while (context->used != 56)
        content_sha256_update(context, &pad, 1);
    unsigned char length[8];
    for (unsigned i = 0; i < 8; ++i)
        length[i] = (unsigned char)(bits >> (56 - 8 * i));
    content_sha256_update(context, length, 8);
    for (unsigned i = 0; i < 8; ++i) {
        digest[4 * i] = (unsigned char)(context->state[i] >> 24);
        digest[4 * i + 1] = (unsigned char)(context->state[i] >> 16);
        digest[4 * i + 2] = (unsigned char)(context->state[i] >> 8);
        digest[4 * i + 3] = (unsigned char)context->state[i];
    }
}

void content_sha256(const void *data, size_t bytes, unsigned char digest[32])
{
    struct content_sha256 context;
    content_sha256_init(&context);
    content_sha256_update(&context, data, bytes);
    content_sha256_final(&context, digest);
}

void content_hex(const unsigned char digest[32], char text[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        text[2 * i] = digits[digest[i] >> 4];
        text[2 * i + 1] = digits[digest[i] & 15];
    }
    text[64] = '\0';
}
