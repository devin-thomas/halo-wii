/* Case list and per-file checks for the content loaders (HWI-008C,
 * HWI-008E). See content_check.h. */
#include "content_check.h"

#include <stdio.h>
#include <string.h>

#include "hma_graph.h"
#include "hra_animation.h"
#include "hwc_collision.h"
#include "hwf_font.h"
#include "hwl_lightmap.h"
#include "hwm_model.h"
#include "hus_strings.h"
#include "hws_sound.h"

#define ADPCM_CHUNK_BLOCKS 64u

static const char *const kind_names[CONTENT_KIND_COUNT] = {"texture", "animation", "sound", "model", "lightmap",
                                                           "collision", "model_animation", "font", "strings"};

const char *content_kind_name(enum content_kind kind)
{
    return (unsigned)kind < CONTENT_KIND_COUNT ? kind_names[kind] : "unknown";
}

uint32_t content_crc32(uint32_t crc, const unsigned char *data, uint32_t bytes)
{
    crc = ~crc;
    for (uint32_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int hex64(const char *text)
{
    if (strlen(text) != 64)
        return 0;
    for (int i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}

int content_read_cases(const char *list_path, const char *prefix, struct content_case *cases, unsigned capacity)
{
    FILE *file = fopen(list_path, "r");
    if (file == NULL)
        return -1;
    char line[640];
    unsigned count = 0;
    int ok = 1;
    while (ok && fgets(line, sizeof(line), file) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (count >= capacity) {
            ok = 0;
            break;
        }
        struct content_case *c = &cases[count];
        char kind[16], expect[16];
        unsigned long bytes;
        if (sscanf(line, "case %23s %15s %15s %255s %lu %64s %64s", c->id, kind, expect, c->path, &bytes,
                   c->file_sha, c->decoded_sha) != 7) {
            ok = 0;
            break;
        }
        c->kind = CONTENT_KIND_COUNT;
        for (int i = 0; i < CONTENT_KIND_COUNT; ++i)
            if (strcmp(kind, kind_names[i]) == 0)
                c->kind = (enum content_kind)i;
        c->expect = content_error_from_name(expect);
        c->bytes = (uint32_t)bytes;
        if (c->kind == CONTENT_KIND_COUNT || c->expect == CONTENT_ERROR_COUNT || !hex64(c->file_sha) ||
            (c->expect == CONTENT_OK ? !hex64(c->decoded_sha) : strcmp(c->decoded_sha, "-") != 0) || bytes == 0 ||
            bytes > CONTENT_CHECK_MAX_FILE_BYTES || strncmp(c->path, prefix, strlen(prefix)) != 0) {
            ok = 0;
            break;
        }
        ++count;
    }
    fclose(file);
    return ok ? (int)count : -1;
}

static void texture_case(const struct content_blob *blob, content_texture_hook hook, content_clock clock,
                         struct content_outcome *o)
{
    struct hwt_header h;
    o->error = hwt_parse(blob->data, blob->bytes, &h);
    if (o->error != CONTENT_OK)
        return;
    o->units = h.images;
    struct content_sha256 sha;
    content_sha256_init(&sha);
    uint64_t start = clock();
    for (uint32_t i = 0; i < h.images; ++i) {
        struct hwt_image image;
        if (hwt_image_at(&h, i, &image) != CONTENT_OK) {
            o->error = CONTENT_COUNT;
            return;
        }
        const unsigned char *data = blob->data + image.offset;
        unsigned char row[4 * 64];
        for (uint32_t y = 0; y < image.height; ++y)
            for (uint32_t x = 0; x < image.width; x += 64) {
                uint32_t run = image.width - x < 64 ? image.width - x : 64;
                for (uint32_t k = 0; k < run; ++k)
                    hwt_decode_texel(h.gx_format, data, image.width, x + k, y, row + 4 * k);
                content_sha256_update(&sha, row, 4 * run);
            }
    }
    o->decode_ticks = clock() - start;
    unsigned char digest[32];
    content_sha256_final(&sha, digest);
    content_hex(digest, o->decoded);
    if (hook != NULL)
        hook(blob, &h, o);
}

static void animation_case(const struct content_blob *blob, unsigned char *scratch, uint32_t scratch_bytes,
                           content_clock clock, struct content_outcome *o)
{
    struct hra_record r;
    o->error = hra_parse(blob->data, blob->bytes, &r);
    if (o->error != CONTENT_OK)
        return;
    o->units = r.event_count;
    unsigned char digest[32];
    uint64_t start = clock();
    o->error = hra_verify_source(&r, scratch, scratch_bytes, digest);
    o->decode_ticks = clock() - start;
    if (o->error == CONTENT_OK)
        content_hex(digest, o->decoded);
}

static void sound_case(const struct content_blob *blob, unsigned char *scratch, uint32_t scratch_bytes,
                       content_clock clock, struct content_outcome *o)
{
    struct hws_header h;
    o->error = hws_parse(blob->data, blob->bytes, &h);
    if (o->error != CONTENT_OK)
        return;
    o->units = h.frames;
    struct content_sha256 sha;
    content_sha256_init(&sha);
    uint64_t start = clock();
    if (h.codec == HWS_CODEC_PCM16BE) {
        content_sha256_update(&sha, h.payload, h.payload_bytes);
    } else {
        uint32_t chunk_samples = ADPCM_CHUNK_BLOCKS * HWS_ADPCM_BLOCK_SAMPLES * h.channels;
        if (scratch_bytes < 4u * chunk_samples) {
            o->error = CONTENT_CAPACITY;
            return;
        }
        int16_t *pcm = (int16_t *)(void *)scratch;
        unsigned char *bytes = scratch + 2u * chunk_samples;
        uint32_t blocks = hws_adpcm_blocks(&h);
        for (uint32_t first = 0; first < blocks; first += ADPCM_CHUNK_BLOCKS) {
            uint32_t count = blocks - first < ADPCM_CHUNK_BLOCKS ? blocks - first : ADPCM_CHUNK_BLOCKS;
            o->error = hws_decode_adpcm(&h, first, count, pcm);
            if (o->error != CONTENT_OK)
                return;
            uint32_t samples = count * HWS_ADPCM_BLOCK_SAMPLES * h.channels;
            /* Explicit big-endian PCM16 bytes, as the host digest. */
            for (uint32_t s = 0; s < samples; ++s)
                content_put_be16(bytes + 2 * s, (uint16_t)pcm[s]);
            content_sha256_update(&sha, bytes, 2u * samples);
        }
    }
    o->decode_ticks = clock() - start;
    unsigned char digest[32];
    content_sha256_final(&sha, digest);
    content_hex(digest, o->decoded);
}

/* The six decoded kinds: load into owned memory (timed as decode), measure
 * the heap with the file and decoded form held, release the file, measure
 * again, then digest the decoded form alone. */
union decoded {
    struct hwm_model model;
    struct hwl_geometry lightmap;
    struct hwc_collision collision;
    struct hma_graph graph;
    struct hwf_font font;
    struct hus_strings strings;
};

static enum content_error decoded_load(enum content_kind kind, const struct content_blob *blob, union decoded *d,
                                       struct content_outcome *o)
{
    enum content_error error;
    switch (kind) {
    case CONTENT_KIND_MODEL:
        error = hwm_load(blob->data, blob->bytes, &d->model);
        o->units = d->model.vertex_count;
        o->items = d->model.index_count;
        o->decoded_bytes = d->model.arena.bytes;
        break;
    case CONTENT_KIND_LIGHTMAP:
        error = hwl_load(blob->data, blob->bytes, &d->lightmap);
        o->units = d->lightmap.vertex_count;
        o->items = d->lightmap.surface_count;
        o->decoded_bytes = d->lightmap.arena.bytes;
        break;
    case CONTENT_KIND_COLLISION:
        error = hwc_load(blob->data, blob->bytes, &d->collision);
        for (uint32_t k = 0; k < HWC_ARRAYS; ++k)
            o->units += d->collision.counts[k];
        o->items = d->collision.bsp_count;
        o->decoded_bytes = d->collision.arena.bytes;
        break;
    case CONTENT_KIND_MODEL_ANIMATION:
        error = hma_load(blob->data, blob->bytes, &d->graph);
        o->units = d->graph.animation_count;
        o->items = d->graph.compressed_count;
        o->expanded_bytes = d->graph.expanded_bytes;
        o->decoded_bytes = d->graph.arena.bytes;
        break;
    case CONTENT_KIND_FONT:
        error = hwf_load(blob->data, blob->bytes, &d->font);
        o->units = d->font.font.character_count;
        o->items = d->font.font.pixel_bytes;
        o->decoded_bytes = d->font.arena.bytes;
        break;
    default:
        error = hus_load(blob->data, blob->bytes, &d->strings);
        o->units = d->strings.string_count;
        o->items = d->strings.unit_count;
        o->decoded_bytes = d->strings.arena.bytes;
        break;
    }
    return error;
}

static void decoded_digest_release(enum content_kind kind, union decoded *d, unsigned char digest[32])
{
    switch (kind) {
    case CONTENT_KIND_MODEL: hwm_digest(&d->model, digest); hwm_release(&d->model); break;
    case CONTENT_KIND_LIGHTMAP: hwl_digest(&d->lightmap, digest); hwl_release(&d->lightmap); break;
    case CONTENT_KIND_COLLISION: hwc_digest(&d->collision, digest); hwc_release(&d->collision); break;
    case CONTENT_KIND_MODEL_ANIMATION: hma_digest(&d->graph, digest); hma_release(&d->graph); break;
    case CONTENT_KIND_FONT: hwf_digest(&d->font, digest); hwf_release(&d->font); break;
    default: hus_digest(&d->strings, digest); hus_release(&d->strings); break;
    }
}

static void decoded_case(enum content_kind kind, struct content_blob *blob, content_clock clock, content_heap heap,
                         int32_t heap_base, struct content_outcome *o)
{
    union decoded d;
    uint64_t start = clock();
    o->error = decoded_load(kind, blob, &d, o);
    o->decode_ticks = clock() - start;
    if (o->error != CONTENT_OK)
        return;
    o->heap_peak = heap != NULL ? heap() - heap_base : -1;
    content_blob_release(blob);
    o->heap_resident = heap != NULL ? heap() - heap_base : -1;
    unsigned char digest[32];
    start = clock();
    decoded_digest_release(kind, &d, digest);
    o->digest_ticks = clock() - start;
    content_hex(digest, o->decoded);
}

void content_run_case(const struct content_case *c, unsigned char *scratch, uint32_t scratch_bytes,
                      content_texture_hook hook, content_clock clock, content_heap heap, struct content_outcome *o)
{
    memset(o, 0, sizeof(*o));
    o->control = -1;
    o->heap_peak = o->heap_resident = -1;
    strcpy(o->decoded, "-");
    struct content_blob blob;
    int32_t heap_base = heap != NULL ? heap() : 0;
    uint64_t start = clock();
    enum content_error load = content_load_file(c->path, CONTENT_CHECK_MAX_FILE_BYTES, &blob);
    o->load_ticks = clock() - start;
    if (load != CONTENT_OK) {
        o->error = load;
        return;
    }
    unsigned char digest[32];
    char hex[65];
    content_sha256(blob.data, blob.bytes, digest);
    content_hex(digest, hex);
    o->file_ok = blob.bytes == c->bytes && strcmp(hex, c->file_sha) == 0;
    if (c->kind == CONTENT_KIND_TEXTURE)
        texture_case(&blob, hook, clock, o);
    else if (c->kind == CONTENT_KIND_ANIMATION)
        animation_case(&blob, scratch, scratch_bytes, clock, o);
    else if (c->kind == CONTENT_KIND_SOUND)
        sound_case(&blob, scratch, scratch_bytes, clock, o);
    else
        decoded_case(c->kind, &blob, clock, heap, heap_base, o);
    content_blob_release(&blob);
    if (c->expect == CONTENT_OK) {
        o->decoded_match = o->error == CONTENT_OK && strcmp(o->decoded, c->decoded_sha) == 0;
        o->pass = o->file_ok && o->decoded_match && o->readback.mismatches == 0;
    } else {
        o->pass = o->file_ok && o->error == c->expect;
    }
}
