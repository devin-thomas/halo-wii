/* Asset-free diagnostic. Native libogc only; this is not a Halo game runtime. */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "build_id.h"

_Static_assert(sizeof(void *) == 4, "The probe requires PPC32 pointers");
_Static_assert(sizeof(int) == 4, "The probe requires 32-bit int");

static FILE *record;
static unsigned int previous_runs;
static int log_failed;

static int init_record(void)
{
    FILE *sentinel;
    if (!fatInitDefault()) {
        printf("SD unavailable: persistence and file logging UNTESTED\n");
        return 0;
    }
    if (mkdir("sd:/halo-wii-probe", 0777) != 0 && errno != EEXIST) {
        printf("SD directory failed: %s\n", strerror(errno));
        return -1;
    }
    sentinel = fopen("sd:/halo-wii-probe/sentinel.txt", "r");
    if (sentinel != NULL) {
        char line[64];
        char *end;
        const char prefix[] = "halo-wii-probe-v1 ";
        int valid = fgets(line, sizeof(line), sentinel) != NULL;
        unsigned long count = 0;
        if (valid) {
            valid = strncmp(line, prefix, sizeof(prefix) - 1) == 0;
            char *digits = line + sizeof(prefix) - 1;
            if (valid && *digits >= '0' && *digits <= '9') {
                errno = 0;
                count = strtoul(digits, &end, 10);
                valid = errno == 0 && count < UINT32_MAX && strcmp(end, "\n") == 0;
            } else valid = 0;
        }
        if (fgetc(sentinel) != EOF || ferror(sentinel)) valid = 0;
        int close_result = fclose(sentinel);
        if (!valid || close_result != 0) {
            printf("Invalid sentinel; preserved without overwrite\n");
            return -1;
        }
        previous_runs = count;
    } else if (errno != ENOENT) {
        printf("Sentinel read failed: %s\n", strerror(errno));
        return -1;
    }
    sentinel = fopen("sd:/halo-wii-probe/sentinel.txt", "w");
    if (sentinel == NULL) {
        printf("Sentinel open failed: %s\n", strerror(errno));
        return -1;
    }
    int write_result = fprintf(sentinel, "halo-wii-probe-v1 %u\n", previous_runs + 1);
    int close_result = fclose(sentinel);
    if (write_result < 0 || close_result != 0) {
        printf("Sentinel write failed\n");
        return -1;
    }
    record = fopen("sd:/halo-wii-probe/probe.log", "a");
    if (record == NULL) {
        printf("Log open failed: %s\n", strerror(errno));
        return -1;
    }
    return 1;
}

int main(void)
{
    GXRModeObj *mode;
    void *framebuffer;
    uint64_t started, elapsed_us;
    unsigned int frames = 0, connected = 0, activity = 0;
    uint64_t ticks = 0;
    int storage;
    const char *exit_reason = "system_event";
    VIDEO_Init();
    PAD_Init();
    WPAD_Init();
    mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL) {
        fprintf(stderr, "VIDEO_GetPreferredMode failed\n");
        return 1;
    }
    framebuffer = SYS_AllocateFramebuffer(mode);
    if (framebuffer == NULL) {
        fprintf(stderr, "Framebuffer allocation failed\n");
        return 1;
    }
    framebuffer = MEM_K0_TO_K1(framebuffer);
    console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight,
                 mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(framebuffer);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    printf("Halo Wii asset-free probe\nBuild: %s\n", WII_BUILD_ID);
    printf("Video: %ux%u mode=%u\n", mode->fbWidth, mode->xfbHeight, mode->viTVMode);
    printf("MEM1 arena: %p..%p (%u bytes)\n", SYS_GetArena1Lo(), SYS_GetArena1Hi(), SYS_GetArena1Size());
    printf("MEM2 arena: %p..%p (%u bytes)\n", SYS_GetArena2Lo(), SYS_GetArena2Hi(), SYS_GetArena2Size());
    printf("Arena bounds are not a free-heap/peak-memory measurement.\n");
    storage = init_record();
    printf("SD status=%d previous probe runs=%u\n", storage, previous_runs);
    printf("PAD START / Remote HOME: exit. Auto-exit frames=%u\n", WII_PROBE_AUTO_EXIT_FRAMES);
    if (record != NULL) {
        int begin_result = fprintf(record, "BEGIN build=%s previous_runs=%u video=%ux%u mode=%u\n",
                WII_BUILD_ID, previous_runs, mode->fbWidth, mode->xfbHeight, mode->viTVMode);
        int arena_result = fprintf(record, "ARENAS mem1=%p..%p bytes=%u mem2=%p..%p bytes=%u\n",
                SYS_GetArena1Lo(), SYS_GetArena1Hi(), SYS_GetArena1Size(),
                SYS_GetArena2Lo(), SYS_GetArena2Hi(), SYS_GetArena2Size());
        int flush_result = fflush(record);
        if (begin_result < 0 || arena_result < 0 || flush_result != 0) {
            log_failed = 1;
            printf("Log header/flush failed\n");
        }
    }
    started = gettime();
    while (SYS_MainLoop()) {
        unsigned int present = PAD_ScanPads();
        WPAD_ScanPads();
        connected |= present;
        elapsed_us = ticks_to_microsecs(gettime() - started);
        ticks = elapsed_us * 30 / 1000000;
        if (frames % 30 == 0) {
            printf("\x1b[12;0Hframe=%u elapsed_us=%" PRIu64 " 30Hz ticks=%" PRIu64 "     \n",
                   frames, elapsed_us, ticks);
            for (int slot = 0; slot < 4; ++slot) {
                printf("PAD%d present=%u held=%04x stick=%d,%d trigger=%u,%u      \n",
                       slot, (present >> slot) & 1, PAD_ButtonsHeld(slot),
                       PAD_StickX(slot), PAD_StickY(slot), PAD_TriggerL(slot), PAD_TriggerR(slot));
            }
        }
        for (int slot = 0; slot < 4; ++slot) {
            if (PAD_ButtonsHeld(slot) || PAD_StickX(slot) || PAD_StickY(slot) ||
                PAD_TriggerL(slot) || PAD_TriggerR(slot)) activity |= 1U << slot;
            if (PAD_ButtonsDown(slot) & PAD_BUTTON_START) exit_reason = "pad_start";
            if (WPAD_ButtonsDown(slot) & WPAD_BUTTON_HOME) exit_reason = "remote_home";
        }
        if (strcmp(exit_reason, "system_event") != 0) break;
#if WII_PROBE_AUTO_EXIT_FRAMES > 0
        if (frames >= WII_PROBE_AUTO_EXIT_FRAMES) {
            exit_reason = "auto_exit";
            break;
        }
#endif
        ++frames;
        VIDEO_WaitVSync();
    }
    printf("\nEXIT %s; no gameplay/hardware qualification implied\n", exit_reason);
    if (record != NULL) {
        int write_result = fprintf(record, "END build=%s frames=%u ticks=%" PRIu64
                                   " connected=%x activity=%x exit=%s storage=%d\n",
                                   WII_BUILD_ID, frames, ticks, connected, activity, exit_reason, storage);
        int close_result = fclose(record);
        if (write_result < 0 || close_result != 0 || log_failed) {
            printf("Log write/close failed\n");
            return 1;
        }
    }
    return storage < 0 ? 1 : 0;
}
