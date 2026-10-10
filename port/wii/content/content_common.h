/* Shared pieces of the Wii content loaders (HWI-008C, HWI-008E).
 *
 * Every converted container is big-endian. Fields are assembled from bytes
 * with explicit shifts, so nothing here depends on host byte order, struct
 * layout or compiler bit-fields (ADR-018). The same sources compile for the
 * Wii (PowerPC) and for a little-endian host. */
#ifndef HALO_WII_CONTENT_COMMON_H
#define HALO_WII_CONTENT_COMMON_H

#include <stddef.h>
#include <stdint.h>

enum content_error {
    CONTENT_OK = 0,
    CONTENT_ARGUMENT,   /* NULL or impossible argument */
    CONTENT_IO,         /* open, stat or read failed */
    CONTENT_SIZE,       /* file size outside the caller's bound */
    CONTENT_MEMORY,     /* allocation failed */
    CONTENT_TRUNCATED,  /* fewer bytes than the header needs */
    CONTENT_MAGIC,      /* wrong signature */
    CONTENT_VERSION,    /* unsupported container or source version */
    CONTENT_ENUM,       /* enumerated field outside its set */
    CONTENT_DIMENSIONS, /* texture dimensions, faces or levels inconsistent */
    CONTENT_COUNT,      /* a count outside its bound or inconsistent */
    CONTENT_LENGTH,     /* declared length differs from the bytes present */
    CONTENT_RESERVED,   /* a reserved or padding field is not zero */
    CONTENT_EVENT,      /* unknown event type or misplaced end event */
    CONTENT_DELTA,      /* event delta inconsistent with its kind */
    CONTENT_VALUE,      /* a field value outside its declared width */
    CONTENT_IDENTITY,   /* rebuilt source differs from the recorded identity */
    CONTENT_CAPACITY,   /* caller's output buffer too small */
    CONTENT_SECTION,    /* sectioned container: section id, record size, order, offset or size wrong */
    CONTENT_RANGE,      /* a record names an element outside its array, or ranges do not tile it */
    CONTENT_ERROR_COUNT
};

const char *content_error_name(enum content_error error);
/* CONTENT_ERROR_COUNT when the name is unknown. */
enum content_error content_error_from_name(const char *name);

static inline uint32_t content_be16(const unsigned char *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static inline uint32_t content_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void content_put_le16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static inline void content_put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static inline void content_put_be16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFFu);
    p[1] = (unsigned char)(v & 0xFFu);
}

/* A file loaded whole into memory this module owns: 32-byte aligned (GX and
 * DMA), capacity rounded up to 32 bytes, zero past the end of the file. */
struct content_blob {
    unsigned char *data;
    uint32_t bytes;
    uint32_t capacity;
};

/* 32-byte aligned allocation for owned content memory (NULL on failure or
 * for 0 bytes); release with content_free_aligned. */
void *content_alloc_aligned(uint32_t bytes);
void content_free_aligned(void *pointer);

/* Reads path whole. Rejects a size of 0 or above max_bytes, a short read, or
 * a file that grows while it is read. On failure *out is left empty. */
enum content_error content_load_file(const char *path, uint32_t max_bytes, struct content_blob *out);
void content_blob_release(struct content_blob *blob);

/* SHA-256 (FIPS 180-4), incremental. */
struct content_sha256 {
    uint32_t state[8];
    uint64_t length;
    unsigned char buffer[64];
    uint32_t used;
};

void content_sha256_init(struct content_sha256 *context);
void content_sha256_update(struct content_sha256 *context, const void *data, size_t bytes);
void content_sha256_final(struct content_sha256 *context, unsigned char digest[32]);
void content_sha256(const void *data, size_t bytes, unsigned char digest[32]);
/* 64 lowercase hex digits plus NUL. */
void content_hex(const unsigned char digest[32], char text[65]);

#endif
