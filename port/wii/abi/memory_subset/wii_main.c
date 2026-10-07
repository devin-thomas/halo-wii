/* Dedicated asset-free diagnostic; no production engine boundary changes. */
#include <gccore.h>
#include <fat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "fixture.h"
#include "subset_build_id.h"

int main(void)
{
    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL) {
        fprintf(stderr, "MEMORY video mode unavailable\n");
        return 2;
    }
    void *framebuffer = SYS_AllocateFramebuffer(mode);
    if (framebuffer == NULL) {
        fprintf(stderr, "MEMORY framebuffer allocation failed\n");
        return 2;
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
    printf("Asset-free scalar engine subset\nBuild: %s\n", WII_MEMORY_BUILD_ID);
    if (!fatInitDefault()) {
        fprintf(stderr, "MEMORY SD unavailable; no persisted result\n");
        return 2;
    }
    if (mkdir("sd:/halo-wii-memory", 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "MEMORY directory failed: %s\n", strerror(errno));
        return 2;
    }
    FILE *report = fopen("sd:/halo-wii-memory/subset.log", "a");
    if (report == NULL) {
        fprintf(stderr, "MEMORY log open failed: %s\n", strerror(errno));
        return 2;
    }
    if (fprintf(report, "BEGIN MEMORY build=%s\n", WII_MEMORY_BUILD_ID) < 0 ||
        fflush(report) != 0) {
        fclose(report);
        return 2;
    }
    /* Continue independent valid-input checks to expose all endian mismatches. */
    int result = wii_memory_subset(report, 1);
    int write_result = fprintf(report, "END MEMORY build=%s result=%d\n", WII_MEMORY_BUILD_ID, result);
    int flush_result = fflush(report);
    int close_result = fclose(report);
    if (write_result < 0 || flush_result != 0 || close_result != 0) {
        fprintf(stderr, "MEMORY log completion failed\n");
        return 2;
    }
    printf("Subset result: %d; SD report complete\n", result);
    for (unsigned int i = 0; i < 60; ++i) VIDEO_WaitVSync();
    return result;
}
