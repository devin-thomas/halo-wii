/* HWS1 sound container loader and Xbox ADPCM decoder (HWI-008C).
 *
 * Layout (tools/wii/content_convert.py pack_hws): a 32-byte big-endian
 * header (magic, version 1, codec, sample rate, channels, reserved 0,
 * frames, payload bytes; zero to 32 bytes), then the payload. Codec 1 is
 * Xbox ADPCM kept verbatim: per channel and 36-byte block, a little-endian
 * first sample and step index, then 32 bytes of nibbles; 64 samples per
 * block. Codec 2 is big-endian PCM16. */
#ifndef HALO_WII_HWS_SOUND_H
#define HALO_WII_HWS_SOUND_H

#include "content_common.h"

#define HWS_HEADER_BYTES 32u
#define HWS_ADPCM_BLOCK_BYTES 36u
#define HWS_ADPCM_BLOCK_SAMPLES 64u

enum { HWS_CODEC_XBOX_ADPCM = 1, HWS_CODEC_PCM16BE = 2 };

struct hws_header {
    uint32_t version, codec, rate, channels, frames, payload_bytes;
    const unsigned char *payload;
};

enum content_error hws_parse(const unsigned char *data, uint32_t bytes, struct hws_header *out);

/* Blocks (all channels) in an ADPCM payload. */
uint32_t hws_adpcm_blocks(const struct hws_header *header);

/* Decodes ADPCM blocks [first, first + count) into interleaved signed 16-bit
 * samples (count * 64 frames). The block header word is read explicitly
 * little-endian; a step index above 88 is clamped as the Xbox decoder does. */
enum content_error hws_decode_adpcm(const struct hws_header *header, uint32_t first, uint32_t count, int16_t *out);

#endif
