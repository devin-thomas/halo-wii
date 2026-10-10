/* HWI-008D authored decode helpers: allocator accounting, CRC, XFB packing,
 * HMJ1/HWS1 headers and the Xbox ADPCM decoder. Shared by the Wii DOL and the
 * host reference build. */
#include "media_core.h"

#include <stdlib.h>
#include <string.h>

/* ---- counting allocator --------------------------------------------------- */
struct media_heap media_heap;

/* The size header keeps the 16-byte alignment both decoders may assume. */
#define HEADER_BYTES 16u

void *media_alloc(size_t size)
{
    unsigned char *block = malloc(size + HEADER_BYTES);
    if (block == NULL)
        return NULL;
    memcpy(block, &size, sizeof(size));
    media_heap.current += size;
    ++media_heap.allocations;
    if (media_heap.current > media_heap.peak)
        media_heap.peak = media_heap.current;
    return block + HEADER_BYTES;
}

void media_free(void *pointer)
{
    if (pointer == NULL)
        return;
    unsigned char *block = (unsigned char *)pointer - HEADER_BYTES;
    size_t size;
    memcpy(&size, block, sizeof(size));
    media_heap.current -= size;
    free(block);
}

void *media_realloc(void *pointer, size_t size)
{
    if (pointer == NULL)
        return media_alloc(size);
    unsigned char *block = (unsigned char *)pointer - HEADER_BYTES;
    size_t old;
    memcpy(&old, block, sizeof(old));
    unsigned char *grown = realloc(block, size + HEADER_BYTES);
    if (grown == NULL)
        return NULL;
    memcpy(grown, &size, sizeof(size));
    media_heap.current = media_heap.current - old + size;
    if (media_heap.current > media_heap.peak)
        media_heap.peak = media_heap.current;
    return grown + HEADER_BYTES;
}

void media_heap_reset_peak(void)
{
    media_heap.peak = media_heap.current;
}

/* ---- CRC-32 (reflected 0xEDB88320, as zlib) ------------------------------- */
static uint32_t crc_table[256];
static int crc_ready;

uint32_t media_crc32(uint32_t crc, const void *data, size_t length)
{
    if (!crc_ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (c & 1u)));
            crc_table[n] = c;
        }
        crc_ready = 1;
    }
    const unsigned char *bytes = data;
    crc = ~crc;
    while (length--)
        crc = crc_table[(crc ^ *bytes++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint32_t media_crc32_s16be(uint32_t crc, const int16_t *samples, size_t count)
{
    unsigned char chunk[256];
    while (count) {
        size_t n = count < sizeof(chunk) / 2 ? count : sizeof(chunk) / 2;
        for (size_t i = 0; i < n; ++i) {
            uint16_t value = (uint16_t)samples[i];
            chunk[2 * i] = (unsigned char)(value >> 8);
            chunk[2 * i + 1] = (unsigned char)value;
        }
        crc = media_crc32(crc, chunk, 2 * n);
        samples += n;
        count -= n;
    }
    return crc;
}

/* ---- XFB packing ------------------------------------------------------------ */
void media_pack_yuyv_from_420(uint8_t *dst, unsigned dst_stride, const struct media_planes *p)
{
    for (unsigned row = 0; row < p->height; ++row) {
        const uint8_t *y = p->y + (size_t)row * p->y_stride;
        const uint8_t *cb = p->cb + (size_t)(row >> 1) * p->c_stride;
        const uint8_t *cr = p->cr + (size_t)(row >> 1) * p->c_stride;
        uint8_t *out = dst + (size_t)row * dst_stride;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        /* One aligned word store per pixel pair; same bytes as the generic path. */
        if (((uintptr_t)out & 3u) == 0) {
            uint32_t *words = (uint32_t *)(void *)out;
            for (unsigned pair = 0; pair < p->width / 2; ++pair, y += 2)
                words[pair] = (uint32_t)y[0] << 24 | (uint32_t)cb[pair] << 16 | (uint32_t)y[1] << 8 | cr[pair];
            continue;
        }
#endif
        for (unsigned pair = 0; pair < p->width / 2; ++pair) {
            out[0] = y[0];
            out[1] = cb[pair];
            out[2] = y[1];
            out[3] = cr[pair];
            out += 4;
            y += 2;
        }
    }
}

static uint8_t clamp_u8(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

/* BT.601 studio swing from full-range RGB, 8.8 fixed point with rounding. */
void media_pack_yuyv_from_rgb(uint8_t *dst, unsigned dst_stride, const uint8_t *rgb, unsigned width,
                              unsigned height)
{
    for (unsigned row = 0; row < height; ++row) {
        const uint8_t *in = rgb + (size_t)row * width * 3u;
        uint8_t *out = dst + (size_t)row * dst_stride;
        for (unsigned pair = 0; pair < width / 2; ++pair) {
            int r0 = in[0], g0 = in[1], b0 = in[2], r1 = in[3], g1 = in[4], b1 = in[5];
            int r = r0 + r1, g = g0 + g1, b = b0 + b1;
            out[0] = clamp_u8(16 + ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8));
            out[2] = clamp_u8(16 + ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8));
            out[1] = clamp_u8(128 + ((-38 * r - 74 * g + 112 * b + 256) >> 9));
            out[3] = clamp_u8(128 + ((112 * r - 94 * g - 18 * b + 256) >> 9));
            in += 6;
            out += 4;
        }
    }
}

int16_t media_float_to_s16(float value)
{
    float scaled = value * 32767.0f;
    if (scaled >= 32767.0f)
        return 32767;
    if (scaled <= -32768.0f)
        return -32768;
    return (int16_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

/* ---- explicit byte-order loads ----------------------------------------------- */
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

int media_hmj_header(const uint8_t *data, size_t size, struct media_hmj *out)
{
    if (size < MEDIA_HMJ_HEADER || memcmp(data, "HMJ1", 4) != 0)
        return 0;
    out->frames = be32(data + 4);
    out->fps_num = be32(data + 8);
    out->fps_den = be32(data + 12);
    out->width = be16(data + 16);
    out->height = be16(data + 18);
    out->audio_rate = be32(data + 20);
    out->audio_channels = be32(data + 24);
    if (be32(data + 28) != 0 || out->frames == 0 || out->frames > 1000000u || out->fps_num == 0 ||
        out->fps_den == 0 || out->width == 0 || out->width > 1024 || out->width % 2 || out->height == 0 ||
        out->height > 1024)
        return 0;
    return 1;
}

uint32_t media_hmj_offset(const uint8_t *table, uint32_t index)
{
    return be32(table + 4u * index);
}

int media_hmj_check_index(const uint8_t *table, const struct media_hmj *hmj, size_t file_size)
{
    uint32_t previous = MEDIA_HMJ_HEADER + 4u * (hmj->frames + 1u);
    for (uint32_t i = 0; i <= hmj->frames; ++i) {
        uint32_t offset = media_hmj_offset(table, i);
        if (offset < previous || offset > file_size || (i > 0 && offset == previous))
            return 0;
        previous = offset;
    }
    return previous == file_size;
}

int media_hws_header(const uint8_t *data, size_t size, struct media_hws *out)
{
    if (size < MEDIA_HWS_HEADER || memcmp(data, "HWS1", 4) != 0)
        return 0;
    out->version = be16(data + 4);
    out->codec = be16(data + 6);
    out->rate = be32(data + 8);
    out->channels = be16(data + 12);
    out->frames = be32(data + 16);
    out->payload = be32(data + 20);
    return out->version == 1 && be16(data + 14) == 0 && out->payload == size - MEDIA_HWS_HEADER &&
           (out->channels == 1 || out->channels == 2);
}

/* ---- Xbox ADPCM ----------------------------------------------------------------- */
static const int8_t adpcm_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767};

/* Per block and channel: a little-endian header word (first sample, step
 * index), then 4-byte groups of eight nibbles, low nibble first, alternating
 * channels; 64 samples per channel, the 64th nibble ignored. This matches
 * port/linux/src/dsound_sdl.c and tools/wii/media_formats.py. */
long media_xbox_adpcm_decode(const uint8_t *src, size_t bytes, unsigned channels, int16_t *dst)
{
    if (channels != 1 && channels != 2)
        return -1;
    size_t block_bytes = MEDIA_ADPCM_BLOCK * channels;
    if (bytes % block_bytes)
        return -1;
    size_t blocks = bytes / block_bytes;
    for (size_t block = 0; block < blocks; ++block) {
        const uint8_t *base = src + block * block_bytes;
        int16_t *out = dst + block * MEDIA_ADPCM_SAMPLES * channels;
        for (unsigned channel = 0; channel < channels; ++channel) {
            const uint8_t *head = base + 4u * channel;
            int predictor = (int16_t)(uint16_t)(head[0] | head[1] << 8);
            int index = head[2] > 88 ? 88 : head[2];
            out[channel] = (int16_t)predictor;
            unsigned position = 1;
            for (unsigned group = 0; group < 8; ++group) {
                const uint8_t *nibbles = base + 4u * channels + (group * channels + channel) * 4u;
                for (unsigned k = 0; k < 8; ++k, ++position) {
                    if (position >= MEDIA_ADPCM_SAMPLES)
                        break;
                    unsigned nibble = (nibbles[k >> 1] >> ((k & 1u) * 4u)) & 15u;
                    int step = adpcm_step[index];
                    int diff = step >> 3;
                    if (nibble & 1u)
                        diff += step >> 2;
                    if (nibble & 2u)
                        diff += step >> 1;
                    if (nibble & 4u)
                        diff += step;
                    predictor += (nibble & 8u) ? -diff : diff;
                    predictor = predictor < -32768 ? -32768 : predictor > 32767 ? 32767 : predictor;
                    index += adpcm_index[nibble];
                    index = index < 0 ? 0 : index > 88 ? 88 : index;
                    out[position * channels + channel] = (int16_t)predictor;
                }
            }
        }
    }
    return (long)(blocks * MEDIA_ADPCM_SAMPLES);
}
