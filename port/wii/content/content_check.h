/* Case list and per-file checks shared by the Wii loader self-test (main.c)
 * and the host check (host_check.c) (HWI-008C, HWI-008E).
 *
 * A case names one converted file, the result its loader must give and, for
 * a valid file, the SHA-256 of its decoded contents as the host tool
 * (tools/wii/content_loader_cases.py) computed them:
 *   texture          RGBA8 of every texel of every image (face, level, slice; rows)
 *   animation        the rebuilt Xbox event stream
 *   sound            the decoded samples as big-endian PCM16, interleaved
 *   model, lightmap, collision, model_animation, font, strings
 *                    the loader's canonical digest of its decoded form
 *                    (hwm_digest, hwl_digest, hwc_digest, hma_digest,
 *                    hwf_digest, hus_digest)
 * For the decoded kinds the file is released before the digest is taken, so
 * the digest proves the decoded form stands on its own. */
#ifndef HALO_WII_CONTENT_CHECK_H
#define HALO_WII_CONTENT_CHECK_H

#include "content_common.h"
#include "hwt_texture.h"

#define CONTENT_CHECK_MAX_CASES 256u
#define CONTENT_CHECK_MAX_FILE_BYTES (8u * 1024u * 1024u)

enum content_kind {
    CONTENT_KIND_TEXTURE,
    CONTENT_KIND_ANIMATION,
    CONTENT_KIND_SOUND,
    CONTENT_KIND_MODEL,
    CONTENT_KIND_LIGHTMAP,
    CONTENT_KIND_COLLISION,
    CONTENT_KIND_MODEL_ANIMATION,
    CONTENT_KIND_FONT,
    CONTENT_KIND_STRINGS,
    CONTENT_KIND_COUNT
};

struct content_case {
    char id[24];
    enum content_kind kind;
    enum content_error expect;
    char path[256];
    uint32_t bytes;
    char file_sha[65];
    char decoded_sha[65];
};

struct content_readback {
    unsigned draws, peeks, mismatches, interpolated;
    int max_err[4];
    uint32_t crc;
};

struct content_outcome {
    enum content_error error;
    int file_ok, decoded_match, pass;
    char decoded[65];
    uint32_t units;          /* images, events, frames; vertices, elements, animations, characters, strings */
    uint32_t items;          /* decoded kinds: indices, surfaces, BSPs, compressed animations, pixel bytes, code units */
    uint32_t decoded_bytes;  /* decoded kinds: the owned decoded memory (arena) */
    uint64_t expanded_bytes; /* model animations: bytes if compressed animations were expanded at load */
    int32_t heap_peak;       /* heap in use above the case's start with the file and decoded form held (-1 unknown) */
    int32_t heap_resident;   /* the same after the file is released: the decoded form alone (-1 unknown) */
    uint32_t gx_images;      /* images drawn and read back */
    uint32_t mip_levels;     /* levels drawn through one 2D mip chain */
    int control;             /* wrong-format control on image 0: -1 none, 0 matched, 1 detected */
    struct content_readback readback;
    uint64_t load_ticks, decode_ticks, digest_ticks;
};

/* Called for every valid texture after its CPU decode (GX upload and read
 * back on the Wii; NULL on the host). */
typedef void (*content_texture_hook)(const struct content_blob *blob, const struct hwt_header *header,
                                     struct content_outcome *outcome);
typedef uint64_t (*content_clock)(void);
/* Heap bytes in use (the Wii's mallinfo); NULL where unavailable. */
typedef int32_t (*content_heap)(void);

const char *content_kind_name(enum content_kind kind);

/* Parses "case <id> <kind> <expect> <path> <bytes> <file sha256> <decoded
 * sha256 or ->" lines ('#' comments). path must start with prefix. Returns
 * the number of cases, or -1 for a malformed list. */
int content_read_cases(const char *list_path, const char *prefix, struct content_case *cases, unsigned capacity);

/* Loads, hashes, parses and decodes one case. scratch must hold at least
 * HRA_MAX_STREAM_BYTES + 64 KiB. heap may be NULL. */
void content_run_case(const struct content_case *c, unsigned char *scratch, uint32_t scratch_bytes,
                      content_texture_hook hook, content_clock clock, content_heap heap,
                      struct content_outcome *outcome);

uint32_t content_crc32(uint32_t crc, const unsigned char *data, uint32_t bytes);

#endif
