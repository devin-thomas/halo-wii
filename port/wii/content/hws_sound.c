/* HWS1 sound container loader and Xbox ADPCM decoder (HWI-008C). See
 * hws_sound.h. The decoder matches port/linux/src/dsound_sdl.c and
 * tools/wii/media_formats.py decode_xbox_adpcm. */
#include "hws_sound.h"

#include <string.h>

static const int8_t index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

enum content_error hws_parse(const unsigned char *data, uint32_t bytes, struct hws_header *out)
{
    if (data == NULL || out == NULL)
        return CONTENT_ARGUMENT;
    if (bytes < HWS_HEADER_BYTES)
        return CONTENT_TRUNCATED;
    if (memcmp(data, "HWS1", 4) != 0)
        return CONTENT_MAGIC;
    struct hws_header h;
    h.version = content_be16(data + 4);
    h.codec = content_be16(data + 6);
    h.rate = content_be32(data + 8);
    h.channels = content_be16(data + 12);
    uint32_t reserved = content_be16(data + 14);
    h.frames = content_be32(data + 16);
    h.payload_bytes = content_be32(data + 20);
    h.payload = data + HWS_HEADER_BYTES;
    if (h.version != 1)
        return CONTENT_VERSION;
    if ((h.codec != HWS_CODEC_XBOX_ADPCM && h.codec != HWS_CODEC_PCM16BE) || (h.rate != 22050 && h.rate != 44100) ||
        (h.channels != 1 && h.channels != 2))
        return CONTENT_ENUM;
    if (reserved != 0)
        return CONTENT_RESERVED;
    for (uint32_t i = 24; i < HWS_HEADER_BYTES; ++i)
        if (data[i] != 0)
            return CONTENT_RESERVED;
    if (h.payload_bytes != bytes - HWS_HEADER_BYTES)
        return CONTENT_LENGTH;
    if (h.codec == HWS_CODEC_XBOX_ADPCM) {
        uint32_t block = HWS_ADPCM_BLOCK_BYTES * h.channels;
        if (h.payload_bytes % block != 0 || (uint64_t)h.payload_bytes / block * HWS_ADPCM_BLOCK_SAMPLES != h.frames)
            return CONTENT_COUNT;
    } else if ((uint64_t)h.frames * 2u * h.channels != h.payload_bytes) {
        return CONTENT_COUNT;
    }
    *out = h;
    return CONTENT_OK;
}

uint32_t hws_adpcm_blocks(const struct hws_header *header)
{
    return header->codec == HWS_CODEC_XBOX_ADPCM ? header->payload_bytes / (HWS_ADPCM_BLOCK_BYTES * header->channels)
                                                 : 0;
}

enum content_error hws_decode_adpcm(const struct hws_header *header, uint32_t first, uint32_t count, int16_t *out)
{
    if (header == NULL || out == NULL || header->codec != HWS_CODEC_XBOX_ADPCM)
        return CONTENT_ARGUMENT;
    uint32_t blocks = hws_adpcm_blocks(header);
    if (first > blocks || count > blocks - first)
        return CONTENT_ARGUMENT;
    const uint32_t channels = header->channels, block_bytes = HWS_ADPCM_BLOCK_BYTES * channels;
    for (uint32_t block = 0; block < count; ++block) {
        const unsigned char *base = header->payload + (first + block) * block_bytes;
        int16_t *frames = out + block * HWS_ADPCM_BLOCK_SAMPLES * channels;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            /* Explicit little-endian block header: first sample, step index. */
            int32_t predictor = (int32_t)(base[4 * channel] | ((uint32_t)base[4 * channel + 1] << 8));
            if (predictor & 0x8000)
                predictor -= 0x10000;
            int32_t index = base[4 * channel + 2] > 88 ? 88 : base[4 * channel + 2];
            frames[channel] = (int16_t)predictor;
            for (uint32_t group = 0; group < 8; ++group) {
                const unsigned char *nibbles = base + 4 * channels + (group * channels + channel) * 4;
                for (uint32_t k = 0; k < 4; ++k) {
                    uint32_t value = nibbles[k];
                    uint32_t position = group * 8 + k * 2 + 1;
                    for (uint32_t half = 0; half < 2; ++half, ++position) {
                        if (position >= HWS_ADPCM_BLOCK_SAMPLES)
                            continue;
                        uint32_t nibble = half ? value >> 4 : value & 15u;
                        int32_t step = step_table[index];
                        int32_t diff = step >> 3;
                        if (nibble & 1) diff += step >> 2;
                        if (nibble & 2) diff += step >> 1;
                        if (nibble & 4) diff += step;
                        predictor += (nibble & 8) ? -diff : diff;
                        if (predictor > 32767) predictor = 32767;
                        if (predictor < -32768) predictor = -32768;
                        index += index_table[nibble];
                        if (index < 0) index = 0;
                        if (index > 88) index = 88;
                        frames[position * channels + channel] = (int16_t)predictor;
                    }
                }
            }
        }
    }
    return CONTENT_OK;
}
