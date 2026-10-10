/* Host reference for the HWI-007 game-state ownership prototype (game_state_image.c),
 * built for the upstream i686 ABI. Runs the guest's cycle sequence: build the full
 * census state at alternating placements, restore the previous image, update through
 * the engine, save, and the image fault cases; prints the canonical STATE lines.
 * Usage: game_state_host <work directory> [cycles] */
#include "game_state_image.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_BYTES ((size_t)0x1400000) /* HALO_PORT_GAME_STATE_SIZE */
#define STATE_CPU ((size_t)0x13C0000)
#define STATE_GPU ((size_t)0x40000)

static char path_buffers[2][512];
static unsigned path_next;

static const char *path(const char *directory, const char *name)
{
    char *buffer = path_buffers[path_next++ % 2];
    snprintf(buffer, sizeof(path_buffers[0]), "%s/%s", directory, name);
    return buffer;
}

static int copy_with_fault(const char *from, const char *to, long truncate, long flip_at)
{
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    long offset = 0;
    int c;
    if (!in || !out)
        return 0;
    while ((c = fgetc(in)) != EOF) {
        if (truncate >= 0 && offset >= truncate)
            break;
        if (offset == flip_at)
            c ^= 0x01;
        fputc(c, out);
        ++offset;
    }
    fclose(in);
    return fclose(out) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s work_directory [cycles]\n", argv[0]);
        return 2;
    }
    const char *work = argv[1];
    unsigned cycles = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 4;
    unsigned char *storage = malloc(STATE_BYTES + 8192 + 32);
    static struct gs_region region;
    struct gs_result result;
    struct gs_report saved, restored;
    char text[512];
    int ok = storage != NULL, previous = 0;
    uint32_t previous_crc = 0;
    printf("BEGIN host pointer_bits=%u state_bytes=%lu census_cpu=%lu census_gpu=%lu\n",
           (unsigned)(sizeof(void *) * 8), (unsigned long)STATE_BYTES, gs_census_cpu_requested, gs_census_gpu_requested);
    {
        uint16_t seed, native;
        gs_identifier_seeds("object", &seed, &native);
        printf("SEEDS object=%04x native_copy=%04x", seed, native);
        gs_identifier_seeds("players", &seed, &native);
        printf(" players=%04x native_copy=%04x", seed, native);
        unsigned differ = 0, arrays = 0;
        for (unsigned row = 0; row < gs_census_row_count; ++row)
            if (gs_census_rows[row].kind == GS_DATA) {
                gs_identifier_seeds(gs_census_rows[row].name, &seed, &native);
                ++arrays;
                differ += seed != native;
            }
        printf(" arrays=%u native_copy_differs=%u\n", arrays, differ);
    }
    for (unsigned cycle = 0; ok && cycle < cycles; ++cycle) {
        unsigned char *base = storage + ((32 - (uintptr_t)storage % 32) % 32) + (cycle & 1) * 4096;
        ok = gs_build(&region, base, STATE_BYTES, STATE_CPU, STATE_GPU, &result);
        if (!ok) {
            printf("FAIL build cycle=%u %s %zu\n", cycle, gs_error_name(result.error), result.offset);
            break;
        }
        printf("BUILD cycle=%u placement=%u allocations=%u cpu_used=%lu gpu_used=%lu cpu_free=%lu\n", cycle, cycle & 1,
               region.count, (unsigned long)region.cpu_used, (unsigned long)region.gpu_used,
               (unsigned long)(region.cpu_size - region.cpu_used));
        if (previous) {
            FILE *in = fopen(path(work, "state.bin"), "rb");
            ok = in && gs_restore(&region, in, &result);
            if (in)
                fclose(in);
            if (!ok || !gs_save(&region, NULL, &restored, &result) || restored.image_crc != previous_crc) {
                printf("FAIL restore cycle=%u %s %zu\n", cycle, gs_error_name(result.error), result.offset);
                ok = 0;
                break;
            }
            printf("RESTORE cycle=%u step=%u image_crc=%08x same=1\n", cycle, restored.step, restored.image_crc);
        }
        if (!gs_update(&region, &result)) {
            printf("FAIL update cycle=%u %s %zu\n", cycle, gs_error_name(result.error), result.offset);
            ok = 0;
            break;
        }
        FILE *out = fopen(path(work, "state.bin"), "wb");
        ok = out && gs_save(&region, out, &saved, &result);
        if (out && fclose(out) != 0)
            ok = 0;
        if (!ok) {
            printf("FAIL save cycle=%u\n", cycle);
            break;
        }
        gs_describe(&saved, text, sizeof(text));
        printf("CYCLE %u %s", cycle, text);
        previous = 1;
        previous_crc = saved.image_crc;
    }
    /* damaged images: truncated, one flipped payload bit, and a count past its capacity */
    if (ok) {
        const struct { const char *name; long truncate, flip; } faults[] = {
            {"truncated", 100, -1}, {"crc_flip", -1, 4000}, {"empty", 0, -1}};
        for (unsigned index = 0; index < sizeof(faults) / sizeof(faults[0]); ++index) {
            unsigned char *base = storage + ((32 - (uintptr_t)storage % 32) % 32);
            copy_with_fault(path(work, "state.bin"), path(work, "fault.bin"), faults[index].truncate,
                            faults[index].flip);
            int built = gs_build(&region, base, STATE_BYTES, STATE_CPU, STATE_GPU, &result);
            FILE *in = fopen(path(work, "fault.bin"), "rb");
            int taken = built && in && gs_restore(&region, in, &result);
            if (in)
                fclose(in);
            printf("FAULT %s rejected=%d reason=%s live=%d\n", faults[index].name, !taken,
                   gs_error_name(result.error), region.live);
            ok = ok && built && !taken && !region.live;
        }
    }
    printf("END result=%s\n", ok ? "pass" : "fail");
    free(storage);
    return ok ? 0 : 1;
}
