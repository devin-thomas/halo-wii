/* HWI-008D movie and audio decoder benchmark (Wii DOL).
 *
 * Reads a job list from the SD card and, per job, decodes privately
 * transcoded movie clips or game sounds with the shared decode core
 * (media_core.c, media_decoders.c). Video frames are presented by packing
 * them into the Wii's Y'CbCr 4:2:2 external framebuffer (XFB). Timings come
 * from the guest timebase. The repository contains no game data; every
 * input is staged on SD by the caller.
 *
 * Jobs (sd:/halo-wii-media/jobs.txt, one per line, space separated):
 *   video <mpeg1|mjpeg> <name> <sd path> <expected sequence crc32 hex>
 *   mp2 <name> <sd path> <expected audio crc32 hex>
 *   adpcm <name> <sd path> <expected pcm crc32 hex>
 *   av mpeg1 <name> <movie.mpg> - <frame limit, 0 = all>
 *   av mjpeg <name> <movie.hmj> <audio.pcm> <frame limit, 0 = all>
 * Per-frame records go to sd:/halo-wii-media/<name>-<run>.bin and a summary
 * line per job to bench.log. */
#include <gccore.h>
#include <asndlib.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "build_id.h"
#include "media_core.h"

#define MEDIA_DIR "sd:/halo-wii-media"
#define JOBS_PATH MEDIA_DIR "/jobs.txt"
#define LOG_PATH MEDIA_DIR "/bench.log"
#define RUNS_PATH MEDIA_DIR "/runs.txt"
#define RUNS_PREFIX "halo-wii-media-v1 "
#define MAX_JOBS 32
#define MAX_FRAMES 8192u
#define XFB_W 640u
#define XFB_H 480u
#define XFB_BYTES (XFB_W * XFB_H * 2u)
#define XFB_SLOTS 6u
#define DSP_RATE 48000u
#define AUDIO_BUFFERS 8u
#define AUDIO_FRAMES 4096u
#define AUDIO_BYTES (AUDIO_FRAMES * 4u)

static FILE *record;
static int log_failed;
static unsigned run_number;
static uint8_t *xfb[XFB_SLOTS];
static unsigned xfb_index;
static volatile u32 retraces;

static void present_on_retrace(void);

static void count_retrace(u32 count)
{
    (void)count;
    ++retraces;
    present_on_retrace();
}

static void bench_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void bench_log(const char *format, ...)
{
    va_list args;
    if (record == NULL)
        return;
    va_start(args, format);
    if (vfprintf(record, format, args) < 0)
        log_failed = 1;
    va_end(args);
    if (fflush(record) != 0)
        log_failed = 1;
}

static uint64_t now(void)
{
    return gettime();
}

static uint32_t us(uint64_t ticks)
{
    return (uint32_t)ticks_to_microsecs(ticks);
}

/* ---- MEM2 bump region for whole-file inputs ------------------------------------- */
static uint8_t *mem2_begin, *mem2_end, *mem2_next;

static void mem2_init(void)
{
    mem2_begin = mem2_next = (uint8_t *)(((uintptr_t)SYS_GetArena2Lo() + 31u) & ~(uintptr_t)31u);
    mem2_end = (uint8_t *)((uintptr_t)SYS_GetArena2Hi() & ~(uintptr_t)31u);
    SYS_SetArena2Lo(mem2_end);
}

static uint8_t *mem2_take(size_t size)
{
    size = (size + 31u) & ~(size_t)31u;
    if ((size_t)(mem2_end - mem2_next) < size)
        return NULL;
    uint8_t *block = mem2_next;
    mem2_next += size;
    return block;
}

/* Whole file into MEM2; I/O time is returned separately from decode time. */
static uint8_t *load_file(const char *path, size_t *size, uint64_t *io_ticks)
{
    uint64_t start = now();
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        length = ftell(file);
    uint8_t *data = length > 0 ? mem2_take((size_t)length) : NULL;
    int ok = data != NULL && fseek(file, 0, SEEK_SET) == 0 &&
             fread(data, 1, (size_t)length, file) == (size_t)length;
    if (fclose(file) != 0 || !ok)
        return NULL;
    *size = (size_t)length;
    *io_ticks = now() - start;
    return data;
}

/* ---- statistics ------------------------------------------------------------------- */
static uint32_t sample_a[MAX_FRAMES], sample_b[MAX_FRAMES], sample_c[MAX_FRAMES], sorted[MAX_FRAMES];

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

struct stats {
    uint32_t mean, p95, max, min;
    uint64_t sum;
};

static struct stats summarize(const uint32_t *values, unsigned count)
{
    struct stats s = {0, 0, 0, 0, 0};
    if (count == 0)
        return s;
    memcpy(sorted, values, count * sizeof(uint32_t));
    qsort(sorted, count, sizeof(uint32_t), compare_u32);
    for (unsigned i = 0; i < count; ++i)
        s.sum += values[i];
    s.mean = (uint32_t)(s.sum / count);
    s.p95 = sorted[(count * 95u + 99u) / 100u - 1u];
    s.max = sorted[count - 1];
    s.min = sorted[0];
    return s;
}

static unsigned count_over(const uint32_t *values, unsigned count, double limit)
{
    unsigned over = 0;
    for (unsigned i = 0; i < count; ++i)
        over += values[i] > limit;
    return over;
}

static FILE *open_records(const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), MEDIA_DIR "/%s-%u.bin", name, run_number);
    return fopen(path, "wb");
}

static void put_be32(FILE *file, uint32_t value)
{
    unsigned char bytes[4] = {value >> 24, value >> 16, value >> 8, value};
    if (file == NULL || fwrite(bytes, 1, 4, file) != 4)
        log_failed = 1;
}

static void close_records(FILE *file)
{
    if (file == NULL || fclose(file) != 0)
        log_failed = 1;
}

/* ---- presentation: pack into the back XFB, then flip ----------------------------- */
static uint8_t *back_buffer(void)
{
    return xfb[xfb_index];
}

static void flip(void)
{
    DCStoreRange(xfb[xfb_index], XFB_BYTES);
    VIDEO_SetNextFramebuffer(MEM_K0_TO_K1(xfb[xfb_index]));
    VIDEO_Flush();
    xfb_index ^= 1u;
}

/* ---- job: decode and present as fast as possible (inputs fully in MEM2) --------- */
static int job_video(const char *codec, const char *name, const char *path, uint32_t expected)
{
    mem2_next = mem2_begin;
    size_t size;
    uint64_t io;
    uint8_t *data = load_file(path, &size, &io);
    int mpeg = strcmp(codec, "mpeg1") == 0;
    if (data == NULL || (!mpeg && strcmp(codec, "mjpeg") != 0)) {
        bench_log("VIDEO name=%s codec=%s result=fail reason=load\n", name, codec);
        return 0;
    }
    struct media_hmj hmj;
    struct media_mpeg *movie = NULL;
    double fps;
    struct mallinfo heap_before = mallinfo();
    media_heap_reset_peak();
    size_t decoder_base = media_heap.current;
    uint64_t setup_start = now();
    if (mpeg) {
        movie = media_mpeg_open_memory(data, size, 1, 0);
        if (movie == NULL || media_mpeg_width(movie) != (int)XFB_W || media_mpeg_height(movie) != (int)XFB_H) {
            bench_log("VIDEO name=%s codec=%s result=fail reason=open\n", name, codec);
            media_mpeg_close(movie);
            return 0;
        }
        fps = media_mpeg_framerate(movie);
    } else {
        if (!media_hmj_header(data, size, &hmj) || !media_hmj_check_index(data + MEDIA_HMJ_HEADER, &hmj, size) ||
            hmj.width != XFB_W || hmj.height != XFB_H) {
            bench_log("VIDEO name=%s codec=%s result=fail reason=container\n", name, codec);
            return 0;
        }
        fps = (double)hmj.fps_num / hmj.fps_den;
    }
    uint32_t setup_us = us(now() - setup_start);
    FILE *records = open_records(name);
    unsigned frames = 0;
    uint32_t sequence = 0, heap_inuse_peak = 0;
    int failed = 0;
    for (;;) {
        if (frames >= MAX_FRAMES) {
            failed = 1;
            break;
        }
        struct media_planes planes;
        uint8_t *rgb = NULL;
        double time;
        uint64_t start = now();
        if (mpeg) {
            if (!media_mpeg_video(movie, &planes, &time))
                break;
        } else {
            if (frames >= hmj.frames)
                break;
            uint32_t begin = media_hmj_offset(data + MEDIA_HMJ_HEADER, frames);
            uint32_t end = media_hmj_offset(data + MEDIA_HMJ_HEADER, frames + 1);
            int width, height;
            rgb = media_jpeg_decode(data + begin, end - begin, &width, &height);
            if (rgb == NULL || width != (int)XFB_W || height != (int)XFB_H) {
                failed = 1;
                media_jpeg_free(rgb);
                break;
            }
        }
        uint64_t decoded = now();
        uint8_t *shown = back_buffer();
        if (mpeg)
            media_pack_yuyv_from_420(shown, XFB_W * 2u, &planes);
        else
            media_pack_yuyv_from_rgb(shown, XFB_W * 2u, rgb, XFB_W, XFB_H);
        flip();
        uint64_t presented = now();
        struct mallinfo heap_now = mallinfo();
        if ((uint32_t)heap_now.uordblks > heap_inuse_peak)
            heap_inuse_peak = (uint32_t)heap_now.uordblks;
        if (rgb)
            media_jpeg_free(rgb);
        /* Checksums are outside the timed regions. */
        uint32_t crc = media_crc32(0, shown, XFB_BYTES);
        const unsigned char bytes[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
        sequence = media_crc32(sequence, bytes, 4);
        sample_a[frames] = us(decoded - start);
        sample_b[frames] = us(presented - decoded);
        sample_c[frames] = sample_a[frames] + sample_b[frames];
        put_be32(records, crc);
        put_be32(records, sample_a[frames]);
        put_be32(records, sample_b[frames]);
        put_be32(records, (uint32_t)media_heap.current);
        ++frames;
    }
    size_t decoder_peak = media_heap.peak - decoder_base;
    struct mallinfo heap_after = mallinfo();
    media_mpeg_close(movie);
    close_records(records);
    struct stats d = summarize(sample_a, frames), p = summarize(sample_b, frames), t = summarize(sample_c, frames);
    double period_us = 1e6 / fps;
    int pass = !failed && frames > 0 && sequence == expected;
    bench_log("VIDEO name=%s codec=%s frames=%u fps=%.5f file_bytes=%u load_us=%u setup_us=%u "
              "decode_us_mean=%u decode_us_p95=%u decode_us_max=%u decode_us_min=%u "
              "present_us_mean=%u present_us_p95=%u present_us_max=%u "
              "total_us_mean=%u total_us_p95=%u total_us_max=%u total_us_sum=%llu "
              "realtime_factor_mean=%.3f realtime_factor_p95=%.3f frames_over_period=%u "
              "decoder_heap_peak=%u heap_inuse_before=%d heap_inuse_peak=%u heap_inuse_after=%d heap_arena=%d "
              "mem2_input=%u xfb_bytes=%u sequence_crc=%08x expected=%08x result=%s\n",
              name, codec, frames, fps, (unsigned)size, us(io), setup_us, d.mean, d.p95, d.max, d.min, p.mean,
              p.p95, p.max, t.mean, t.p95, t.max, (unsigned long long)t.sum, t.mean ? period_us / t.mean : 0.0,
              t.p95 ? period_us / t.p95 : 0.0, count_over(sample_c, frames, period_us), (unsigned)decoder_peak,
              heap_before.uordblks, (unsigned)heap_inuse_peak, heap_after.uordblks, heap_after.arena,
              (unsigned)size, 2u * XFB_BYTES, (unsigned)sequence, (unsigned)expected, pass ? "pass" : "fail");
    return pass;
}

/* ---- job: MP2 audio decode only ---------------------------------------------------------- */
static int16_t stereo_frame[MEDIA_MP2_FRAME * 2];

static int job_mp2(const char *name, const char *path, uint32_t expected)
{
    mem2_next = mem2_begin;
    size_t size;
    uint64_t io;
    uint8_t *data = load_file(path, &size, &io);
    struct media_mpeg *movie = data ? media_mpeg_open_memory(data, size, 0, 1) : NULL;
    if (movie == NULL) {
        bench_log("MP2 name=%s result=fail reason=open\n", name);
        return 0;
    }
    int rate = media_mpeg_samplerate(movie);
    unsigned packets = 0, frames, total_frames = 0;
    uint64_t decode_ticks = 0;
    uint32_t crc = 0, worst = 0;
    double time;
    for (;;) {
        uint64_t start = now();
        int got = media_mpeg_audio(movie, stereo_frame, &frames, &time);
        uint64_t spent = now() - start;
        if (!got)
            break;
        decode_ticks += spent;
        if (us(spent) > worst)
            worst = us(spent);
        crc = media_crc32_s16be(crc, stereo_frame, frames * 2u);
        total_frames += frames;
        ++packets;
    }
    media_mpeg_close(movie);
    double seconds = rate > 0 ? (double)total_frames / rate : 0.0;
    uint32_t total_us = us(decode_ticks);
    int pass = packets > 0 && crc == expected;
    bench_log("MP2 name=%s rate=%d channels=2 audio_frames=%u packets=%u seconds=%.3f decode_us=%u "
              "us_per_audio_second=%.1f us_per_packet_mean=%.1f us_per_packet_max=%u cpu_share_at_realtime=%.4f "
              "crc=%08x expected=%08x result=%s\n",
              name, rate, total_frames, packets, seconds, total_us, seconds > 0 ? total_us / seconds : 0.0,
              packets ? (double)total_us / packets : 0.0, worst, seconds > 0 ? total_us / seconds / 1e6 : 0.0,
              (unsigned)crc, (unsigned)expected, pass ? "pass" : "fail");
    return pass;
}

/* ---- job: Xbox ADPCM decode from an HWS1 container ---------------------------------------- */
static int job_adpcm(const char *name, const char *path, uint32_t expected)
{
    mem2_next = mem2_begin;
    size_t size;
    uint64_t io;
    uint8_t *data = load_file(path, &size, &io);
    struct media_hws hws;
    if (data == NULL || !media_hws_header(data, size, &hws) || hws.codec != 1) {
        bench_log("ADPCM name=%s result=fail reason=container\n", name);
        return 0;
    }
    int16_t *pcm = (int16_t *)mem2_take((size_t)hws.frames * hws.channels * 2u);
    if (pcm == NULL) {
        bench_log("ADPCM name=%s result=fail reason=memory\n", name);
        return 0;
    }
    uint64_t start = now();
    long frames = media_xbox_adpcm_decode(data + MEDIA_HWS_HEADER, hws.payload, hws.channels, pcm);
    uint32_t decode_us = us(now() - start);
    uint32_t crc = frames > 0 ? media_crc32_s16be(0, pcm, (size_t)frames * hws.channels) : 0;
    double seconds = frames > 0 ? (double)frames / hws.rate : 0.0;
    int pass = frames >= 0 && (uint32_t)frames == hws.frames && crc == expected;
    bench_log("ADPCM name=%s rate=%u channels=%u frames=%ld seconds=%.3f payload=%u load_us=%u decode_us=%u "
              "us_per_audio_second=%.1f crc=%08x expected=%08x result=%s\n",
              name, (unsigned)hws.rate, (unsigned)hws.channels, frames, seconds, (unsigned)hws.payload, us(io),
              decode_us, seconds > 0 ? decode_us / seconds : 0.0, (unsigned)crc, (unsigned)expected,
              pass ? "pass" : "fail");
    return pass;
}

/* ---- job: real-time playback with ASND audio as the master clock ------------------------- */
static uint8_t audio_ring[AUDIO_BUFFERS][AUDIO_BYTES] __attribute__((aligned(32)));
static volatile unsigned audio_filled, audio_submitted, audio_underruns;
static volatile int audio_eof;

/* Called by ASND when the voice can queue another buffer; a call that finds
 * no filled buffer before the end of the audio counts as an underrun. */
static void audio_callback(s32 voice)
{
    if (audio_filled > audio_submitted) {
        if (ASND_AddVoice(voice, audio_ring[audio_submitted % AUDIO_BUFFERS], AUDIO_BYTES) == SND_OK)
            ++audio_submitted;
    } else if (!audio_eof) {
        ++audio_underruns;
    }
}

/* Buffers free for refilling: the voice holds at most the playing one and one queued. */
static int audio_slot_free(void)
{
    return audio_filled - audio_submitted < AUDIO_BUFFERS - 2u;
}

struct av_source {
    int mpeg;
    struct media_mpeg *movie;
    /* MJPEG */
    FILE *video, *audio;
    struct media_hmj hmj;
    uint8_t *table, *jpeg;
    uint32_t next_frame;
    /* shared */
    int16_t pending[MEDIA_MP2_FRAME * 2];
    unsigned pending_frames, pending_used;
    int audio_ended;
    uint64_t io_ticks;
    uint32_t io_bytes;
    double first_audio_time;
};

static void io_hook(uint64_t elapsed, size_t bytes, void *user)
{
    struct av_source *source = user;
    source->io_ticks += elapsed;
    source->io_bytes += (uint32_t)bytes;
}

/* Fill one ring buffer with stereo s16; returns 0 at the end of the audio. */
static int fill_audio(struct av_source *source, uint8_t *buffer)
{
    if (!source->mpeg) {
        uint64_t start = now();
        size_t got = fread(buffer, 1, AUDIO_BYTES, source->audio);
        source->io_ticks += now() - start;
        source->io_bytes += (uint32_t)got;
        if (got < AUDIO_BYTES)
            memset(buffer + got, 0, AUDIO_BYTES - got);
        return got > 0;
    }
    int16_t *out = (int16_t *)buffer;
    unsigned used = 0;
    while (used < AUDIO_FRAMES) {
        if (source->pending_used == source->pending_frames) {
            double time;
            if (source->audio_ended || !media_mpeg_audio(source->movie, source->pending, &source->pending_frames, &time)) {
                source->audio_ended = 1;
                break;
            }
            if (source->first_audio_time < 0)
                source->first_audio_time = time;
            source->pending_used = 0;
        }
        unsigned take = source->pending_frames - source->pending_used;
        if (take > AUDIO_FRAMES - used)
            take = AUDIO_FRAMES - used;
        memcpy(out + used * 2u, source->pending + source->pending_used * 2u, take * 4u);
        used += take;
        source->pending_used += take;
    }
    if (used < AUDIO_FRAMES)
        memset(out + used * 2u, 0, (AUDIO_FRAMES - used) * 4u);
    return used > 0;
}

static int next_video(struct av_source *source, uint8_t *dst, double *time, uint32_t *decode_us)
{
    if (source->mpeg) {
        struct media_planes planes;
        uint64_t start = now();
        if (!media_mpeg_video(source->movie, &planes, time))
            return 0;
        *decode_us = us(now() - start);
        media_pack_yuyv_from_420(dst, XFB_W * 2u, &planes);
        return 1;
    }
    if (source->next_frame >= source->hmj.frames)
        return 0;
    uint32_t begin = media_hmj_offset(source->table, source->next_frame);
    uint32_t end = media_hmj_offset(source->table, source->next_frame + 1);
    uint64_t start = now();
    if (fseek(source->video, (long)begin, SEEK_SET) != 0 || fread(source->jpeg, 1, end - begin, source->video) != end - begin)
        return -1;
    uint64_t read = now();
    source->io_ticks += read - start;
    source->io_bytes += end - begin;
    int width, height;
    uint8_t *rgb = media_jpeg_decode(source->jpeg, end - begin, &width, &height);
    if (rgb == NULL || width != (int)XFB_W || height != (int)XFB_H) {
        media_jpeg_free(rgb);
        return -1;
    }
    *decode_us = us(now() - read);
    media_pack_yuyv_from_rgb(dst, XFB_W * 2u, rgb, XFB_W, XFB_H);
    media_jpeg_free(rgb);
    *time = (double)source->next_frame * source->hmj.fps_den / source->hmj.fps_num;
    ++source->next_frame;
    return 1;
}

static void refill_audio(struct av_source *source)
{
    while (audio_slot_free() && !source->audio_ended) {
        if (!fill_audio(source, audio_ring[audio_filled % AUDIO_BUFFERS])) {
            source->audio_ended = 1;
            break;
        }
        ++audio_filled;
    }
    if (source->audio_ended)
        audio_eof = 1;
}

/* ASND advances a voice's tick counter once per mixing tick
 * (ASND_GetSamplesPerTick() output samples). The audio clock interpolates
 * between ticks with the timebase stamped by ASND's per-tick callback, and is
 * clamped to the tick's length, so it is continuous to the microsecond. */
static volatile u32 tick_base;
static volatile u64 tick_base_time;
static u32 samples_per_tick;

static void asnd_tick(void)
{
    tick_base = ASND_GetTickCounterVoice(0);
    tick_base_time = gettime();
}

/* Output samples played since the voice started, in microseconds; IRQ safe. */
static uint64_t voice_us(void)
{
    u32 level = IRQ_Disable();
    u32 base = tick_base;
    u64 stamped = tick_base_time;
    IRQ_Restore(level);
    uint64_t since = ticks_to_microsecs(gettime() - stamped);
    uint64_t limit = (uint64_t)samples_per_tick * 1000000u / DSP_RATE;
    if (since > limit)
        since = limit;
    return (uint64_t)base * 1000000u / DSP_RATE + since;
}

static double audio_clock(const struct av_source *source)
{
    return source->first_audio_time + (double)voice_us() / 1e6;
}

/* Presentation queue: XFB slots are free, queued (decoded ahead), pending
 * (flipped, waiting for the next retrace) or visible. The post-retrace
 * callback promotes the pending slot and stamps the audio clock and retrace
 * count at the moment the frame becomes visible. */
enum { SLOT_FREE, SLOT_QUEUED, SLOT_PENDING, SLOT_VISIBLE };
static volatile unsigned slot_state[XFB_SLOTS];
static volatile int pending_slot = -1, visible_slot = -1;
static volatile unsigned pending_frame;
static volatile int stamping;
static uint64_t stamp_us[MAX_FRAMES];
static uint32_t stamp_retrace[MAX_FRAMES];
static uint8_t frame_flags[MAX_FRAMES];
static double frame_time[MAX_FRAMES];

static void present_on_retrace(void)
{
    if (!stamping || pending_slot < 0)
        return;
    if (visible_slot >= 0)
        slot_state[visible_slot] = SLOT_FREE;
    visible_slot = pending_slot;
    slot_state[visible_slot] = SLOT_VISIBLE;
    pending_slot = -1;
    stamp_us[pending_frame] = voice_us();
    stamp_retrace[pending_frame] = retraces;
}

struct av_queue {
    unsigned slot[XFB_SLOTS], frame[XFB_SLOTS], head, count, max_count, decoded;
    int video_ended, failed;
};

/* Decode the next frame into a free XFB slot; 0 when nothing was decoded. */
static int decode_ahead(struct av_source *source, struct av_queue *queue, unsigned limit)
{
    int free_slot = -1;
    for (unsigned i = 0; i < XFB_SLOTS; ++i)
        if (slot_state[i] == SLOT_FREE) {
            free_slot = (int)i;
            break;
        }
    if (free_slot < 0 || queue->video_ended || (limit && queue->decoded >= limit) || queue->decoded >= MAX_FRAMES)
        return 0;
    double time;
    uint32_t decode_us;
    int got = next_video(source, xfb[free_slot], &time, &decode_us);
    if (got <= 0) {
        queue->video_ended = 1;
        queue->failed |= got < 0;
        return 0;
    }
    DCStoreRange(xfb[free_slot], XFB_BYTES);
    slot_state[free_slot] = SLOT_QUEUED;
    unsigned tail = (queue->head + queue->count) % XFB_SLOTS;
    queue->slot[tail] = (unsigned)free_slot;
    queue->frame[tail] = queue->decoded;
    ++queue->count;
    if (queue->count > queue->max_count)
        queue->max_count = queue->count;
    sample_a[queue->decoded] = decode_us;
    frame_time[queue->decoded] = time;
    frame_flags[queue->decoded] = 0;
    ++queue->decoded;
    return 1;
}

static int job_av(const char *codec, const char *name, const char *path, const char *audio_path, unsigned limit)
{
    struct av_source source;
    memset(&source, 0, sizeof(source));
    source.mpeg = strcmp(codec, "mpeg1") == 0;
    source.first_audio_time = source.mpeg ? -1.0 : 0.0;
    double fps;
    uint32_t rate;
    mem2_next = mem2_begin;
    media_heap_reset_peak();
    size_t decoder_base = media_heap.current;
    if (source.mpeg) {
        source.movie = media_mpeg_open_file(path, 1, 1, now, io_hook, &source);
        if (source.movie == NULL) {
            bench_log("AV name=%s codec=%s result=fail reason=open\n", name, codec);
            return 0;
        }
        fps = media_mpeg_framerate(source.movie);
        rate = (uint32_t)media_mpeg_samplerate(source.movie);
    } else {
        unsigned char header[MEDIA_HMJ_HEADER];
        source.video = fopen(path, "rb");
        source.audio = fopen(audio_path, "rb");
        struct stat info;
        if (source.video == NULL || source.audio == NULL || stat(path, &info) != 0 ||
            fread(header, 1, sizeof(header), source.video) != sizeof(header) ||
            !media_hmj_header(header, (size_t)info.st_size, &source.hmj) ||
            (source.table = mem2_take(4u * (source.hmj.frames + 1u))) == NULL ||
            fread(source.table, 4, source.hmj.frames + 1u, source.video) != source.hmj.frames + 1u ||
            !media_hmj_check_index(source.table, &source.hmj, (size_t)info.st_size) ||
            (source.jpeg = mem2_take(1u << 20)) == NULL) {
            bench_log("AV name=%s codec=%s result=fail reason=open\n", name, codec);
            if (source.video)
                fclose(source.video);
            if (source.audio)
                fclose(source.audio);
            return 0;
        }
        fps = (double)source.hmj.fps_num / source.hmj.fps_den;
        rate = source.hmj.audio_rate;
    }
    double period = 1.0 / fps, field = 1001.0 / 60000.0;
    for (unsigned i = 0; i < XFB_SLOTS; ++i)
        slot_state[i] = SLOT_FREE;
    /* The slot on screen now stays visible until the first flip. */
    unsigned on_screen = xfb_index ^ 1u;
    slot_state[on_screen] = SLOT_VISIBLE;
    visible_slot = (int)on_screen;
    pending_slot = -1;
    struct av_queue queue;
    memset(&queue, 0, sizeof(queue));
    unsigned presented = 0, dropped = 0, waits = 0;
    int stalled = 0;
    uint64_t idle_ticks = 0;

    /* Pre-roll: fill the audio ring and the frame queue. */
    audio_filled = audio_submitted = audio_underruns = 0;
    audio_eof = 0;
    refill_audio(&source);
    while (decode_ahead(&source, &queue, limit))
        ;
    if (queue.count == 0)
        queue.failed = 1;
    FILE *records = open_records(name);
    ASND_Pause(0);
    audio_submitted = 1;
    stamping = 1;
    tick_base = 0;
    tick_base_time = gettime();
    ASND_SetCallback(asnd_tick);
    ASND_SetVoice(0, VOICE_STEREO_16BIT, (s32)rate, 0, audio_ring[0], AUDIO_BYTES, 255, 255, audio_callback);
    uint64_t started = now();
    u32 retrace_start = retraces;
    while (!queue.failed && (queue.count > 0 || !queue.video_ended)) {
        refill_audio(&source);
        if (queue.count > 0 && pending_slot < 0) {
            unsigned slot = queue.slot[queue.head], frame = queue.frame[queue.head];
            double clock = audio_clock(&source), time = frame_time[frame];
            if (clock > time + period) {
                /* more than a frame late: skip its display to catch up */
                slot_state[slot] = SLOT_FREE;
                frame_flags[frame] = 1;
                ++dropped;
                queue.head = (queue.head + 1) % XFB_SLOTS;
                --queue.count;
                continue;
            }
            if (clock + field >= time) {
                pending_frame = frame;
                slot_state[slot] = SLOT_PENDING;
                pending_slot = (int)slot;
                VIDEO_SetNextFramebuffer(MEM_K0_TO_K1(xfb[slot]));
                VIDEO_Flush();
                ++presented;
                queue.head = (queue.head + 1) % XFB_SLOTS;
                --queue.count;
                waits = 0;
                continue;
            }
        }
        if (decode_ahead(&source, &queue, limit))
            continue;
        uint64_t idle = now();
        VIDEO_WaitVSync();
        idle_ticks += now() - idle;
        if (++waits > 600u) { /* ten seconds without progress */
            stalled = 1;
            break;
        }
    }
    /* Let the last flip become visible. */
    for (unsigned i = 0; i < 3 && pending_slot >= 0; ++i)
        VIDEO_WaitVSync();
    stamping = 0;
    double elapsed = (double)us(now() - started) / 1e6;
    double clock_end = audio_clock(&source);
    unsigned underruns = audio_underruns;
    ASND_StopVoice(0);
    ASND_SetCallback(NULL);
    ASND_Pause(1);
    size_t decoder_peak = media_heap.peak - decoder_base;
    if (source.mpeg)
        media_mpeg_close(source.movie);
    else {
        fclose(source.video);
        fclose(source.audio);
    }
    /* Keep the bench's double buffering valid: draw next into the slot not on screen. */
    xfb_index = visible_slot == 0 ? 1u : 0u;
    double drift_sum = 0.0, drift_min = 1e9, drift_max = -1e9, drift_first = 0.0, drift_last = 0.0;
    double vi_first = 0.0, vi_last = 0.0;
    unsigned stamped = 0, late = 0;
    for (unsigned f = 0; f < queue.decoded; ++f) {
        double drift = 0.0, vi = 0.0;
        if (!frame_flags[f]) {
            double shown = source.first_audio_time + (double)stamp_us[f] / 1e6;
            drift = shown - frame_time[f];
            vi = (double)(stamp_retrace[f] - retrace_start) * field - (shown - source.first_audio_time);
            if (stamped == 0) {
                drift_first = drift;
                vi_first = vi;
            }
            drift_last = drift;
            vi_last = vi;
            drift_sum += drift;
            drift_min = drift < drift_min ? drift : drift_min;
            drift_max = drift > drift_max ? drift : drift_max;
            late += drift > field;
            ++stamped;
        }
        put_be32(records, f);
        put_be32(records, (uint32_t)(frame_time[f] * 1e6 + 0.5));
        put_be32(records, frame_flags[f]);
        put_be32(records, (uint32_t)(int32_t)(drift * 1e6));
        put_be32(records, (uint32_t)(int32_t)(vi * 1e6));
        put_be32(records, sample_a[f]);
    }
    close_records(records);
    struct stats d = summarize(sample_a, queue.decoded);
    int pass = !queue.failed && !stalled && stamped == presented && presented > 0 &&
               queue.decoded == presented + dropped;
    bench_log("AV name=%s codec=%s fps=%.5f audio_rate=%u frames_decoded=%u presented=%u dropped=%u late=%u "
              "xfb_slots=%u max_queued=%u drift_ms_mean=%.3f drift_ms_min=%.3f drift_ms_max=%.3f "
              "drift_ms_first=%.3f drift_ms_last=%.3f vi_minus_audio_ms_first=%.3f vi_minus_audio_ms_last=%.3f "
              "first_audio_time_ms=%.3f elapsed_s=%.3f audio_clock_end_s=%.3f audio_buffers_submitted=%u "
              "underruns=%u idle_us=%u decode_us_mean=%u decode_us_p95=%u decode_us_max=%u io_us=%u io_bytes=%u "
              "decoder_heap_peak=%u stalled=%d samples_per_tick=%u result=%s\n",
              name, codec, fps, (unsigned)rate, queue.decoded, presented, dropped, late, XFB_SLOTS,
              queue.max_count, stamped ? drift_sum / stamped * 1e3 : 0.0, drift_min * 1e3, drift_max * 1e3,
              drift_first * 1e3, drift_last * 1e3, vi_first * 1e3, vi_last * 1e3, source.first_audio_time * 1e3,
              elapsed, clock_end, audio_submitted, underruns, us(idle_ticks), d.mean, d.p95, d.max,
              us(source.io_ticks), (unsigned)source.io_bytes, (unsigned)decoder_peak, stalled,
              (unsigned)samples_per_tick, pass ? "pass" : "fail");
    return pass;
}

/* ---- storage, jobs and main ------------------------------------------------------------------ */
static int read_runs(unsigned *previous)
{
    char text[64];
    *previous = 0;
    FILE *file = fopen(RUNS_PATH, "rb");
    if (file == NULL)
        return errno == ENOENT;
    size_t used = fread(text, 1, sizeof(text) - 1, file);
    text[used] = 0;
    int ok = !ferror(file) && fclose(file) == 0;
    unsigned value;
    char tail;
    if (!ok || strncmp(text, RUNS_PREFIX, strlen(RUNS_PREFIX)) != 0 ||
        sscanf(text + strlen(RUNS_PREFIX), "%u%c", &value, &tail) != 2 || tail != '\n')
        return 0;
    *previous = value;
    return 1;
}

static int init_storage(void)
{
    unsigned previous;
    if (!fatInitDefault())
        return 0;
    if (mkdir(MEDIA_DIR, 0777) != 0 && errno != EEXIST)
        return 0;
    if (!read_runs(&previous))
        return 0;
    FILE *file = fopen(RUNS_PATH, "w");
    if (file == NULL)
        return 0;
    int written = fprintf(file, RUNS_PREFIX "%u\n", previous + 1);
    if (fclose(file) != 0 || written < 0)
        return 0;
    unsigned readback;
    if (!read_runs(&readback) || readback != previous + 1)
        return 0;
    run_number = readback;
    record = fopen(LOG_PATH, "a");
    return record != NULL;
}

static char jobs_text[4096];

static int run_jobs(unsigned *passed, unsigned *total)
{
    FILE *file = fopen(JOBS_PATH, "rb");
    if (file == NULL)
        return 0;
    size_t used = fread(jobs_text, 1, sizeof(jobs_text) - 1, file);
    int complete = !ferror(file) && fgetc(file) == EOF;
    if (fclose(file) != 0 || !complete)
        return 0;
    jobs_text[used] = 0;
    for (char *line = jobs_text; *line;) {
        char *end = line + strcspn(line, "\r\n");
        char *next = end + strspn(end, "\r\n");
        *end = 0;
        if (*line == 0) {
            line = next;
            continue;
        }
        char kind[16], a[32], b[48], c[96], d[96], e[16], tail;
        int fields = sscanf(line, "%15s %31s %47s %95s %95s %15s %c", kind, a, b, c, d, e, &tail);
        int ok = 0;
        if (*total >= MAX_JOBS)
            return 0;
        if (strcmp(kind, "video") == 0 && fields == 5)
            ok = job_video(a, b, c, (uint32_t)strtoul(d, NULL, 16));
        else if (strcmp(kind, "mp2") == 0 && fields == 4)
            ok = job_mp2(a, b, (uint32_t)strtoul(c, NULL, 16));
        else if (strcmp(kind, "adpcm") == 0 && fields == 4)
            ok = job_adpcm(a, b, (uint32_t)strtoul(c, NULL, 16));
        else if (strcmp(kind, "av") == 0 && fields == 6)
            ok = job_av(a, b, c, d, (unsigned)strtoul(e, NULL, 10));
        else
            bench_log("JOB line_rejected fields=%d\n", fields);
        *passed += ok != 0;
        ++*total;
        line = next;
    }
    return 1;
}

int main(void)
{
    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    for (unsigned i = 0; i < XFB_SLOTS; ++i)
        xfb[i] = SYS_AllocateFramebuffer(mode);
    VIDEO_Configure(mode);
    VIDEO_SetPostRetraceCallback(count_retrace);
    for (unsigned i = 0; i < XFB_SLOTS; ++i) {
        uint32_t *words = (uint32_t *)xfb[i];
        for (unsigned w = 0; w < XFB_BYTES / 4u; ++w)
            words[w] = 0x10801080u; /* black */
        DCStoreRange(xfb[i], XFB_BYTES);
    }
    VIDEO_SetNextFramebuffer(MEM_K0_TO_K1(xfb[0]));
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    xfb_index = 1;
    mem2_init();
    ASND_Init();
    samples_per_tick = ASND_GetSamplesPerTick();
    ASND_Pause(1);

    int storage = init_storage();
    unsigned passed = 0, total = 0;
    int jobs_ok = 0;
    if (storage) {
        bench_log("BEGIN target=media_bench build=%s run=%u fb=%ux%u mem2_free=%u mode_tv=%u\n",
                  WII_BUILD_ID, run_number, (unsigned)mode->fbWidth, (unsigned)mode->xfbHeight,
                  (unsigned)(mem2_end - mem2_begin), (unsigned)mode->viTVMode);
        jobs_ok = mode->fbWidth == XFB_W && mode->xfbHeight == XFB_H && run_jobs(&passed, &total);
    }
    struct mallinfo heap = mallinfo();
    int success = storage && jobs_ok && total > 0 && passed == total && !log_failed;
    bench_log("END target=media_bench build=%s run=%u jobs=%u passed=%u heap_inuse_end=%d decoder_heap_end=%u "
              "result=%s\n",
              WII_BUILD_ID, run_number, total, passed, heap.uordblks, (unsigned)media_heap.current,
              success ? "pass" : "fail");
    if (record != NULL && (fclose(record) != 0 || log_failed))
        success = 0;
    ASND_End();
    return success ? 0 : 1;
}
