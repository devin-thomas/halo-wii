#include "cache_address_fixture.h"
#include "cache_address_owned.h"
#include "cache_address_probe.h"
#include "cache_probe_payload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CACHE_MATERIAL_FIXTURE
#include "cache_material_fixture.h"
#define CACHE_ADDRESS_REPORT_KIND "MATERIAL_SYNTHETIC"
#define CACHE_ADDRESS_REPORT_PATH "sd:/halo-wii-memory/cache-material-synthetic.log"
#elif defined(CACHE_BSP_FIXTURE)
#include "cache_bsp_fixture.h"
#define CACHE_ADDRESS_REPORT_KIND "BSP_SYNTHETIC"
#define CACHE_ADDRESS_REPORT_PATH "sd:/halo-wii-memory/cache-bsp-synthetic.log"
#elif defined(CACHE_WIDGET_FIXTURE)
#include "cache_widget_fixture.h"
#define CACHE_ADDRESS_REPORT_KIND "WIDGET_SYNTHETIC"
#define CACHE_ADDRESS_REPORT_PATH "sd:/halo-wii-memory/cache-widget-synthetic.log"
#else
#define CACHE_ADDRESS_REPORT_KIND "CACHE"
#define CACHE_ADDRESS_REPORT_PATH "sd:/halo-wii-memory/cache-address.log"
#endif
#ifdef GEKKO
#include <gccore.h>
#include <fat.h>
#include <errno.h>
#include <sys/stat.h>
#include <malloc.h>
#include "../../port/linux/include/halo_port_capacity.h"

static void arenas(FILE *report, const char *stage)
{
    fprintf(report, "ARENAS %s mem1_lo=%p mem1_hi=%p mem1_bytes=%lu mem2_lo=%p mem2_hi=%p mem2_bytes=%lu\n",
            stage, SYS_GetArena1Lo(), SYS_GetArena1Hi(), (unsigned long)SYS_GetArena1Size(),
            SYS_GetArena2Lo(), SYS_GetArena2Hi(), (unsigned long)SYS_GetArena2Size());
}

#if CACHE_PRIVATE_SIZE > 0
static int mem2_copy(FILE *report)
{
    uintptr_t old_lo = (uintptr_t)SYS_GetArena2Lo();
    uintptr_t start = (old_lo + 31u) & ~(uintptr_t)31u;
    uintptr_t hi = (uintptr_t)SYS_GetArena2Hi();
    size_t aligned = (CACHE_PRIVATE_SIZE + 31u) & ~(size_t)31u;
    if (start > hi || aligned > hi - start) {
        fprintf(report, "MEM2 FAIL bounded private copy does not fit\n");
        return 1;
    }
    SYS_SetArena2Lo((void *)(start + aligned));
    unsigned char *copy = (unsigned char *)start;
    memcpy(copy, cache_private_bytes, CACHE_PRIVATE_SIZE);
    arenas(report, "copy_live");
    struct cache_address_region region = {copy, CACHE_PRIVATE_SIZE, UINT32_C(0x803a6000)};
    struct cache_address_header header = {0};
    struct cache_address_result status = {0};
    struct cache_address_span table = {0};
    int result = !cache_address_decode_header(&region, &header, &status);
    if (!result) result = !cache_address_resolve(&region, header.instances_address, header.tag_count, 32, &table, &status);
    if (!result) result = header.tag_count != CACHE_PRIVATE_COUNT ||
                         cache_probe_crc32(copy, CACHE_PRIVATE_SIZE) != CACHE_PRIVATE_CRC ||
                         cache_probe_crc32(copy + table.offset, table.length) != CACHE_PRIVATE_TABLE_CRC;
    fprintf(report, "MEM2 COPY storage=%p bytes=%lu alignment_charge=%lu failures=%d\n", (void *)copy,
            (unsigned long)CACHE_PRIVATE_SIZE, (unsigned long)(start + aligned - old_lo), result);
    if ((uintptr_t)SYS_GetArena2Lo() != start + aligned || (uintptr_t)SYS_GetArena2Hi() != hi) {
        fprintf(report, "MEM2 FAIL concurrent arena mutation; refusing unsafe restore\n");
        return 1;
    }
    SYS_SetArena2Lo((void *)old_lo);
    arenas(report, "copy_released");
    return result;
}
#endif
#endif

int main(void)
{
    FILE *report = stdout;
#ifdef GEKKO
    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL) { fprintf(stderr, "CACHE video mode unavailable\n"); return 2; }
    void *framebuffer = SYS_AllocateFramebuffer(mode);
    if (framebuffer == NULL) { fprintf(stderr, "CACHE framebuffer allocation failed\n"); return 2; }
    framebuffer = MEM_K0_TO_K1(framebuffer);
    console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode); VIDEO_SetNextFramebuffer(framebuffer); VIDEO_SetBlack(false); VIDEO_Flush();
    VIDEO_WaitVSync(); if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    if (!fatInitDefault()) { fprintf(stderr, "CACHE SD unavailable\n"); return 2; }
    if (mkdir("sd:/halo-wii-memory", 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "CACHE log directory failed\n"); return 2;
    }
    report = fopen(CACHE_ADDRESS_REPORT_PATH, "a");
    if (report == NULL) { fprintf(stderr, "CACHE log open failed\n"); return 2; }
#endif
    if (fprintf(report, "BEGIN %s build=%s\n", CACHE_ADDRESS_REPORT_KIND, CACHE_PROBE_BUILD_ID) < 0 || fflush(report)) return 2;
#ifdef GEKKO
    arenas(report, "after_sd");
    struct mallinfo heap = mallinfo();
    fprintf(report, "HEAP after_sd arena=%d in_use=%d free=%d releasable_top=%d\n",
            heap.arena, heap.uordblks, heap.fordblks, heap.keepcost);
    fprintf(report, "CAPACITY native_state=%lu tag_reservation=%lu desktop_texture=%lu xbox_texture=%lu sound=%lu\n",
            (unsigned long)HALO_PORT_GAME_STATE_SIZE, (unsigned long)0x1600000,
            (unsigned long)HALO_PORT_TEXTURE_CACHE_SIZE, (unsigned long)0x1600000, (unsigned long)0x400000);
#endif
    int result = wii_cache_address_fixture(report, 1);
#ifdef CACHE_WIDGET_FIXTURE
    result |= wii_cache_widget_fixture(report, 1);
#endif
#ifdef CACHE_BSP_FIXTURE
    result |= wii_cache_bsp_fixture(report, 1);
#endif
#ifdef CACHE_MATERIAL_FIXTURE
    result |= cache_material_fixture(report, 1);
#endif
#if CACHE_PRIVATE_SIZE > 0
    result |= cache_address_owned(report, cache_private_bytes, CACHE_PRIVATE_SIZE, CACHE_PRIVATE_CRC,
                                  CACHE_PRIVATE_COUNT, CACHE_PRIVATE_TABLE_CRC);
#ifdef GEKKO
    result |= mem2_copy(report);
    arenas(report, "after_owned_release");
#endif
#endif
    if (fprintf(report, "END %s build=%s result=%d\n", CACHE_ADDRESS_REPORT_KIND, CACHE_PROBE_BUILD_ID, result) < 0 || fflush(report) || ferror(report)) return 2;
#ifdef GEKKO
    if (fclose(report)) return 2;
    printf("Cache address diagnostic: %d\n", result);
    for (unsigned i = 0; i < 60; ++i) VIDEO_WaitVSync();
#endif
    return result;
}
