/* HWI-008D movie and audio decode core, shared by the Wii benchmark DOL and
 * its host reference build so both run the same decoder code.
 *
 * Decoders: pl_mpeg (MIT) for MPEG-1 video and MP2 audio, stb_image (MIT or
 * public domain) for baseline JPEG frames, and an authored Xbox ADPCM decoder.
 * All container and header fields are read with explicit big-endian or
 * little-endian byte loads; nothing relies on host struct layout (ADR-018). */
#ifndef HALO_WII_MEDIA_CORE_H
#define HALO_WII_MEDIA_CORE_H

#include <stddef.h>
#include <stdint.h>

/* ---- counting allocator used by both third-party decoders ------------- */
struct media_heap {
    size_t current, peak;
    unsigned long allocations;
};
extern struct media_heap media_heap;
void *media_alloc(size_t size);
void *media_realloc(void *pointer, size_t size);
void media_free(void *pointer);
void media_heap_reset_peak(void);

/* ---- checksums and output packing ------------------------------------- */
uint32_t media_crc32(uint32_t crc, const void *data, size_t length);
/* CRC of signed 16-bit samples serialised as big-endian bytes. */
uint32_t media_crc32_s16be(uint32_t crc, const int16_t *samples, size_t count);

struct media_planes {
    const uint8_t *y, *cb, *cr;
    unsigned y_stride, c_stride, width, height;
};

/* 4:2:0 planes -> Wii XFB Y'CbCr 4:2:2 (bytes Y0 Cb Y1 Cr per pixel pair).
 * Chroma rows are repeated (nearest), never interpolated. width must be even. */
void media_pack_yuyv_from_420(uint8_t *dst, unsigned dst_stride, const struct media_planes *planes);
/* Full-range RGB24 -> studio-range Y'CbCr 4:2:2 (BT.601), chroma averaged per pair. */
void media_pack_yuyv_from_rgb(uint8_t *dst, unsigned dst_stride, const uint8_t *rgb, unsigned width,
                              unsigned height);

/* ---- MPEG-1 program stream through pl_mpeg ----------------------------- */
struct media_mpeg;
typedef void (*media_io_hook)(uint64_t elapsed_units, size_t bytes, void *user);
/* Opens from memory the caller keeps alive; video and audio can be disabled. */
struct media_mpeg *media_mpeg_open_memory(uint8_t *bytes, size_t length, int video, int audio);
/* Opens a file read in PLM_BUFFER_DEFAULT_SIZE chunks; hook (may be NULL)
 * receives the time each read took, in units from clock(). */
struct media_mpeg *media_mpeg_open_file(const char *path, int video, int audio,
                                        uint64_t (*clock)(void), media_io_hook hook, void *user);
void media_mpeg_close(struct media_mpeg *movie);
int media_mpeg_width(struct media_mpeg *movie);
int media_mpeg_height(struct media_mpeg *movie);
double media_mpeg_framerate(struct media_mpeg *movie);
int media_mpeg_samplerate(struct media_mpeg *movie);
/* 1 and the next frame's planes and presentation time, or 0 at the end. */
int media_mpeg_video(struct media_mpeg *movie, struct media_planes *planes, double *time);
/* 1 and up to MEDIA_MP2_FRAME interleaved stereo s16 frames, or 0 at the end. */
#define MEDIA_MP2_FRAME 1152u
int media_mpeg_audio(struct media_mpeg *movie, int16_t *stereo, unsigned *frames, double *time);
int16_t media_float_to_s16(float value);

/* ---- HMJ1 indexed MJPEG container (authored for this benchmark) -------- */
/* 32-byte big-endian header: "HMJ1", frames, fps_num, fps_den (u32), width,
 * height (u16), audio_rate, audio_channels, reserved (u32); then frames+1
 * u32 offsets from the file start; JPEG frame i spans [offset[i], offset[i+1]). */
#define MEDIA_HMJ_HEADER 32u
struct media_hmj {
    uint32_t frames, fps_num, fps_den, width, height, audio_rate, audio_channels;
};
int media_hmj_header(const uint8_t *data, size_t size, struct media_hmj *out);
uint32_t media_hmj_offset(const uint8_t *table, uint32_t index);
/* Validates the whole index against the file size; 1 when sound. */
int media_hmj_check_index(const uint8_t *table, const struct media_hmj *hmj, size_t file_size);

/* Baseline JPEG -> RGB24 through stb_image; free the result with media_jpeg_free. */
uint8_t *media_jpeg_decode(const uint8_t *jpeg, size_t length, int *width, int *height);
void media_jpeg_free(uint8_t *rgb);

/* ---- Xbox ADPCM (Halo sound compression 1) ------------------------------ */
#define MEDIA_ADPCM_BLOCK 36u
#define MEDIA_ADPCM_SAMPLES 64u
/* Frames decoded, or -1 for a partial block or an unsupported channel count.
 * dst receives frames*channels interleaved samples. */
long media_xbox_adpcm_decode(const uint8_t *src, size_t bytes, unsigned channels, int16_t *dst);

/* HWS1 sound container (tools/wii/content_convert.py). */
#define MEDIA_HWS_HEADER 32u
struct media_hws {
    uint32_t version, codec, rate, channels, frames, payload;
};
int media_hws_header(const uint8_t *data, size_t size, struct media_hws *out);

#endif
