/* HWI-008D host reference: runs the same decode core as the Wii benchmark DOL
 * and writes per-frame CRCs of the packed XFB output, so the guest can be
 * checked frame for frame. Optional raw output feeds quality comparisons.
 *
 *   media_bench_host mpeg1 <movie.mpg> <crcs.bin> [planes.i420]
 *   media_bench_host mjpeg <movie.hmj> <crcs.bin> [frames.rgb]
 *   media_bench_host adpcm <sound.hws>
 *
 * crcs.bin holds one big-endian u32 per frame. A JSON summary goes to stdout.
 * Build: cc -std=c11 -O2 -ffp-contract=off host_main.c media_core.c media_decoders.c -lm */
#include "media_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return NULL;
    long length = ftell(file);
    uint8_t *data = length > 0 ? malloc((size_t)length) : NULL;
    if (data == NULL || fseek(file, 0, SEEK_SET) != 0 || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static int put_be32(FILE *file, uint32_t value)
{
    unsigned char bytes[4] = {value >> 24, value >> 16, value >> 8, value};
    return fwrite(bytes, 1, 4, file) == 4;
}

static int run_mpeg1(const char *path, const char *crcs, const char *raw)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    FILE *out = fopen(crcs, "wb"), *planes_out = raw ? fopen(raw, "wb") : NULL;
    if (data == NULL || out == NULL || (raw && planes_out == NULL))
        return 2;
    struct media_mpeg *movie = media_mpeg_open_memory(data, size, 1, 0);
    if (movie == NULL)
        return 2;
    unsigned width = (unsigned)media_mpeg_width(movie), height = (unsigned)media_mpeg_height(movie);
    uint8_t *xfb = malloc((size_t)width * height * 2u);
    struct media_planes planes;
    double time, first_time = -1.0, last_time = 0.0;
    uint32_t all = 0;
    unsigned frames = 0;
    while (media_mpeg_video(movie, &planes, &time)) {
        media_pack_yuyv_from_420(xfb, width * 2u, &planes);
        uint32_t crc = media_crc32(0, xfb, (size_t)width * height * 2u);
        all = media_crc32(all, (const unsigned char[]){crc >> 24, crc >> 16, crc >> 8, crc}, 4);
        if (!put_be32(out, crc))
            return 2;
        if (planes_out) {
            for (unsigned row = 0; row < height; ++row)
                fwrite(planes.y + (size_t)row * planes.y_stride, 1, width, planes_out);
            for (unsigned row = 0; row < height / 2; ++row)
                fwrite(planes.cb + (size_t)row * planes.c_stride, 1, width / 2, planes_out);
            for (unsigned row = 0; row < height / 2; ++row)
                fwrite(planes.cr + (size_t)row * planes.c_stride, 1, width / 2, planes_out);
        }
        if (first_time < 0)
            first_time = time;
        last_time = time;
        ++frames;
    }
    size_t video_peak = media_heap.peak;
    media_mpeg_close(movie);
    media_heap_reset_peak();
    movie = media_mpeg_open_memory(data, size, 0, 1);
    if (movie == NULL)
        return 2;
    int16_t stereo[MEDIA_MP2_FRAME * 2];
    unsigned count, audio_frames = 0;
    double audio_first = -1.0;
    uint32_t audio_crc = 0;
    int rate = media_mpeg_samplerate(movie);
    while (media_mpeg_audio(movie, stereo, &count, &time)) {
        if (audio_first < 0)
            audio_first = time;
        audio_crc = media_crc32_s16be(audio_crc, stereo, count * 2u);
        audio_frames += count;
    }
    printf("{\"codec\": \"mpeg1\", \"width\": %u, \"height\": %u, \"frames\": %u, \"first_time\": %.6f, "
           "\"last_time\": %.6f, \"sequence_crc\": \"%08x\", \"video_heap_peak\": %lu, \"audio_rate\": %d, "
           "\"audio_frames\": %u, \"audio_first_time\": %.6f, \"audio_crc\": \"%08x\", \"audio_heap_peak\": %lu}\n",
           width, height, frames, first_time, last_time, (unsigned)all, (unsigned long)video_peak, rate,
           audio_frames, audio_first, (unsigned)audio_crc, (unsigned long)media_heap.peak);
    media_mpeg_close(movie);
    return fclose(out) != 0 || (planes_out && fclose(planes_out) != 0) ? 2 : 0;
}

static int run_mjpeg(const char *path, const char *crcs, const char *raw)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    FILE *out = fopen(crcs, "wb"), *rgb_out = raw ? fopen(raw, "wb") : NULL;
    struct media_hmj hmj;
    if (data == NULL || out == NULL || (raw && rgb_out == NULL) || !media_hmj_header(data, size, &hmj) ||
        !media_hmj_check_index(data + MEDIA_HMJ_HEADER, &hmj, size))
        return 2;
    uint8_t *xfb = malloc((size_t)hmj.width * hmj.height * 2u);
    uint32_t all = 0;
    for (uint32_t i = 0; i < hmj.frames; ++i) {
        uint32_t start = media_hmj_offset(data + MEDIA_HMJ_HEADER, i);
        uint32_t end = media_hmj_offset(data + MEDIA_HMJ_HEADER, i + 1);
        int width, height;
        uint8_t *rgb = media_jpeg_decode(data + start, end - start, &width, &height);
        if (rgb == NULL || (unsigned)width != hmj.width || (unsigned)height != hmj.height)
            return 3;
        media_pack_yuyv_from_rgb(xfb, hmj.width * 2u, rgb, hmj.width, hmj.height);
        uint32_t crc = media_crc32(0, xfb, (size_t)hmj.width * hmj.height * 2u);
        all = media_crc32(all, (const unsigned char[]){crc >> 24, crc >> 16, crc >> 8, crc}, 4);
        if (!put_be32(out, crc))
            return 2;
        if (rgb_out)
            fwrite(rgb, 1, (size_t)width * height * 3u, rgb_out);
        media_jpeg_free(rgb);
    }
    printf("{\"codec\": \"mjpeg\", \"width\": %u, \"height\": %u, \"frames\": %u, \"sequence_crc\": \"%08x\", "
           "\"video_heap_peak\": %lu}\n",
           (unsigned)hmj.width, (unsigned)hmj.height, (unsigned)hmj.frames, (unsigned)all,
           (unsigned long)media_heap.peak);
    return fclose(out) != 0 || (rgb_out && fclose(rgb_out) != 0) ? 2 : 0;
}

static int run_adpcm(const char *path)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    struct media_hws hws;
    if (data == NULL || !media_hws_header(data, size, &hws) || hws.codec != 1)
        return 2;
    int16_t *pcm = malloc((size_t)hws.frames * hws.channels * sizeof(int16_t) + 1);
    long frames = media_xbox_adpcm_decode(data + MEDIA_HWS_HEADER, hws.payload, hws.channels, pcm);
    if (frames < 0 || (uint32_t)frames != hws.frames)
        return 3;
    printf("{\"codec\": \"xbox_adpcm\", \"rate\": %u, \"channels\": %u, \"frames\": %ld, \"crc\": \"%08x\"}\n",
           (unsigned)hws.rate, (unsigned)hws.channels, frames,
           (unsigned)media_crc32_s16be(0, pcm, (size_t)frames * hws.channels));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "mpeg1") == 0)
        return run_mpeg1(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    if (argc >= 4 && strcmp(argv[1], "mjpeg") == 0)
        return run_mjpeg(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    if (argc == 3 && strcmp(argv[1], "adpcm") == 0)
        return run_adpcm(argv[2]);
    fprintf(stderr, "usage: media_bench_host mpeg1|mjpeg <file> <crcs.bin> [raw] | adpcm <file.hws>\n");
    return 1;
}
