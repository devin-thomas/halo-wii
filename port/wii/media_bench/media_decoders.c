/* HWI-008D: the vendored decoders' implementations (pinned upstream headers,
 * port/wii/third_party/README.md) and thin wrappers around them. Both route
 * every allocation through the counting allocator in media_core.c. */
#include "media_core.h"

#include <stdio.h>
#include <string.h>

#define PLM_MALLOC(size) media_alloc(size)
#define PLM_REALLOC(pointer, size) media_realloc(pointer, size)
#define PLM_FREE(pointer) media_free(pointer)
#define PL_MPEG_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/pl_mpeg/pl_mpeg_wii.h"
#pragma GCC diagnostic pop

#define STBI_MALLOC(size) media_alloc(size)
#define STBI_REALLOC(pointer, size) media_realloc(pointer, size)
#define STBI_FREE(pointer) media_free(pointer)
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

struct media_mpeg {
    plm_t *plm;
    uint64_t (*clock)(void);
    media_io_hook hook;
    void *user;
};

static struct media_mpeg *wrap(plm_t *plm, int video, int audio)
{
    if (plm == NULL)
        return NULL;
    struct media_mpeg *movie = media_alloc(sizeof(*movie));
    if (movie == NULL) {
        plm_destroy(plm);
        return NULL;
    }
    memset(movie, 0, sizeof(*movie));
    movie->plm = plm;
    plm_set_video_enabled(plm, video);
    plm_set_audio_enabled(plm, audio);
    if (!plm_has_headers(plm) || (video && plm_get_num_video_streams(plm) < 1) ||
        (audio && plm_get_num_audio_streams(plm) < 1)) {
        media_mpeg_close(movie);
        return NULL;
    }
    return movie;
}

struct media_mpeg *media_mpeg_open_memory(uint8_t *bytes, size_t length, int video, int audio)
{
    return wrap(plm_create_with_memory(bytes, length, 0), video, audio);
}

/* The library's own file reader, timed. */
static void timed_load(plm_buffer_t *buffer, void *user)
{
    struct media_mpeg *movie = user;
    long before = ftell(buffer->fh);
    uint64_t start = movie->clock ? movie->clock() : 0;
    plm_buffer_load_file_callback(buffer, NULL);
    uint64_t end = movie->clock ? movie->clock() : 0;
    long after = ftell(buffer->fh);
    if (movie->hook)
        movie->hook(end - start, before >= 0 && after > before ? (size_t)(after - before) : 0, movie->user);
}

struct media_mpeg *media_mpeg_open_file(const char *path, int video, int audio, uint64_t (*clock)(void),
                                        media_io_hook hook, void *user)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    plm_buffer_t *buffer = plm_buffer_create_with_file(file, 1);
    struct media_mpeg *movie = media_alloc(sizeof(*movie));
    if (buffer == NULL || movie == NULL) {
        if (buffer)
            plm_buffer_destroy(buffer);
        else
            fclose(file);
        media_free(movie);
        return NULL;
    }
    memset(movie, 0, sizeof(*movie));
    movie->clock = clock;
    movie->hook = hook;
    movie->user = user;
    plm_buffer_set_load_callback(buffer, timed_load, movie);
    plm_t *plm = plm_create_with_buffer(buffer, 1);
    if (plm == NULL) {
        media_free(movie);
        return NULL;
    }
    movie->plm = plm;
    plm_set_video_enabled(plm, video);
    plm_set_audio_enabled(plm, audio);
    if (!plm_has_headers(plm) || (video && plm_get_num_video_streams(plm) < 1) ||
        (audio && plm_get_num_audio_streams(plm) < 1)) {
        media_mpeg_close(movie);
        return NULL;
    }
    return movie;
}

void media_mpeg_close(struct media_mpeg *movie)
{
    if (movie == NULL)
        return;
    plm_destroy(movie->plm);
    media_free(movie);
}

int media_mpeg_width(struct media_mpeg *movie) { return plm_get_width(movie->plm); }
int media_mpeg_height(struct media_mpeg *movie) { return plm_get_height(movie->plm); }
double media_mpeg_framerate(struct media_mpeg *movie) { return plm_get_framerate(movie->plm); }
int media_mpeg_samplerate(struct media_mpeg *movie) { return plm_get_samplerate(movie->plm); }

int media_mpeg_video(struct media_mpeg *movie, struct media_planes *planes, double *time)
{
    plm_frame_t *frame = plm_decode_video(movie->plm);
    if (frame == NULL)
        return 0;
    planes->y = frame->y.data;
    planes->cb = frame->cb.data;
    planes->cr = frame->cr.data;
    planes->y_stride = frame->y.width;
    planes->c_stride = frame->cb.width;
    planes->width = frame->width;
    planes->height = frame->height;
    *time = frame->time;
    return 1;
}

int media_mpeg_audio(struct media_mpeg *movie, int16_t *stereo, unsigned *frames, double *time)
{
    plm_samples_t *samples = plm_decode_audio(movie->plm);
    if (samples == NULL)
        return 0;
    for (unsigned i = 0; i < samples->count * 2u; ++i)
        stereo[i] = media_float_to_s16(samples->interleaved[i]);
    *frames = samples->count;
    *time = samples->time;
    return 1;
}

uint8_t *media_jpeg_decode(const uint8_t *jpeg, size_t length, int *width, int *height)
{
    int components;
    if (length > 0x7FFFFFFF)
        return NULL;
    return stbi_load_from_memory(jpeg, (int)length, width, height, &components, 3);
}

void media_jpeg_free(uint8_t *rgb)
{
    stbi_image_free(rgb);
}
